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
  digitalWrite(LED, HIGH);
  
  delay(500);
  Serial.println();
  Serial.println(F("[BOOT] ESP8266 OTA System"));
  Serial.printf("[BOOT] Model: %s\n", FW_MODEL);
  Serial.printf("[BOOT] Version: %s\n", FW_VERSION);
  
  printMemoryStats();

  // WiFi optimieren für OTA
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setOutputPower(20.5);  // Max TX-Power
  
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

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  delay(2000);
  httpCheckAndUpdate();
  
  lastOtaCheck = millis();
}

void loop() {
  uint32_t now = millis();

  if ((now - lastOtaCheck) > 60000UL && !isUpdating) {
    Serial.println(F("[LOOP] OTA check time..."));
    httpCheckAndUpdate();
    lastOtaCheck = now;
  }

  static uint32_t ledToggle = 0;
  if ((now - ledToggle) > 1000) {
    digitalWrite(LED, !digitalRead(LED));
    ledToggle = now;
  }

  delay(100);
}

void printMemoryStats() {
  Serial.printf("[MEM] Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[MEM] Heap fragmentation: %u%%\n", ESP.getHeapFragmentation());
  Serial.printf("[MEM] Max free block: %u bytes\n", ESP.getMaxFreeBlockSize());
}

bool httpCheckAndUpdate() {
  if (isUpdating) {
    Serial.println(F("[OTA] Already updating"));
    return false;
  }

  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[OTA] Free heap: %u bytes\n", freeHeap);
  
  if (freeHeap < 30000) {
    Serial.printf("[OTA] Insufficient memory (%u < 30000)\n", freeHeap);
    return false;
  }

  // === PHASE 1: Manifest ===
  Serial.println(F("[OTA] Fetching manifest..."));
  
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  client->setTimeout(20000);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.useHTTP10(true);

  if (!http.begin(*client, String(FW_MANIFEST_URL))) {
    Serial.println(F("[OTA] http.begin() failed"));
    return false;
  }

  int code = http.GET();
  Serial.printf("[OTA] HTTP: %d\n", code);

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  client.reset();  // Speicher freigeben!

  if (err) {
    Serial.printf("[OTA] JSON error: %s\n", err.c_str());
    return false;
  }

  const char* model = doc["model"] | "";
  const char* version = doc["version"] | "";
  const char* url = doc["url"] | "";

  Serial.printf("[OTA] Model: %s | Version: %s\n", model, version);

  if (strcmp(model, FW_MODEL) != 0) {
    Serial.println(F("[OTA] Model mismatch"));
    return false;
  }

  if (strcmp(version, FW_VERSION) == 0) {
    Serial.println(F("[OTA] Up-to-date"));
    return false;
  }

  Serial.printf("[OTA] New: %s -> %s\n", FW_VERSION, version);

  // WICHTIG: Speicher aufräumen vor OTA!
  doc.clear();
  yield();
  delay(100);
  
  printMemoryStats();
  
  // === PHASE 2: OTA Update mit kleineren Buffern ===
  Serial.println(F("[OTA] Starting download..."));
  
  isUpdating = true;
  
  // KRITISCH: Kleinere Buffer für D1 Mini!
  std::unique_ptr<BearSSL::WiFiClientSecure> fwClient(new BearSSL::WiFiClientSecure);
  fwClient->setInsecure();
  fwClient->setBufferSizes(1024, 512);  // REDUZIERT von (2048, 1024)!
  fwClient->setTimeout(60000);

  // Watchdog-Handling
  ESPhttpUpdate.onStart([]() {
    Serial.println(F("[OTA] Flashing..."));
    ESP.wdtDisable();
    digitalWrite(LED, LOW);
  });

  uint32_t lastYield = millis();
  ESPhttpUpdate.onProgress([&lastYield](int cur, int total) {
    uint32_t now = millis();
    
    // SEHR HÄUFIG yield() aufrufen!
    if ((now - lastYield) > 100) {  // Alle 100ms
      int pct = (total > 0) ? (cur * 100) / total : 0;
      Serial.printf("[OTA] %d%% (%d/%d)\r", pct, cur, total);
      lastYield = now;
    }
    
    // KRITISCH: WiFi-Stack füttern
    yield();
    delay(1);  // Gib WiFi-Stack Zeit
    ESP.wdtFeed();
  });

  ESPhttpUpdate.onEnd([]() {
    Serial.println(F("\n[OTA] Complete!"));
    ESP.wdtEnable(WDTO_8S);
  });

  ESPhttpUpdate.onError([](int err) {
    Serial.printf("[OTA] ERROR %d: %s\n", err, ESPhttpUpdate.getLastErrorString().c_str());
    ESP.wdtEnable(WDTO_8S);
  });

  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(false);
  
  // Setze LED-Mode für Update (optional)
  ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);

  Serial.printf("[OTA] URL: %s\n", url);
  t_httpUpdate_return ret = ESPhttpUpdate.update(*fwClient, String(url));

  isUpdating = false;

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] FAILED: %s\n", ESPhttpUpdate.getLastErrorString().c_str());
      printMemoryStats();
      return false;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println(F("[OTA] No updates"));
      return false;

    case HTTP_UPDATE_OK:
      Serial.println(F("[OTA] SUCCESS! Rebooting..."));
      delay(2000);
      ESP.restart();
      return true;
  }

  return false;
}