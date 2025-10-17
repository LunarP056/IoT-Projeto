/*
 * PROJETO: Monitoramento de Ambiente com ESP32 (Luminosidade e Distância)
 * FUNÇÃO: Coletar dados de sensores e enviar via HTTP POST para uma planilha Google Sheets (Apps Script).
 * DEPENDÊNCIAS: Core ESP32, WiFiManager, BH1750.
 */

// ====================================================================
// INCLUSÃO DE BIBLIOTECAS
// ====================================================================

#include <WiFi.h>        // Biblioteca nativa para funcionalidades Wi-Fi do ESP32.
#include <WiFiManager.h> // Gerenciador para configurar o Wi-Fi sem codificar SSID/Senha.
#include <time.h>        // Biblioteca para sincronização de tempo (NTP) e timestamps.
#include <Wire.h>        // Biblioteca para comunicação I2C (necessária para o sensor BH1750).
#include <BH1750.h>      // Driver para o sensor de luminosidade BH1750.
#include <HTTPClient.h>  // Biblioteca para fazer requisições HTTP (enviar dados para a web).

// ====================================================================
// CONFIGURAÇÕES GLOBAIS (EDITÁVEIS)
// ====================================================================

// URL do Google Apps Script para receber os dados via POST.
const char* SHEETS_URL = "https://script.google.com/macros/s/AKfycbwX7wyOwjaBb6jI69mneSN4QnMQWn2Ix1to62z9skojNqEPoPgCXsgVVnE8Y-0PvbDN/exec"; 

// ID único para identificar este dispositivo na planilha.
const char* DEVICE_ID = "PC_USER_01"; 

// Configurações do NTP (Network Time Protocol) para sincronizar o relógio do ESP32.
const char* ntpServer = "pool.ntp.org";      // Servidor NTP global.
const long gmtOffset_sec = -10800;           // Offset de GMT em segundos (Ex: -3 horas para Brasília: -3 * 3600 = -10800).
const int daylightOffset_sec = 0;            // Ajuste para Horário de Verão (0 se não usado).

// ====================================================================
// VARIÁVEIS DE HARDWARE E ESTADO
// ====================================================================

// Cria um objeto para o sensor BH1750.
BH1750 lightmeter;

// Pinos GPIO do ESP32 para o sensor ultrassônico HC-SR04.
const int trigPin = 12; // Pino Trigger (OUTPUT) - Envia o pulso sonoro.
const int echoPin = 14; // Pino Echo (INPUT) - Recebe o retorno do pulso sonoro.

// Flag para garantir que o sensor BH1750 seja inicializado apenas uma vez.
bool bh1750_iniciado = false; 

// Objeto cliente WiFi (usado internamente pela HTTPClient, mas bom declarar).
WiFiClient espClient; 

// ====================================================================
// FUNÇÃO: setup_wifi_manager()
// ====================================================================
// Inicia o WiFiManager. Se não estiver configurado, cria um hotspot
// para que o usuário possa configurar o Wi-Fi.

void setup_wifi_manager() {
    WiFiManager wm; // Cria uma instância do WiFiManager.

    Serial.println("Iniciando WiFiManager...");

    // Tenta conectar automaticamente. Se falhar, cria o Access Point (AP) "ESP Setup".
    if (!wm.autoConnect("ESP Setup", "12345678")) {
        Serial.println("Falha na conexão Wi-Fi. Reiniciando em 3 segundos...");
        delay(3000);
        ESP.restart(); // Reinicia o ESP32 se a conexão falhar.
    } 

    Serial.println("WiFi Conectado!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Configura e sincroniza o relógio interno do ESP32 via NTP.
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Tempo sincronizado via NTP.");
}

// ====================================================================
// FUNÇÃO: get_current_timestamp()
// ====================================================================
// Retorna o timestamp Unix (segundos desde 1970) após a sincronização NTP.

long get_current_timestamp() {
    time_t now;      // Variável para o timestamp Unix.
    struct tm timeinfo; // Estrutura para informações de data/hora.
    
    // Tenta obter a hora local sincronizada.
    if (!getLocalTime(&timeinfo)) {
        return 0; // Retorna 0 se falhar na obtenção do tempo.
    }
    
    time(&now); // Converte o struct tm para timestamp.
    return now;
}

// ====================================================================
// FUNÇÃO: proximidade()
// ====================================================================
// Realiza a leitura do sensor ultrassônico HC-SR04 e retorna a distância em cm.

float proximidade() {
    // 1. Limpa o pino TRIGGER (garante LOW por 2µs).
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    
    // 2. Envia um pulso alto de 10µs no TRIGGER (dispara o som).
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    // 3. Mede a duração do pulso de retorno no pino ECHO (tempo que o som levou para ir e voltar).
    unsigned long duration = pulseIn(echoPin, HIGH);
    
    // 4. Calcula a distância: (duração * velocidade do som em cm/µs) / 2
    // Velocidade do som (343 m/s) = 0.0343 cm/µs.
    float distance = (duration * 0.0343) / 2;
    
    return distance; // Distância em centímetros.
}

// ====================================================================
// FUNÇÃO: luminosidade()
// ====================================================================
// Realiza a leitura do sensor BH1750 e retorna o nível de luz em Lux.

float luminosidade() {
    // Inicializa o sensor APENAS na primeira vez que a função é chamada.
    if (!bh1750_iniciado) {
        // Tenta iniciar a comunicação I2C com o modo de alta resolução contínua.
        if (lightmeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
            Serial.println("BH1750 Pronto!");
            bh1750_iniciado = true;
        } else {
            // Falha na inicialização do sensor.
            Serial.println("ERRO: BH1750 não encontrado.");
            return -3.00; // Valor de erro para ser ignorado no loop principal.
        }
    }
    
    // Retorna a leitura do nível de luz em Lux.
    return lightmeter.readLightLevel();
}

// ====================================================================
// FUNÇÃO: enviar_dados_sheets()
// ====================================================================
// Monta o JSON com os dados e envia via HTTP POST para o Apps Script.

void enviar_dados_sheets(float distance, float lux) {
    // Verifica a conexão Wi-Fi antes de tentar o envio.
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi não conectado. Pulando envio.");
        return;
    }

    HTTPClient http;
    http.begin(SHEETS_URL); // Inicia a conexão HTTP com o URL do Google Sheets.
    http.addHeader("Content-Type", "application/json"); // Define o cabeçalho para JSON.

    char payload[256]; // Buffer para armazenar a string JSON.
    
    char dist_str[10];
    char lux_str[10];
    // Converte floats para strings com 2 casas decimais.
    dtostrf(distance, 4, 2, dist_str); 
    dtostrf(lux, 4, 2, lux_str);

    // Monta a string JSON no formato {"id":"PC_USER_01","distance":XX.XX,"lux":YY.YY}
    snprintf(payload, sizeof(payload), 
             "{\"id\":\"%s\",\"distance\":%s,\"lux\":%s}", 
             DEVICE_ID, dist_str, lux_str);
    
    Serial.print("Enviando JSON: ");
    Serial.println(payload);

    // Realiza o POST e armazena o código de resposta (ex: 200 para sucesso).
    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0) {
        Serial.print("Dados enviados com sucesso. Código HTTP: ");
        Serial.println(httpResponseCode);
        String response = http.getString();
        Serial.print("Resposta do Servidor: ");
        Serial.println(response); // Imprime a resposta do Apps Script (geralmente "Success").
    } else {
        Serial.print("Falha ao enviar dados. Erro HTTP: ");
        Serial.println(httpResponseCode);
    }
    
    http.end(); // Fecha a conexão HTTP para liberar recursos.
}


// ====================================================================
// FUNÇÃO: setup()
// ====================================================================
// Executada uma única vez ao iniciar a placa.

void setup() {
    Serial.begin(9600); // Inicia a comunicação serial para debug (Monitor Serial).
    
    // Configuração dos pinos do sensor ultrassônico.
    pinMode(trigPin, OUTPUT);
    pinMode(echoPin, INPUT); 

    // Configura o Wi-Fi e sincroniza o tempo via NTP.
    setup_wifi_manager(); 

    // Inicializa a comunicação I2C.
    // Padrão do ESP32 DevKit: SDA=21, SCL=22. Ajuste se usar pinos diferentes.
    Wire.begin(21, 22); 
    delay(200); // Pequena pausa para estabilização do I2C.
}

// ====================================================================
// FUNÇÃO: loop()
// ====================================================================
// Executada repetidamente após a função setup().

void loop() {
    // 1. Coleta dos dados dos sensores
    float distance = proximidade();
    float lux = luminosidade();
    long timestamp = get_current_timestamp(); // Pega o timestamp atual (não usado no payload, mas útil para debug).

    // 2. Verifica se a leitura de luz é válida
    if (lux < 0) {
        Serial.println("AVISO: Leitura de Lux inválida. Pulando envio.");
        delay(30000); // Espera o tempo normal de loop antes de tentar novamente.
        return; 
    }

    // 3. Envia os dados
    enviar_dados_sheets(distance, lux);
    
    // 4. Pausa antes da próxima leitura/envio
    delay(30000); // Aguarda 30 segundos antes de executar o loop novamente.
}
