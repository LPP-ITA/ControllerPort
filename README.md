
# 🚪 ControllerPort - Controle de Acesso IoT (ESP8266)

![Plataforma](https://img.shields.io/badge/plataforma-ESP8266-blue.svg)
![Status](https://img.shields.io/badge/status-produção-green.svg)

Firmware de nível industrial para controle de acesso físico no laboratório LPP-ITA. O sistema opera de forma híbrida (Online/Offline), garantindo abertura de portas mesmo sem conexão com o servidor, utilizando cache local sincronizado e configuração via Portal Captive.

## 🌟 Funcionalidades Principais

* **Operação Híbrida:**
    * **Online:** Valida permissões em tempo real via MQTT com o Backend Java.
    * **Offline:** Utiliza um cache local (in-memory) de tags autorizadas caso a rede caia.
* **Instalação Plug-and-Play (WiFiManager):**
    * Sem senhas *hardcoded*. Ao ligar pela primeira vez, cria uma rede WiFi `LPP_Porta_Setup` para configuração via celular.
* **Feedback Não-Bloqueante:**
    * Buzzer e LEDs operam com *multitasking* (sem `delay()`), garantindo que o processador nunca pare de ler cartões.
* **Resiliência:**
    * Watchdog de conexão WiFi e MQTT.
    * Sincronização automática de cache a cada 30 minutos.

## 🛠️ Especificações de Hardware

### Componentes
* **Microcontrolador:** ESP8266 (NodeMCU v2 ou Wemos D1 Mini)
* **Leitor RFID:** MFRC522 (13.56 MHz)
* **Atuador:** Módulo Relé 5V (para fechos eletromagnéticos ou eletroímãs)
* **Feedback:** Buzzer Ativo (5V/3.3V) e LEDs (Verde/Vermelho)

### 🔌 Pinagem (Wiring Diagram)

| Componente | Pino ESP8266 (Código) | GPIO | Função |
| :--- | :--- | :--- | :--- |
| **Relé** | `D1` | GPIO 5 | Acionamento da Porta |
| **Buzzer** | `D0` | GPIO 16 | Alerta Sonoro |
| **LED Verde** | `D2` | GPIO 4 | Status Sucesso/Online |
| **LED Vermelho** | `D3` | GPIO 0 | Status Erro/Offline |
| **RFID SDA (SS)** | `D8` | GPIO 15 | Chip Select SPI |
| **RFID SCK** | `D5` | GPIO 14 | Clock SPI |
| **RFID MOSI** | `D7` | GPIO 13 | Master Out Slave In |
| **RFID MISO** | `D6` | GPIO 12 | Master In Slave Out |
| **RFID RST** | `D4` | GPIO 2 | Reset |

> **Nota:** O pino `D3` (GPIO 0) deve estar em HIGH durante o boot. Garanta que o circuito do LED não force LOW na inicialização.

## 🚀 Como Instalar e Configurar

### 1. Compilação e Upload
Você pode usar a IDE do Arduino ou o Docker para compilar.

**Via Docker (Recomendado para CI/CD):**
```bash
# Construir a imagem
docker build -t lpp-controller-build .

# Compilar e extrair binários
docker run --rm -v $(pwd):/app lpp-controller-build
````

**Via Arduino IDE:**

1.  Instale o core **ESP8266** no Boards Manager.
2.  Instale as bibliotecas obrigatórias:
      * `MFRC522`
      * `ArduinoJson`
      * `AsyncMqttClient`
      * `WiFiManager` (por tzapu)
      * `Ticker`
3.  Selecione a placa **NodeMCU 1.0 (ESP-12E Module)** e faça o upload.

### 2\. Configuração Inicial (Primeiro Uso)

1.  Ligue o dispositivo.
2.  O sistema tentará conectar. Se falhar, o **LED Vermelho** piscará ou ficará aceso.
3.  Procure no seu celular a rede WiFi: **`LPP_Porta_Setup`**.
4.  Conecte-se (senha padrão se houver, ou aberta).
5.  O portal de configuração abrirá automaticamente (ou acesse `192.168.4.1`).
6.  Insira:
      * **SSID** da rede do laboratório.
      * **Senha** do WiFi.
      * **IP do Broker MQTT** (Servidor Java).
7.  Salve. O dispositivo reiniciará e conectará automaticamente.

## 📡 Protocolo MQTT

| Tópico | Direção | Descrição | Payload Exemplo |
| :--- | :--- | :--- | :--- |
| `ita-api/access/requests` | `Pub` | Envia UID lido para validação | `{"tagId": "A1B2C3D4", "portaId": "lab_01"}` |
| `ita-api/doors/{ID}/access` | `Sub` | Recebe decisão do servidor | `{"decision": "GRANTED"}` |
| `ita-api/doors/{ID}/cache/response` | `Sub` | Recebe lista de tags para offline | `["A1B2C3D4", "E5F6G7H8"]` |
| `ita-api/devices/{ID}/status` | `Pub` | Heartbeat e telemetria | `{"status": "online", "ip": "...", "cacheSize": 50}` |

## 🛡️ Segurança

  * O tráfego MQTT deve ser protegido via TLS (porta 8883) em produção.
  * O cache offline é volátil (RAM); reinicializações forçam uma nova sincronização segura com o servidor.

-----

**Desenvolvido para ITA - Laboratório de Pesquisa em Plasmas (LPP)**
