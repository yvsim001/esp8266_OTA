#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ArduinoJson.h>
#include <Ticker.h>

#ifndef FW_MODEL
#define FW_MODEL "esp8266-power"
#endif
#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

// ---------- CONFIG ----------
const char* MQTT_HOST = "pi.local";
const uint16_t MQTT_PORT = 1883;
const char* TOPIC_MEAS = "power/line1/current";
const char* TOPIC_STATUS = "power/esp8266/status";
const char* FW_MANIFEST_URL = "http://pi.local/firmware/manifest.json";

// ---------- GLOBALS ----------
WiFiClient net;
PubSubClient mqtt(net);
Ticker sampleTicker;

constexpr uint16_t N_SAMPLES = 1024;    // fenêtre RMS
volatile uint16_t sampleCount = 0;
volatile float accSq = 0.0f;
volatile float acc = 0.0f;

float vref = 1.65f;        // Volts, centre ACS712 (à calibrer)
float adcMaxV = 3.20f;     // D1 mini ~3.2V (sinon 1.0V pour ESP8266 nu)
float vcc = 3.30f;         // V alimentation MCU
float mvPerA = 185.0f;     // ACS712 5A: 185 mV/A ; 20A: 100 mV/A ; 30A: 66 mV/A

// lecture ADC -> Volts
inline float readADCv() {
  int raw = analogRead(A0);               // 0..1023
  return (raw / 1023.0f) * adcMaxV;
}

void calibrateOffset(uint16_t n=2000) {
  float sum=0;
  for(uint16_t i=0;i<n;i++){ sum += readADCv(); delay(1); }
  vref = sum / n;
}

void sampleOnce() {
  float v = readADCv();
  float centered = v - vref;
  acc += centered;
  accSq += centered * centered;
  sampleCount++;
}

void startSampling(float hz=2000.0f) {
  // ESP8266 Ticker max ~1 kHz fiable, ici on sur-échantillonne approx avec loop()
  sampleTicker.attach_ms(1, [](){ sampleOnce(); });
}

void stopSampling(){ sampleTicker.detach(); }

void publishStatus(const char* state){
  StaticJsonDocument<256> doc;
  doc["status"] = state;
  doc["model"] = FW_MODEL;
  doc["version"] = FW_VERSION;
  doc["rssi"] = WiFi.RSSI();
  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(TOPIC_STATUS, buf, n, true);
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  while (!mqtt.connected()) {
    String cid = String(FW_MODEL) + "-" + String(ESP.getChipId(), HEX);
    if (mqtt.connect(cid.c_str())) {
      publishStatus("online");
      break;
    }
    delay(2000);
  }
}

bool httpCheckAndUpdate() {
  HTTPClient http;
  if (!http.begin(FW_MANIFEST_URL)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return false;

  const char* model = doc["model"] | "";
  const char* version = doc["version"] | "";
  const char* url = doc["url"] | "";

  if (String(model) != FW_MODEL) return false;
  if (String(version) == FW_VERSION) return false;

  t_httpUpdate_return ret = ESPhttpUpdate.update(url);
  if (ret == HTTP_UPDATE_OK) return true;
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFiManager wm;
  wm.setTimeout(120);
  if(!wm.autoConnect("ESP8266-Setup")) {
    ESP.restart();
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  calibrateOffset(); // capture vref au repos
  startSampling();

  publishStatus("boot");
}

void loop() {
  ensureMqtt();
  mqtt.loop();

  static uint32_t lastReport = 0;
  static uint32_t lastOtaCheck = 0;
  uint32_t now = millis();

  // Rapport toutes les 2s
  if (now - lastReport > 2000) {
    noInterrupts();
    uint16_t n = sampleCount;
    float s = acc;
    float s2 = accSq;
    sampleCount = 0; acc = 0; accSq = 0;
    interrupts();

    if (n > 0) {
      float mean = s / n;          // en Volts autour de 0
      float vrms = sqrtf(s2 / n);  // Vrms
      float arns = (vrms * 1000.0f) / mvPerA;  // mV→A
      float aavg = (mean * 1000.0f) / mvPerA;

      StaticJsonDocument<256> doc;
      doc["rms"] = arns;
      doc["avg"] = aavg;
      doc["offset"] = vref;
      doc["samples"] = n;
      doc["ts"] = (uint32_t)(time(nullptr));

      char buf[256];
      size_t len = serializeJson(doc, buf);
      mqtt.publish(TOPIC_MEAS, buf, len, false);
    }
    lastReport = now;
  }

  // Vérification OTA toutes les 10 min
  if (now - lastOtaCheck > 600000UL) {
    httpCheckAndUpdate();
    lastOtaCheck = now;
  }
}
