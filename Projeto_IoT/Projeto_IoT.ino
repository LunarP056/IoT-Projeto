#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <Wire.h>
#include <BH1750.h>
#include <HTTPClient.h>

const char* SHEETS_URL = "https://script.google.com/macros/s/AKfycbwX7wyOwjaBb6jI69mneSN4QnMQWn2Ix1to62z9skojNqEPoPgCXsgVVnE8Y-0PvbDN/exec"; 

const char* DEVICE_ID = "PC_USER_01"; 

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800; 
const int daylightOffset_sec = 0;

BH1750 lightmeter;
const int trigPin = 12;
const int echoPin = 14;
bool bh1750_iniciado = false; 

WiFiClient espClient; 

void setup_wifi_manager() {
    WiFiManager wm;

    Serial.println("Iniciando WiFiManager...");

    if (!wm.autoConnect("ESP Setup", "12345678")) {
        Serial.println("Falha na conexão Wi-Fi. Reiniciando em 3 segundos...");
        delay(3000);
        ESP.restart();
    } 

    Serial.println("WiFi Conectado!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Tempo sincronizado via NTP.");
}

long get_current_timestamp() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return 0; 
    }
    time(&now);
    return now;
}

float proximidade() {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    unsigned long duration = pulseIn(echoPin, HIGH);
    float distance = (duration * 0.0343) / 2;
    return distance;
}

float luminosidade() {
    if (!bh1750_iniciado) {
        if (lightmeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
            Serial.println("BH1750 Pronto!");
            bh1750_iniciado = true;
        } else {
            return -3.00; 
        }
    }
    return lightmeter.readLightLevel();
}

void enviar_dados_sheets(float distance, float lux) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi não conectado. Pulando envio.");
        return;
    }

    HTTPClient http;
    http.begin(SHEETS_URL);
    http.addHeader("Content-Type", "application/json");

    char payload[256];
    
    char dist_str[10];
    char lux_str[10];
    dtostrf(distance, 4, 2, dist_str); 
    dtostrf(lux, 4, 2, lux_str);

    snprintf(payload, sizeof(payload), 
             "{\"id\":\"%s\",\"distance\":%s,\"lux\":%s}", 
             DEVICE_ID, dist_str, lux_str);
    
    Serial.print("Enviando JSON: ");
    Serial.println(payload);

    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0) {
        Serial.print("Dados enviados com sucesso. Código HTTP: ");
        Serial.println(httpResponseCode);
        String response = http.getString();
        Serial.print("Resposta do Servidor: ");
        Serial.println(response);
    } else {
        Serial.print("Falha ao enviar dados. Erro HTTP: ");
        Serial.println(httpResponseCode);
    }
    
    http.end();
}


void setup() {
    Serial.begin(9600);
    pinMode(trigPin, OUTPUT);
    pinMode(echoPin, INPUT); // <--- LINHA FINALIZADA

    setup_wifi_manager(); 

    Wire.begin(21, 22); 
    delay(200);
} // <--- CHAVE DE FECHAMENTO ADICIONADA

void loop() {
    float distance = proximidade();
    float lux = luminosidade();
    long timestamp = get_current_timestamp(); 

    if (lux < 0) {
        Serial.println("AVISO: Leitura de Lux inválida. Pulando envio.");
        delay(30000); 
        return; 
    }

    enviar_dados_sheets(distance, lux);
 
    delay(30000); 
}