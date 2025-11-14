#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#ifndef FW_MODEL
#define FW_MODEL "esp8266-power"
#endif
#ifndef FW_VERSION
#define FW_VERSION "v1.0.0"
#endif
#ifndef FW_MANIFEST_URL
#define FW_MANIFEST_URL "https://raw.githubusercontent.com/yvsim001/esp8266_OTA/gh-pages/manifest.json"
#endif

const int LED = LED_BUILTIN;

// Global state
bool isUpdating = false;
uint32_t lastOtaCheck = 0;

// --- PROTOTYPE ---
bool httpCheckAndUpdate();
void printMemoryStats();

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  digitalWrite(LED, HIGH);  // LED aus
  
  delay(500);
  Serial.println();
  Serial.println(F("[BOOT] ESP8266 OTA System"));
  Serial.printf("[BOOT] Model: %s\n", FW_MODEL);
  Serial.printf("[BOOT] Version: %s\n", FW_VERSION);
  Serial.printf("[BOOT] Manifest: %s\n", FW_MANIFEST_URL);
  
  printMemoryStats();

  // ---- WiFi via WiFiManager ----
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // Deaktiviert WiFi-Sleep für stabilen Betrieb
  
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  
  Serial.println(F("[WiFi] Initializing..."));
  bool ok = wm.autoConnect("ESP8266-Setup");
  
  if (!ok) {
    Serial.println(F("[WiFi] Config failed. Rebooting..."));
    delay(1000);
    ESP.restart();
  }
  
  Serial.print(F("[WiFi] Connected: "));
  Serial.println(WiFi.localIP());
  Serial.printf("[WiFi] Signal strength: %d dBm\n", WiFi.RSSI());

  // NTP Zeit synchronisieren
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  // Erstes OTA-Check beim Boot
  delay(2000);  // 2s warten für WiFi-Stabilität
  httpCheckAndUpdate();
  
  lastOtaCheck = millis();
}

void loop() {
  uint32_t now = millis();

  // OTA-Check alle 60 Sekunden (ändern auf 600000 für 10 Min)
  if ((now - lastOtaCheck) > 60000UL && !isUpdating) {
    Serial.println(F("[LOOP] OTA check time..."));
    httpCheckAndUpdate();
    lastOtaCheck = now;
  }

  // WiFi Signal Diagnostic
  static uint32_t lastWiFiCheck = 0;
  if ((now - lastWiFiCheck) > 10000) {
    int rssi = WiFi.RSSI();
    Serial.printf("[WiFi] Signal strength: %d dBm\\n", rssi);
    
    if (rssi > -50) Serial.println("[WiFi] Excellent signal");
    else if (rssi > -70) Serial.println("[WiFi] Good signal");
    else if (rssi > -80) Serial.println("[WiFi] Fair signal");
    else Serial.println("[WiFi] Weak signal - OTA may fail!");
    
    lastWiFiCheck = now;
  }
  // LED Blinken (heartbeat)
  static uint32_t ledToggle = 0;
  if ((now - ledToggle) > 1000) {
    digitalWrite(LED, !digitalRead(LED));
    ledToggle = now;
  }

  delay(100);
}

// --- HELPER: Memory Stats ---
void printMemoryStats() {
  Serial.printf("[MEM] Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[MEM] Heap fragmentation: %u%%\n", ESP.getHeapFragmentation());
  Serial.printf("[MEM] Max free block: %u bytes\n", ESP.getMaxFreeBlockSize());
}

// --- OTA PULL (HTTPS mit robustem Error-Handling) ---
bool httpCheckAndUpdate() {
  if (isUpdating) {
    Serial.println(F("[OTA] Already updating, skipping"));
    return false;
  }

  // Memory check
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[OTA] Starting OTA check. Free heap: %u bytes\n", freeHeap);
  
  if (freeHeap < 25000) {
    Serial.printf("[OTA] Insufficient memory (%u < 25000)\n", freeHeap);
    return false;
  }

  // ====== PHASE 1: Download Manifest ======
  Serial.println(F("[OTA] === Phase 1: Fetch Manifest ==="));
  
  std::unique_ptr<BearSSL::WiFiClientSecure> manifestClient(new BearSSL::WiFiClientSecure);
  manifestClient->setInsecure();
  manifestClient->setTimeout(15000);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.useHTTP10(true);

  Serial.print(F("[OTA] GET: "));
  Serial.println(FW_MANIFEST_URL);

  if (!http.begin(*manifestClient, String(FW_MANIFEST_URL))) {
    Serial.println(F("[OTA] http.begin() failed"));
    return false;
  }

  int httpCode = http.GET();
  Serial.printf("[OTA] HTTP Code: %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    Serial.println(F("[OTA] Manifest download failed"));
    return false;
  }

  // Parse JSON
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("[OTA] JSON parse error: %s\n", err.c_str());
    return false;
  }

  const char* model = doc["model"] | "";
  const char* version = doc["version"] | "";
  const char* url = doc["url"] | "";

  Serial.printf("[OTA] Model: %s | Version: %s\n", model, version);

  // Validate manifest
  if (strlen(model) == 0 || strlen(version) == 0 || strlen(url) == 0) {
    Serial.println(F("[OTA] Invalid manifest (missing fields)"));
    return false;
  }

  if (strcmp(model, FW_MODEL) != 0) {
    Serial.printf("[OTA] Model mismatch: %s != %s\n", model, FW_MODEL);
    return false;
  }

  if (strcmp(version, FW_VERSION) == 0) {
    Serial.println(F("[OTA] Already up-to-date"));
    return false;
  }

  Serial.printf("[OTA] New version available! Current: %s -> New: %s\n", FW_VERSION, version);

  // ====== PHASE 2: Download & Flash Firmware ======
  Serial.println(F("[OTA] === Phase 2: Download & Flash ==="));

  isUpdating = true;
  
  std::unique_ptr<BearSSL::WiFiClientSecure> fwClient(new BearSSL::WiFiClientSecure);
  fwClient->setInsecure();
  fwClient->setBufferSizes(2048, 1024);
  fwClient->setTimeout(45000);

  // Watchdog & OTA Callbacks
  ESP.wdtEnable(WDTO_8S);
  
  ESPhttpUpdate.onStart([]() {
    Serial.println(F("[OTA] Update starting..."));
    ESP.wdtDisable();  // WDT aus während Flash-Erase
    digitalWrite(LED, LOW);  // LED an (updating)
  });

  ESPhttpUpdate.onProgress([](int cur, int total) {
    static uint32_t lastPrint = 0;
    uint32_t now = millis();
    
    if ((now - lastPrint) > 500) {  // Print alle 500ms
      int percent = (total > 0) ? (cur * 100) / total : 0;
      Serial.printf("[OTA] Progress: %d%% (%d/%d bytes)\r", percent, cur, total);
      lastPrint = now;
    }
    
    yield();        // Gibt anderen Tasks CPU-Zeit
    ESP.wdtFeed();  // Füttert Watchdog
  });

  ESPhttpUpdate.onEnd([]() {
    Serial.println(F("\n[OTA] Update complete"));
    ESP.wdtEnable(WDTO_8S);
    digitalWrite(LED, HIGH);  // LED aus
  });

  ESPhttpUpdate.onError([](int err) {
    Serial.printf("[OTA] Update error: %d - %s\n", err, ESPhttpUpdate.getLastErrorString().c_str());
    ESP.wdtEnable(WDTO_8S);
    digitalWrite(LED, HIGH);
  });

  // Starte Update
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(false);

  Serial.printf("[OTA] Downloading from: %s\n", url);
  t_httpUpdate_return ret = ESPhttpUpdate.update(*fwClient, String(url));

  isUpdating = false;

  // Handle result
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] FAILED: %s\n", ESPhttpUpdate.getLastErrorString().c_str());
      printMemoryStats();
      return false;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println(F("[OTA] No updates available"));
      return false;

    case HTTP_UPDATE_OK:
      Serial.println(F("[OTA] Update OK! Rebooting..."));
      delay(2000);
      ESP.restart();
      return true;

    default:
      Serial.printf("[OTA] Unknown result: %d\n", ret);
      return false;
  }

  return false;
}