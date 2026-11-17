/*
 * AMPARO - dispositivo de emergencia para idosos e PcD
 * Global Solution 2026/2 - CitizenTech - FIAP 2TDSPB
 *
 * Placa: ESP32 DevKit (programado pela IDE Arduino)
 * Sensores/atuadores:
 *   - Botao de emergencia  -> GPIO 4  (INPUT_PULLUP, aperta = LOW)
 *   - MPU6050 (acelerometro) -> I2C SDA=GPIO 21, SCL=GPIO 22
 *   - LED de status         -> GPIO 2
 *   - Buzzer                -> GPIO 15
 *
 * Fluxo: le botao e acelerometro -> detecta emergencia -> POST /eventos na API
 * Amparo (Wi-Fi) com o header X-Device-Token -> a API aciona o cuidador.
 *
 * Roda identico no Wokwi (simulador) e no hardware fisico: so muda o Wi-Fi.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <ArduinoJson.h>

// ===================== CONFIGURACAO =====================
// Wokwi: rede "Wokwi-GUEST" sem senha. Hardware fisico: troque pela sua rede.
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// URL da API (VM Azure da entrega de DevOps, ou IP da sua maquina na mesma rede)
const char* API_URL       = "http://SEU_IP_OU_HOST:8080/eventos";
const char* DEVICE_TOKEN  = "esp32-amparo-dev-token";   // mesmo valor de amparo.device-token na API
const char* CODIGO_SERIAL = "ESP32-0001";               // dispositivo cadastrado na API (Maria das Dores)

// Pinos
const int PIN_BOTAO  = 4;
const int PIN_LED    = 2;
const int PIN_BUZZER = 15;

// Deteccao de queda: aceleracao total acima deste valor (em g) = impacto.
// Calibravel: queda real fica entre 2 e 3 g. (No Wokwi o slider vai ate 2 g por eixo.)
const float LIMIAR_QUEDA_G = 1.8;
// Depois de um impacto, so detecta outro quando a aceleracao voltar abaixo disto (repouso ~1 g)
const float LIMIAR_REPOUSO_G = 1.3;

// Intervalo minimo entre dois eventos (evita disparar 10x no mesmo impacto)
const unsigned long INTERVALO_MIN_MS = 10000;
// ========================================================

Adafruit_MPU6050 mpu;
bool mpuDisponivel = false;
bool emImpacto = false;          // histerese da deteccao de queda
unsigned long ultimoEventoMs = 0;
bool avisouIntervalo = false;

void conectarWiFi() {
  Serial.printf("Conectando ao Wi-Fi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));   // pisca enquanto conecta
    delay(300);
    Serial.print(".");
  }
  digitalWrite(PIN_LED, HIGH);   // LED aceso = conectado
  Serial.printf("\nConectado! IP: %s\n", WiFi.localIP().toString().c_str());
}

/** Envia o evento para a API. Devolve true se a API respondeu 201. */
bool enviarEvento(const char* tipo, const String& descricao) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi caiu, reconectando...");
    conectarWiFi();
  }

  // Monta o JSON exatamente no formato do EventoRequest da API
  JsonDocument doc;
  doc["codigoSerial"] = CODIGO_SERIAL;
  doc["tipo"]         = tipo;
  doc["descricao"]    = descricao;
  String corpo;
  serializeJson(doc, corpo);

  HTTPClient http;
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  http.setTimeout(8000);

  Serial.printf("POST %s\n  body: %s\n", API_URL, corpo.c_str());
  int status = http.POST(corpo);
  String resposta = http.getString();
  http.end();

  Serial.printf("  HTTP %d\n  resposta: %s\n", status, resposta.c_str());
  return status == 201;
}

/** Feedback local + envio, respeitando o intervalo minimo. */
void registrarEmergencia(const char* tipo, const String& descricao) {
  unsigned long agora = millis();
  if (ultimoEventoMs != 0 && agora - ultimoEventoMs < INTERVALO_MIN_MS) {
    if (!avisouIntervalo) {
      Serial.println("Evento ignorado: intervalo minimo de 10 s ainda nao passou");
      avisouIntervalo = true;
    }
    return;
  }
  ultimoEventoMs = agora;
  avisouIntervalo = false;

  Serial.printf("\n*** %s: %s ***\n", tipo, descricao.c_str());

  // Feedback imediato para o cidadao: buzzer + LED
  digitalWrite(PIN_BUZZER, HIGH);
  delay(400);
  digitalWrite(PIN_BUZZER, LOW);

  bool ok = enviarEvento(tipo, descricao);

  // 3 piscadas rapidas = enviado; 1 piscada longa = falha
  if (ok) {
    for (int i = 0; i < 3; i++) {
      digitalWrite(PIN_LED, LOW);  delay(120);
      digitalWrite(PIN_LED, HIGH); delay(120);
    }
    Serial.println("Cuidador acionado pela API.");
  } else {
    digitalWrite(PIN_LED, LOW); delay(1000); digitalWrite(PIN_LED, HIGH);
    Serial.println("FALHA ao enviar o evento. Verifique API_URL e o token.");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOTAO, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  Serial.println("\n=== AMPARO - dispositivo de emergencia ===");

  // Acelerometro
  Wire.begin(21, 22);
  mpuDisponivel = mpu.begin();
  if (mpuDisponivel) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 OK");
  } else {
    Serial.println("MPU6050 nao encontrado: deteccao de queda desativada, botao continua funcionando");
  }

  conectarWiFi();

  // Evento TESTE: confirma conectividade com a API sem acionar ninguem
  enviarEvento("TESTE", "Dispositivo ligado e conectado");
}

void loop() {
  // 1) Botao de emergencia (com debounce)
  if (digitalRead(PIN_BOTAO) == LOW) {
    delay(50);
    if (digitalRead(PIN_BOTAO) == LOW) {
      registrarEmergencia("BOTAO_EMERGENCIA", "Botao de emergencia pressionado");
      while (digitalRead(PIN_BOTAO) == LOW) delay(10);   // espera soltar
    }
  }

  // 2) Deteccao de queda pelo acelerometro
  if (mpuDisponivel) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float magnitudeG = sqrt(a.acceleration.x * a.acceleration.x +
                            a.acceleration.y * a.acceleration.y +
                            a.acceleration.z * a.acceleration.z) / 9.81;

    // Dispara uma vez por impacto: precisa voltar ao repouso antes de detectar de novo
    if (!emImpacto && magnitudeG > LIMIAR_QUEDA_G) {
      emImpacto = true;
      registrarEmergencia("QUEDA_DETECTADA",
                          "Impacto de " + String(magnitudeG, 2) + " g detectado pelo acelerometro");
    } else if (emImpacto && magnitudeG < LIMIAR_REPOUSO_G) {
      emImpacto = false;
      Serial.println("Acelerometro voltou ao repouso");
    }
  }

  delay(50);
}
