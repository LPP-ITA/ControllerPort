# Imagem base oficial do Arduino CLI
FROM arduino/arduino-cli:latest

# Metadados do Projeto
LABEL maintainer="ITA LPP Team"
LABEL description="Ambiente de Build para o ControllerPort (ESP8266)"

# 1. Configurar o Core do ESP8266
# Adiciona a URL do gerenciador de placas e instala o core do ESP8266
RUN arduino-cli core update-index --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json && \
    arduino-cli core install esp8266:esp8266 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json

# 2. Instalar Bibliotecas Dependências
# Instala as libs exatas usadas no código refatorado
RUN arduino-cli lib install \
    "MFRC522" \
    "ArduinoJson" \
    "AsyncMqttClient" \
    "WiFiManager" \
    "Ticker"

# 3. Configurar Diretório de Trabalho
WORKDIR /app

# Copia todo o código fonte para dentro do container
COPY . .

# 4. Comando Padrão de Compilação
# Compila para NodeMCU v2 (ajuste o FQBN se usar outra placa, ex: d1_mini)
CMD ["arduino-cli", "compile", "--fqbn", "esp8266:esp8266:nodemcuv2", "--output-dir", "build", "."]