# Usa Ubuntu como base
FROM ubuntu:22.04

LABEL maintainer="LPP-ITA Engineering Team"

# 1. Instalar dependências
RUN apt-get update && apt-get install -y \
    curl \
    git \
    python3 \
    python3-serial \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 2. Instalar Arduino CLI
RUN curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh

# 3. Configurar Core ESP8266
RUN arduino-cli config init
RUN arduino-cli core update-index --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
RUN arduino-cli core install esp8266:esp8266 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json

# 4. Instalar Bibliotecas
RUN arduino-cli lib install "MFRC522" "ArduinoJson" "WiFiManager"
WORKDIR /root/Arduino/libraries
RUN git clone https://github.com/me-no-dev/ESPAsyncTCP.git
RUN git clone https://github.com/marvinroger/async-mqtt-client.git

# 5. Configurar Diretório de Trabalho (Raiz neutra)
WORKDIR /app

# 6. Comando de Compilação Ajustado
# Espera que o código seja montado em /app/PortaController para satisfazer a regra do Arduino
CMD ["arduino-cli", "compile", "--fqbn", "esp8266:esp8266:nodemcuv2", "--output-dir", "/app/PortaController/build", "/app/PortaController/PortaController.ino"]