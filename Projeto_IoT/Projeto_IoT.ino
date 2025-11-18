/*
 * PROJETO: Logger IoT para ThingSpeak (Canal Único, 3 Fields)
 * FUNÇÃO: Coletar dados de sensores (HC-SR04 e BH1750), fazer uma média de 5 leituras
 * a cada 15 segundos, e enviar os resultados junto com o MAC Address para o ThingSpeak:
 * Field 1: Média de Distância (cm)
 * Field 2: Média de Luminosidade (Lux)
 * Field 3: MAC Address do Dispositivo
 */

// ====================================================================
// INCLUSÃO DE BIBLIOTECAS
// ====================================================================

#include <WiFi.h>          // Biblioteca principal para gerenciamento de Wi-Fi.
#include <WiFiManager.h>   // Biblioteca de Provisionamento Simplificado (Portal Cativo).
#include <time.h>          // Biblioteca para sincronização de tempo (NTP).
#include <Wire.h>          // Biblioteca para comunicação I2C (usada pelo BH1750).
#include <BH1750.h>        // Biblioteca para o sensor de luminosidade digital BH1750.
#include <HTTPClient.h>    // Cliente HTTP para fazer requisições GET para o ThingSpeak.
#include <Arduino.h>       // Inclui funções básicas do Arduino.

// ====================================================================
// CONFIGURAÇÕES DO THINGSPEAK (PREENCHA AQUI!)
// ====================================================================

const char* THINGSPEAK_API_HOST = "http://api.thingspeak.com/update"; 
const char* THINGSPEAK_CHANNEL_ID = "3166584"; 
const char* THINGSPEAK_WRITE_API_KEY = "2RY2FMBN3TFXTYYM"; 

const int FIELD_DISTANCIA = 1;       
const int FIELD_LUZ = 2;             
const int FIELD_MAC_ADDRESS = 3;     

// ====================================================================
// CONFIGURAÇÕES GLOBAIS E VARIÁVEIS DE CONEXÃO
// ====================================================================

// --- Configurações NTP (Network Time Protocol) ---
const char* ntpServer = "pool.ntp.org"; 
const long gmtOffset_sec = -10800;      
const int daylightOffset_sec = 0;       

// --- Configurações de Hardware (Pinos GPIO) ---
const int trigPin = 12; // Pino Trigger do sensor ultrassônico HC-SR04.
const int echoPin = 14; // Pino Echo do sensor ultrassônico HC-SR04.
const int I2C_SDA = 21; // Pino SDA (Dados) para comunicação I2C.
const int I2C_SCL = 22; // Pino SCL (Clock) para comunicação I2C.

// --- Filtros de Sanidade para o HC-SR04 ---
const float MAX_DISTANCE_CM = 350.0;     // Distância máxima plausível para o HC-SR04
const float MIN_VALID_DISTANCE_CM = 2.0; // Distância mínima (abaixo disso é ruído/suporte/Zona Cega)

// ====================================================================
// VARIÁVEIS DE CÁLCULO E TEMPORIZAÇÃO (Lógica de Média)
// ====================================================================

const long LEITURA_INTERVALO_MS = 15000; 
const int NUM_AMOSTRAS = 5;
const long COLETA_INTERVALO_MS = LEITURA_INTERVALO_MS / NUM_AMOSTRAS; 

unsigned long ultimoTempoColeta = 0; 
float somaDistancia = 0.0;          
float somaLux = 0.0;                
int contadorAmostras = 0;           

// ====================================================================
// VARIÁVEIS DE IDENTIFICAÇÃO DO DISPOSITIVO
// ====================================================================

String deviceMacAddress = ""; 

// ====================================================================
// OBJETOS DE HARDWARE
// ====================================================================

BH1750 lightmeter; 

// ====================================================================
// FUNÇÃO: setup_wifi_manager()
// ====================================================================

/**
 * @brief Gerencia a conexão Wi-Fi utilizando o WiFiManager.
 */
void setup_wifi_manager() {
    WiFiManager wm; 
    Serial.println("Iniciando WiFiManager...");
    if (!wm.autoConnect("ESP Setup", "12345678")) {
        Serial.println("Falha na conexão Wi-Fi. Reiniciando em 3 segundos...");
        delay(3000);
        ESP.restart(); 
    }
    Serial.println("WiFi Conectado!");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Tempo sincronizado via NTP.");
}

// ====================================================================
// FUNÇÕES DE UTILIDADE E SENSORES
// ====================================================================

/**
 * @brief Calcula a distância em centímetros usando o HC-SR04 (Ultrassônico) com filtro.
 * @return float Distância calculada em centímetros (cm). Retorna -1.0 se inválida.
 */
float proximidade() {
    // 1. Limpa o TrigPin (garante pulso baixo por 2µs)
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    // 2. Envia um pulso alto (Trigger) por 10µs
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    // 3. Desliga o pulso
    digitalWrite(trigPin, LOW);

    // 4. Mede a duração do pulso de retorno (Echo)
    // Timeout de 30ms para evitar travamento em caso de erro ou longo alcance.
    unsigned long duration = pulseIn(echoPin, HIGH, 30000); 
    
    // 5. Calcula a distância (d = t * v_som / 2)
    float distance = (duration * 0.0343) / 2; 

    // --- FILTRAGEM DE SANIDADE (Zona Cega e Limite Máximo) ---
    if (distance > MAX_DISTANCE_CM || distance < MIN_VALID_DISTANCE_CM) {
        // Se a duração for 0 (timeout) ou estiver fora da faixa plausível
        return -1.0; // Valor de erro para descarte
    }

    return distance; 
}

/**
 * @brief Lê o nível de luminosidade em Lux do BH1750.
 * @return float Luminosidade em Lux. Retorna -2.0 em caso de erro na leitura.
 */
float luminosidade() {
    // A função retorna -1.0 internamente se a medição não estiver pronta.
    float lux = lightmeter.readLightLevel(); 
    
    if (lux < 0.0) {
        // Se a leitura falhou ou está indisponível, retornamos um código de erro
        // diferente para podermos distinguir do filtro de distância.
        return -2.0; 
    }
    return lux; 
}

// ====================================================================
// FUNÇÃO: enviar_dados_thingspeak()
// ====================================================================

/**
 * @brief Envia os dados de distância (média), luz (média) e MAC Address em uma ÚNICA requisição.
 */
void enviar_dados_thingspeak(float distance, float lux, const String& macAddress) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERRO: Wi-Fi desconectado. Não foi possível enviar.");
        return; 
    }

    HTTPClient http; 
    char urlBuffer[300]; 

    // Cria a string da URL com todos os campos formatados.
    snprintf(urlBuffer, sizeof(urlBuffer),
              "%s?api_key=%s&field%d=%.2f&field%d=%.2f&field%d=%s",
              THINGSPEAK_API_HOST,
              THINGSPEAK_WRITE_API_KEY,
              FIELD_DISTANCIA,
              distance, 
              FIELD_LUZ,
              lux, 
              FIELD_MAC_ADDRESS,
              macAddress.c_str() 
              );

    Serial.printf("Enviando Média Dist (%.2f cm), Média Lux (%.2f lx) e MAC (%s)...\n", 
                  distance, lux, macAddress.c_str());

    http.begin(urlBuffer); 

    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.printf("Envio bem-sucedido. Código HTTP: %d (Entry ID: %s)\n", httpResponseCode, response.c_str());
    } else {
        Serial.printf("Falha ao enviar dados. Erro HTTP: %d\n", httpResponseCode);
        Serial.printf("Mensagem de Erro: %s\n", http.errorToString(httpResponseCode).c_str());
    }

    http.end(); 
}


// ====================================================================
// FUNÇÃO: setup() - Configuração Inicial
// ====================================================================

void setup() {
    Serial.begin(9600); 
    Serial.println("\n--- Inicializando ESP32 (Logger ThingSpeak com Média de Amostras) ---");
    Serial.setDebugOutput(true); 

    // 1. Configuração dos pinos do sensor ultrassônico.
    pinMode(trigPin, OUTPUT); 
    pinMode(echoPin, INPUT);  

    // 2. Configura a conexão Wi-Fi.
    setup_wifi_manager();
    
    // 2.1 Captura o MAC Address para identificação
    deviceMacAddress = WiFi.macAddress();
    Serial.printf("MAC Address do Dispositivo: %s\n", deviceMacAddress.c_str());

    // 3. Inicializa a comunicação I2C e o sensor BH1750.
    Wire.begin(I2C_SDA, I2C_SCL); 
    delay(200); 
    
    if (lightmeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
        Serial.println("BH1750 Pronto!");
    } else {
        Serial.println("ERRO: BH1750 não encontrado! A leitura de Lux será inválida.");
    }
}

// ====================================================================
// FUNÇÃO: loop() - Loop Principal de Execução
// ====================================================================

void loop() {
    // Checa a conexão Wi-Fi periodicamente e tenta reconectar.
    if (WiFi.status() != WL_CONNECTED) {
        if(millis() % 5000 < 50){
            WiFi.reconnect();
        }
        delay(100);
        return; 
    }

    unsigned long tempoAtual = millis();

    // ----------------------------------------------------
    // LÓGICA DE COLETA (COLETA_INTERVALO_MS = 3 SEGUNDOS)
    // ----------------------------------------------------
    if (tempoAtual - ultimoTempoColeta >= COLETA_INTERVALO_MS) {
        ultimoTempoColeta = tempoAtual; 

        float distancia = proximidade();
        float lux = luminosidade();
        
        // Verifica se ambas as leituras estão dentro da faixa de plausibilidade.
        bool leituraValida = (distancia != -1.0) && (lux != -2.0);

        if (leituraValida) {
            // Se ambos os sensores retornaram valores plausíveis, acumulamos.
            somaDistancia += distancia;
            somaLux += lux;
            contadorAmostras++;
            Serial.printf("[Coleta %d/%d] Válida. Distância: %.2f cm | Luminosidade: %.2f lx\n", 
                          contadorAmostras, NUM_AMOSTRAS, distancia, lux);
        } else {
            // Log detalhado de por que a amostra foi descartada.
            if (distancia == -1.0) {
                Serial.println("❌ Amostra descartada: Distância fora da faixa válida (2cm a 350cm).");
            }
            if (lux == -2.0) {
                Serial.println("❌ Amostra descartada: Falha na leitura BH1750 (Sensor não pronto ou erro I2C).");
            }
        }
    }
    
    // ----------------------------------------------------
    // LÓGICA DE ENVIO (QUANDO NUM_AMOSTRAS = 5 ESTIVEREM PRONTAS)
    // ----------------------------------------------------
    if (contadorAmostras >= NUM_AMOSTRAS) {
        
        float mediaDistancia = somaDistancia / NUM_AMOSTRAS;
        float mediaLux = somaLux / NUM_AMOSTRAS;

        Serial.printf("\n--- ENVIO THINGSPEAK (Média de %d leituras, total 15s) ---\n", NUM_AMOSTRAS);
        Serial.printf("Média de Envio: Distância=%.2f cm | Luz=%.2f lx\n", mediaDistancia, mediaLux);

        enviar_dados_thingspeak(mediaDistancia, mediaLux, deviceMacAddress); 

        // Reinicia os acumuladores e o contador para o próximo ciclo de 15s
        somaDistancia = 0.0;
        somaLux = 0.0;
        contadorAmostras = 0;

        Serial.println("------------------------------------------\n");
    }

    delay(10);
}
