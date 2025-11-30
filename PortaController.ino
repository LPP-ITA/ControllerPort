#include <ESP8266WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <AsyncMqttClient.h>
#include <Ticker.h>
#include <EEPROM.h>
#include <set>

char WIFI_SSID[64] = "SUA_REDE_WIFI";
char WIFI_PASSWORD[64] = "SUA_SENHA_WIFI";

char MQTT_HOST[64] = "IP_DO_BROKER_MQTT";
int MQTT_PORT = 1883;

char PORTA_ID[32] = "porta_default_id";

#define EEPROM_SIZE 512
#define EEPROM_WIFI_SSID_ADDR    0
#define EEPROM_WIFI_PASS_ADDR    64
#define EEPROM_MQTT_SERVER_ADDR  128
#define EEPROM_PORTA_ID_ADDR     192

#define SS_PIN  D8
#define RST_PIN D4
#define RELAY_PIN D1
#define BUZZER_PIN D0
#define LED_GREEN_PIN D2
#define LED_RED_PIN D3

#define OPEN HIGH
#define CLOSE LOW
#define DOOR_OPEN_DURATION 3000
#define CACHE_UPDATE_INTERVAL 1800

MFRC522 mfrc522(SS_PIN, RST_PIN);
AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
Ticker wifiReconnectTimer;
Ticker cacheUpdateTimer;

String chipIdHex = String(ESP.getChipId(), HEX);
String clientId = "Porta-" + chipIdHex;

String topicAccessRequest;
String topicAccessResponse;
String topicCacheRequest;
String topicCacheResponse;
String topicDeviceStatus;

std::set<String> authorizedTagsCache;

unsigned long lastStatusMsg = 0;
const long statusInterval = 30000;

bool doorShouldOpen = false;
unsigned long doorOpenStartMillis = 0;

void loadConfigFromEEPROM();
void saveStringToEEPROM(int addrOffset, const String &strToWrite, int maxLen);
String readStringFromEEPROM(int addrOffset, int maxLen);
void setupWifi();
void connectToWifi();
void connectToMqtt();
void onWifiConnect(const WiFiEventStationModeGotIP& event);
void onWifiDisconnect(const WiFiEventStationModeDisconnected& event);
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttSubscribe(uint16_t packetId, uint8_t qos);
void onMqttUnsubscribe(uint16_t packetId);
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);
void onMqttPublish(uint16_t packetId);
void publishStatus();
void processTagRead(String uid);
void processTagReadOffline(String uid);
void openDoorBriefly();
void signalSuccess();
void signalFailure();
void signalOfflineSuccess();
void requestCacheUpdate();
void updateLocalCache(char* payload, size_t len);

void setup() {
    Serial.begin(115200);
    Serial.println("\n\nIniciando Controlador de Porta (Modo Híbrido)...");
    Serial.println("Chip ID: " + chipIdHex);

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, CLOSE);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    pinMode(LED_GREEN_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    pinMode(LED_RED_PIN, OUTPUT);
    digitalWrite(LED_RED_PIN, LOW);

    EEPROM.begin(EEPROM_SIZE);
    loadConfigFromEEPROM();

    String portaIdStr = String(PORTA_ID);
    topicAccessRequest = "ita-api/access/requests";
    topicAccessResponse = "ita-api/doors/" + portaIdStr + "/access";
    topicCacheRequest = "ita-api/doors/" + portaIdStr + "/cache/request";
    topicCacheResponse = "ita-api/doors/" + portaIdStr + "/cache/response";
    topicDeviceStatus = "ita-api/devices/" + clientId + "/status";

    Serial.println("ID da Porta configurado: " + portaIdStr);
    Serial.println("Tópico de Pedido de Acesso: " + topicAccessRequest);
    Serial.println("Tópico de Resposta de Acesso: " + topicAccessResponse);
    Serial.println("Tópico de Pedido de Cache: " + topicCacheRequest);
    Serial.println("Tópico de Resposta de Cache: " + topicCacheResponse);

    SPI.begin();
    mfrc522.PCD_Init();
    Serial.println("Leitor RFID iniciado.");

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onSubscribe(onMqttSubscribe);
    mqttClient.onUnsubscribe(onMqttUnsubscribe);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.onPublish(onMqttPublish);
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setClientId(clientId.c_str());

    wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);
    setupWifi();
    connectToWifi();

    cacheUpdateTimer.attach(CACHE_UPDATE_INTERVAL, requestCacheUpdate);
    Serial.println("Timer de atualização de cache (30 min) iniciado.");
}

void loop() {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        String uid = "";
        for (byte i = 0; i < mfrc522.uid.size; i++) {
            uid.concat(String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : ""));
            uid.concat(String(mfrc522.uid.uidByte[i], HEX));
        }
        uid.toUpperCase();
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        Serial.println("Tag lida: " + uid);
        processTagRead(uid);
    }

    if (doorShouldOpen && (millis() - doorOpenStartMillis >= DOOR_OPEN_DURATION)) {
        digitalWrite(RELAY_PIN, CLOSE);
        doorShouldOpen = false;
        Serial.println("Porta fechada.");
    }

    unsigned long now = millis();
    if (now - lastStatusMsg > statusInterval) {
        lastStatusMsg = now;
        publishStatus();
    }
}

void processTagRead(String uid) {
    if (mqttClient.connected()) {
        Serial.println("Modo ONLINE. Verificando tag com o backend...");

        StaticJsonDocument<256> doc;
        doc["tagId"] = uid;
        doc["portaId"] = PORTA_ID;

        String requestPayload;
        serializeJson(doc, requestPayload);

        Serial.println("Enviando pedido de acesso: " + requestPayload);

        uint16_t packetIdPub = mqttClient.publish(topicAccessRequest.c_str(), 1, false, requestPayload.c_str());

        if (packetIdPub == 0) {
            Serial.println("Falha ao enviar pedido MQTT. Tentando cache offline...");
            processTagReadOffline(uid);
        }
    } else {
        processTagReadOffline(uid);
    }
}

void processTagReadOffline(String uid) {
    Serial.println("Modo OFFLINE. Verificando cache local...");

    if (authorizedTagsCache.count(uid) > 0) {
        Serial.println("Tag encontrada no cache. Acesso OFFLINE permitido.");
        openDoorBriefly();
        signalOfflineSuccess();
    } else {
        Serial.println("Tag NÃO encontrada no cache. Acesso OFFLINE negado.");
        signalFailure();
    }
}

void openDoorBriefly() {
    Serial.println("Abrindo a porta...");
    digitalWrite(RELAY_PIN, OPEN);
    doorShouldOpen = true;
    doorOpenStartMillis = millis();
}


void signalSuccess() {
    digitalWrite(LED_GREEN_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
}

void signalOfflineSuccess() {
    digitalWrite(LED_GREEN_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
    digitalWrite(LED_GREEN_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("Sinal de sucesso OFFLINE");
}

void signalFailure() {
    digitalWrite(LED_RED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(500); // Beep longo
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
}

void setupWifi() {
    Serial.println("Configurando WiFi...");
    WiFi.mode(WIFI_STA);
}

void connectToWifi() {
    Serial.print("Conectando a ");
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
    Serial.print("WiFi Conectado! IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_RED_PIN, LOW);
    connectToMqtt();
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
    Serial.println("WiFi Desconectado.");
    digitalWrite(LED_RED_PIN, HIGH);
    mqttReconnectTimer.detach();
    wifiReconnectTimer.once(5, connectToWifi);
}

void connectToMqtt() {
    if (WiFi.isConnected()) {
        Serial.println("Conectando ao Broker MQTT...");
        mqttClient.connect();
    }
}

void onMqttConnect(bool sessionPresent) {
    Serial.println("Conectado ao MQTT.");
    Serial.print("Sessão Presente: "); Serial.println(sessionPresent);

    uint16_t packetIdSub1 = mqttClient.subscribe(topicAccessResponse.c_str(), 1);
    Serial.print("Inscrito em "); Serial.print(topicAccessResponse); Serial.print(" (packetId: "); Serial.println(packetIdSub1);

    uint16_t packetIdSub2 = mqttClient.subscribe(topicCacheResponse.c_str(), 1);
    Serial.print("Inscrito em "); Serial.print(topicCacheResponse); Serial.print(" (packetId: "); Serial.println(packetIdSub2);

    publishStatus();
    requestCacheUpdate();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    Serial.print("Desconectado do MQTT. Razão: ");
    Serial.println((int8_t)reason);

    if (WiFi.isConnected()) {
        mqttReconnectTimer.once(5, connectToMqtt);
    }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
    Serial.println("Inscrição MQTT confirmada.");
    Serial.print("  packetId: "); Serial.println(packetId);
    Serial.print("  qos: "); Serial.println(qos);
}

void onMqttUnsubscribe(uint16_t packetId) {
    Serial.println("Cancelamento de Inscrição MQTT confirmado.");
    Serial.print("  packetId: "); Serial.println(packetId);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    Serial.println("Mensagem MQTT recebida.");
    Serial.print("  Tópico: "); Serial.println(topic);

    if (String(topic) == topicAccessResponse) {
        Serial.println("  -> Mensagem de Resposta de Acesso");
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, payload, len);

        if (error) {
            Serial.print("Erro ao deserializar JSON da resposta de acesso: "); Serial.println(error.c_str());
            return;
        }
        const char* decision = doc["decision"];
        if (decision != nullptr) {
            if (strcmp(decision, "GRANTED") == 0) {
                openDoorBriefly();
                signalSuccess();
            } else if (strcmp(decision, "DENIED") == 0) {
                signalFailure();
            }
        }
    }
    else if (String(topic) == topicCacheResponse) {
        Serial.println("  -> Mensagem de Atualização de Cache");
        updateLocalCache(payload, len);
    }
    else {
        Serial.println("  -> Mensagem recebida em tópico inesperado.");
    }
}

void onMqttPublish(uint16_t packetId) {
    Serial.println("Publicação MQTT confirmada.");
    Serial.print("  packetId: "); Serial.println(packetId);
}

void publishStatus() {
     if (!mqttClient.connected()) {
        return;
    }

    StaticJsonDocument<256> doc;
    doc["deviceId"] = clientId;
    doc["portaId"] = PORTA_ID;
    doc["status"] = "online";
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["cacheSize"] = authorizedTagsCache.size();
    doc["heap"] = ESP.getFreeHeap();

    String statusPayload;
    serializeJson(doc, statusPayload);

    mqttClient.publish(topicDeviceStatus.c_str(), 0, true, statusPayload.c_str());
    Serial.println("Status publicado: " + statusPayload);
}

void requestCacheUpdate() {
    if (mqttClient.connected()) {
        Serial.println("Solicitando atualização de cache ao backend...");
        mqttClient.publish(topicCacheRequest.c_str(), 0, false, "");
    } else {
        Serial.println("Não é possível solicitar cache (MQTT desconectado).");
    }
}

void updateLocalCache(char* payload, size_t len) {
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, payload, len);

    if (error) {
        Serial.print("Erro ao deserializar JSON do cache: "); Serial.println(error.c_str());
        return;
    }

    JsonArray tagList = doc.as<JsonArray>();
    if (tagList.isNull()) {
         Serial.println("Erro: Payload do cache não é um array JSON.");
         return;
    }

    authorizedTagsCache.clear();
    Serial.println("Cache local limpo. Adicionando novas tags...");

    for (JsonVariant tag : tagList) {
        const char* uid = tag.as<const char*>();
        if (uid != nullptr) {
            authorizedTagsCache.insert(String(uid));
        }
    }

    Serial.print("Cache local atualizado. Total de tags autorizadas: ");
    Serial.println(authorizedTagsCache.size());
}


void loadConfigFromEEPROM() {
    String ssid_eeprom = readStringFromEEPROM(EEPROM_WIFI_SSID_ADDR, 64);
    String pass_eeprom = readStringFromEEPROM(EEPROM_WIFI_PASS_ADDR, 64);
    String mqtt_eeprom = readStringFromEEPROM(EEPROM_MQTT_SERVER_ADDR, 64);
    String porta_eeprom = readStringFromEEPROM(EEPROM_PORTA_ID_ADDR, 32);

    if (ssid_eeprom.length() > 0) ssid_eeprom.toCharArray(WIFI_SSID, 64);
    if (pass_eeprom.length() > 0) pass_eeprom.toCharArray(WIFI_PASSWORD, 64);
    if (mqtt_eeprom.length() > 0) mqtt_eeprom.toCharArray(MQTT_HOST, 64);
    if (porta_eeprom.length() > 0) porta_eeprom.toCharArray(PORTA_ID, 32);

    Serial.println("Configurações carregadas:");
    Serial.println("  SSID: " + String(WIFI_SSID));
    Serial.println("  MQTT Host: " + String(MQTT_HOST));
    Serial.println("  Porta ID: " + String(PORTA_ID));
}


void saveStringToEEPROM(int addrOffset, const String &strToWrite, int maxLen) {
    int len = strToWrite.length();
    if (len >= maxLen) len = maxLen - 1;

    for (int i = 0; i < len; i++) {
        EEPROM.write(addrOffset + i, strToWrite[i]);
    }
    EEPROM.write(addrOffset + len, '\0');

    for (int i = len + 1; i < maxLen; i++) {
        EEPROM.write(addrOffset + i, 0);
    }
    if (!EEPROM.commit()) {
      Serial.println("ERRO: Falha ao comitar EEPROM.");
    } else {
      Serial.println("Configuração salva na EEPROM.");
    }
}

String readStringFromEEPROM(int addrOffset, int maxLen) {
    char data[maxLen];
    int len = 0;
    unsigned char k;
    k = EEPROM.read(addrOffset);
    while (k != '\0' && len < (maxLen -1)) {
        data[len] = k;
        len++;
        k = EEPROM.read(addrOffset + len);
    }
    data[len] = '\0';
    return String(data);
}