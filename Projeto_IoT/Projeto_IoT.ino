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
const char* SHEETS_URL = "https://script.google.com/macros/s/AKfycbyd12345ABCDEFabcdef0123456789/exec"; 

// ID único para identificar este dispositivo na planilha.
const char* DEVICE_ID = "PC_USER_01"; 

// Configurações do NTP (Network Time Protocol) para sincronizar o relógio do ESP32.
const char* ntpServer = "pool.ntp.org";      // Servidor NTP global.
const long gmtOffset_sec = -10800;           // Offset de GMT em segundos (Ex: -3 horas para Brasília: -3 * 3600 = -10800).
const int daylightOffset_sec = 0;            // Ajuste para Horário de Verão (0 se não usado).

// ====================================================================
// VARIÁVEIS DE CÁLCULO E TEMPORIZAÇÃO (MILLIS)
// ====================================================================

// Intervalo entre cada leitura de sensor (10 segundos = 10000 ms).
const long LEITURA_INTERVALO_MS = 10000; 
unsigned long ultimoTempoLeitura = 0; // Armazena o último momento que a leitura foi feita.

// --- Configurações da Média Móvel ---
// Baseado no intervalo de 10s, definimos 4 amostras.
// O envio ocorrerá a cada 4 amostras (40 segundos).
const int NUM_AMOSTRAS = 4; 
int indiceAmostra = 0; // Índice atual da amostra no array (onde o próximo dado será armazenado).
int amostrasColetadas = 0; // Contador de quantas amostras válidas foram coletadas (máximo NUM_AMOSTRAS)

// Arrays para armazenar as últimas N amostras para o cálculo da média.
float distarray[NUM_AMOSTRAS]; // Array para distância
float lux_arr[NUM_AMOSTRAS]; // Array para luminosidade

// --- Limiares de Alerta ---
const float LIMIAR_DISTANCIA_CM = 50.0; // Distância (muito próximo) 
const float LIMIAR_LUX = 30.0;         // Luminosidade (muito escuro)

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

void enviar_dados_sheets(float distance_avg, float lux_avg, long timestamp, bool alertaProx, bool alertaLux) {
    // Verifica a conexão Wi-Fi antes de tentar o envio.
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi não conectado. Pulando envio.");
        return;
    }

    HTTPClient http;
    http.begin(SHEETS_URL); // Inicia a conexão HTTP com o URL do Google Sheets.
    http.addHeader("Content-Type", "application/json"); // Define o cabeçalho para JSON.

    char payload[350]; 
    
    char dist_str[10];
    char lux_str[10];
    char ts_str[15]; 
    
    // Converte floats para strings com 2 casas decimais.
    dtostrf(distance_avg, 4, 2, dist_str); 
    dtostrf(lux_avg, 4, 2, lux_str);
    
    // Converte o long timestamp para string.
    snprintf(ts_str, sizeof(ts_str), "%ld", timestamp); 

    // --- Montagem do JSON ---
    snprintf(payload, sizeof(payload), 
             "{\"id\":\"%s\",\"distance\":%s,\"lux\":%s,\"timestamp\":%s,\"alertaProx\":%s,\"alertaLux\":%s}", 
             DEVICE_ID, 
             dist_str, 
             lux_str,
             ts_str,
             alertaProx ? "true" : "false", 
             alertaLux ? "true" : "false"); 
    
    Serial.print("Enviando JSON de Alerta: ");
    Serial.println(payload);

    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0) {
        // Código de resposta HTTP positivo, o ESP32 enviou o pacote.
        Serial.printf("Dados enviados. Código HTTP: %d\n", httpResponseCode);
    } else {
        // Código de resposta HTTP negativo (ex: -11 é falha na conexão, 404 é URL não encontrada).
        Serial.printf("Falha ao enviar dados. Erro HTTP: %d (Verifique a SHEETS_URL e o status do Wi-Fi)\n", httpResponseCode);
    }
    
    http.end(); // Fecha a conexão HTTP.
}

// ====================================================================
// Calcula a média de um array de floats.

float calcular_media(float arr[], int tamanho) {
    float soma = 0.0;
    
    for (int i = 0; i < tamanho; i++) {
        soma += arr[i];
    }
    // Garante que não dividimos por zero e retorna a média.
    return (tamanho > 0) ? (soma / tamanho) : 0.0;
}
// ====================================================================
// FUNÇÃO: setup()
// ====================================================================

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

void loop() {
    unsigned long tempoAtual = millis();

    // --- 1. Lógica de Coleta de Dados (a cada 10 segundos) ---
    if (tempoAtual - ultimoTempoLeitura >= LEITURA_INTERVALO_MS) {
        ultimoTempoLeitura = tempoAtual;

        // ** Ação 1: Coleta dos dados dos sensores **
        float distancia = proximidade();
        float lux = luminosidade(); 
        
        // ** Ação 2: Armazenamento circular dos dados **
        
        // Armazena a leitura atual no índice atual dos arrays.
        distarray[indiceAmostra] = distancia; 
        lux_arr[indiceAmostra] = lux;
        
        Serial.printf("\n[COLETA #%d] Dist: %.2f cm | Lux: %.2f lx\n", indiceAmostra, distancia, lux);

        // ** Ação 3: Avança o índice e o contador **
        
        // Move o índice para a próxima posição (circular).
        int proximoIndice = (indiceAmostra + 1) % NUM_AMOSTRAS; 
        
        // Incrementa o contador de amostras coletadas (máximo NUM_AMOSTRAS).
        if (amostrasColetadas < NUM_AMOSTRAS) {
            amostrasColetadas++;
        }

        // --- 2. Lógica de Envio: ACIONADA QUANDO O BUFFER ESTÁ COMPLETO ---
        // Se já coletamos 4 amostras E a próxima posição seria 0 (o que significa que esta foi a 4ª amostra).
        if (amostrasColetadas == NUM_AMOSTRAS && proximoIndice == 0) {
            // O envio será acionado exatamente após a 4ª, 8ª, 12ª, etc., coleta.

            // 3. Cálculo da Média (média das últimas N leituras)
            int tamanho_real = NUM_AMOSTRAS; // Agora sempre 4, após o buffer estar cheio.
            
            float mediaDistancia = calcular_media(distarray, tamanho_real); 
            float mediaLux = calcular_media(lux_arr, tamanho_real);
            long timestamp = get_current_timestamp(); // Pega o timestamp atual.
            
            Serial.println("\n--- ANÁLISE DE ENVIO (Média Móvel) ---");
            Serial.printf("Amostras na Média: %d\n", tamanho_real);
            Serial.printf("Média Distância: %.2f cm (Limiar: %.2f cm)\n", mediaDistancia, LIMIAR_DISTANCIA_CM); 
            Serial.printf("Média Luminosidade: %.2f lx (Limiar: %.2f lx)\n", mediaLux, LIMIAR_LUX);

            // 4. Lógica de Alerta Condicional
            bool alertaProximidade = mediaDistancia < LIMIAR_DISTANCIA_CM; 
            bool alertaLuminosidade = mediaLux < LIMIAR_LUX;
            
            // Mensagem no Serial Monitor para indicar se houve alerta
            if (alertaProximidade || alertaLuminosidade) {
                Serial.print("** ALERTA DETECTADO! Tipo(s): ");
                if (alertaProximidade) Serial.print("[PROXIMIDADE] ");
                if (alertaLuminosidade) Serial.print("[LUMINOSIDADE]");
                Serial.println(" **");
            } else {
                Serial.println("Condição normal. Nenhum alerta disparado.");
            }
            
            // 5. CHAMA A FUNÇÃO DE ENVIO DE DADOS
            enviar_dados_sheets(mediaDistancia, mediaLux, timestamp, alertaProximidade, alertaLuminosidade); 

            Serial.println("------------------------------------------\n");
        }

        // Atualiza o índice para o próximo ciclo
        indiceAmostra = proximoIndice; 
    }
    
    // Pequeno delay para evitar que o loop rode rápido demais e trave o ESP32 (não é bloqueante)
    delay(10); 
}
