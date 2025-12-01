#include <ESP8266WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <AsyncMqttClient.h>
#include <Ticker.h>
#include <EEPROM.h>
#include <set>
#include <WiFiManager.h> // Instalar via Library Manager: "WiFiManager by tzapu"

// --- Definições de Hardware ---
#define SS_PIN  D8
#define RST_PIN D4
#define RELAY_PIN D1
#define BUZZER_PIN D0
#define LED_GREEN_PIN D2
#define LED_RED_PIN D3

#define OPEN HIGH
#define CLOSE LOW
#define DOOR_OPEN_DURATION 3000
#define CACHE_UPDATE_INTERVAL 1800 // 30 minutos

// --- Identificação ---
char PORTA_ID[32] = "lab_lpp_01"; // Default, pode ser salvo na EEPROM customizada se desejar

// --- Objetos Globais ---
MFRC522 mfrc522(SS_PIN, RST_PIN);
AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
Ticker wifiReconnectTimer;
Ticker cacheUpdateTimer;

String chipIdHex = String(ESP.getChipId(), HEX);
String clientId = "Porta-" + chipIdHex;

// --- Tópicos MQTT ---
String topicAccessRequest;
String topicAccessResponse;
String topicCacheRequest;
String topicCacheResponse;
String topicDeviceStatus;

// --- Cache Local ---
std::set<String> authorizedTagsCache;

// --- Variáveis de Controle (Non-blocking) ---
unsigned long lastStatusMsg = 0;
const long statusInterval = 30000;

bool doorShouldOpen = false;
unsigned long doorOpenStartMillis = 0;

// Controle de Feedback (Buzzer/LED) sem delay
unsigned long feedbackTimer = 0;
bool feedbackActive = false;
int feedbackType = 0; // 1=Sucesso, 2=Erro, 3=Offline

// --- Protótipos ---
void connectToMqtt();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);
void processTagRead(String uid);
void processTagReadOffline(String uid);
void openDoorBriefly();
void triggerFeedback(int type);
void handleFeedbackLoop();
void publishStatus();
void requestCacheUpdate();
void updateLocalCache(char* payload, size_t len);

void setup() {
    Serial.begin(115200);
    
    // Configuração de Pinos
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, CLOSE);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    pinMode(LED_GREEN_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    pinMode(LED_RED_PIN, OUTPUT);
    digitalWrite(LED_RED_PIN, LOW);

    // Inicializa RFID
    SPI.begin();
    mfrc522.PCD_Init();

    // --- Configuração WiFi via Portal (Profissional) ---
    WiFiManager wifiManager;
    // wifiManager.resetSettings(); // Descomente para resetar e testar
    wifiManager.autoConnect("LPP_Porta_Setup"); 
    Serial.println("WiFi Conectado!");

    // Configuração MQTT (Pode ser melhorado com parâmetros customizados do WiFiManager)
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.setServer("IP_DO_BROKER", 1883); // Ideal: Usar WiFiManager parameters para isso
    mqttClient.setClientId(clientId.c_str());
    
    connectToMqtt();

    // Definição de Tópicos Dinâmicos
    String portaIdStr = String(PORTA_ID);
    topicAccessRequest = "ita-api/access/requests";
    topicAccessResponse = "ita-api/doors/" + portaIdStr + "/access";
    topicCacheRequest = "ita-api/doors/" + portaIdStr + "/cache/request";
    topicCacheResponse = "ita-api/doors/" + portaIdStr + "/cache/response";
    topicDeviceStatus = "ita-api/devices/" + clientId + "/status";

    cacheUpdateTimer.attach(CACHE_UPDATE_INTERVAL, requestCacheUpdate);
}

void loop() {
    // 1. Manutenção do Feedback (Piscar LEDs/Tocar Buzzer sem travar CPU)
    handleFeedbackLoop();

    // 2. Controle da Porta (Relé)
    if (doorShouldOpen && (millis() - doorOpenStartMillis >= DOOR_OPEN_DURATION)) {
        digitalWrite(RELAY_PIN, CLOSE);
        doorShouldOpen = false;
        Serial.println("Porta fechada.");
    }

    // 3. Heartbeat Status
    if (millis() - lastStatusMsg > statusInterval) {
        lastStatusMsg = millis();
        publishStatus();
    }

    // 4. Leitura RFID (Só se não estiver processando feedback crítico)
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
        return;
    }

    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
        uid.concat(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
        uid.concat(String(mfrc522.uid.uidByte[i], HEX));
    }
    uid.toUpperCase();
    
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    Serial.println("Tag: " + uid);
    processTagRead(uid);
}

// --- Lógica Não Bloqueante de Feedback ---
void triggerFeedback(int type) {
    feedbackType = type;
    feedbackTimer = millis();
    feedbackActive = true;
    
    // Estado Inicial
    if (type == 1) { // Sucesso
        digitalWrite(LED_GREEN_PIN, HIGH);
        digitalWrite(BUZZER_PIN, HIGH);
    } else if (type == 2) { // Erro
        digitalWrite(LED_RED_PIN, HIGH);
        digitalWrite(BUZZER_PIN, HIGH);
    }
}

void handleFeedbackLoop() {
    if (!feedbackActive) return;
    unsigned long elapsed = millis() - feedbackTimer;

    if (feedbackType == 1) { // Sucesso (Bip curto 200ms)
        if (elapsed > 200) {
            digitalWrite(LED_GREEN_PIN, LOW);
            digitalWrite(BUZZER_PIN, LOW);
            feedbackActive = false;
        }
    } 
    else if (feedbackType == 2) { // Erro (Bip longo 1s)
        if (elapsed > 1000) {
            digitalWrite(LED_RED_PIN, LOW);
            digitalWrite(BUZZER_PIN, LOW);
            feedbackActive = false;
        }
    }
}

// --- Lógica de Negócio ---
void processTagRead(String uid) {
    if (mqttClient.connected()) {
        StaticJsonDocument<256> doc;
        doc["tagId"] = uid;
        doc["portaId"] = PORTA_ID;
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(topicAccessRequest.c_str(), 1, false, payload.c_str());
    } else {
        processTagReadOffline(uid);
    }
}

void processTagReadOffline(String uid) {
    if (authorizedTagsCache.count(uid) > 0) {
        openDoorBriefly();
        triggerFeedback(1);
    } else {
        triggerFeedback(2);
    }
}

void openDoorBriefly() {
    digitalWrite(RELAY_PIN, OPEN);
    doorShouldOpen = true;
    doorOpenStartMillis = millis();
}

// --- MQTT Callbacks ---
void connectToMqtt() {
    mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
    Serial.println("MQTT Conectado.");
    mqttClient.subscribe(topicAccessResponse.c_str(), 1);
    mqttClient.subscribe(topicCacheResponse.c_str(), 1);
    requestCacheUpdate();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    Serial.println("MQTT Desconectado. Reconectando...");
    if (WiFi.isConnected()) {
        mqttReconnectTimer.once(2, connectToMqtt);
    }
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    String topicStr = String(topic);
    
    if (topicStr == topicAccessResponse) {
        StaticJsonDocument<256> doc;
        deserializeJson(doc, payload, len);
        const char* decision = doc["decision"];
        if (strcmp(decision, "GRANTED") == 0) {
            openDoorBriefly();
            triggerFeedback(1);
        } else {
            triggerFeedback(2);
        }
    } 
    else if (topicStr == topicCacheResponse) {
        updateLocalCache(payload, len);
    }
}

void requestCacheUpdate() {
    if (mqttClient.connected()) {
        mqttClient.publish(topicCacheRequest.c_str(), 0, false, "");
    }
}

void updateLocalCache(char* payload, size_t len) {
    DynamicJsonDocument doc(8192); // Aumentar buffer para listas grandes
    deserializeJson(doc, payload, len);
    JsonArray tagList = doc.as<JsonArray>();
    
    authorizedTagsCache.clear();
    for (JsonVariant tag : tagList) {
        authorizedTagsCache.insert(tag.as<String>());
    }
    Serial.print("Cache atualizado: "); Serial.println(authorizedTagsCache.size());
}

void publishStatus() {
    if (!mqttClient.connected()) return;
    StaticJsonDocument<256> doc;
    doc["deviceId"] = clientId;
    doc["status"] = "online";
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["cacheSize"] = authorizedTagsCache.size();
    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(topicDeviceStatus.c_str(), 0, true, payload.c_str());
}