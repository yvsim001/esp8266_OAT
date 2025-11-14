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

// --- PROTOTYPE ---
bool httpCheckAndUpdate();

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  delay(200);
  Serial.println();
  Serial.println(F("[BOOT]"));

  // ---- WiFi via WiFiManager ----
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  bool ok = wm.autoConnect("ESP8266-Setup");
  if (!ok) {
    Serial.println(F("[WiFi] Echec config. Reboot..."));
    delay(1000);
    ESP.restart();
  }
  Serial.print(F("[WiFi] OK: ")); Serial.println(WiFi.localIP());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Premier check OTA au démarrage
  httpCheckAndUpdate();
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();

  // Check OTA toutes les 60 s
  if (now - last > 60000UL) {
    Serial.println(F("[OTA] Check..."));
    httpCheckAndUpdate();
    last = now;
  }

  digitalWrite(LED, HIGH);
  delay(1000);
}

// --- OTA PULL (HTTPS) ---
bool httpCheckAndUpdate() {
  // Vérifier la mémoire disponible
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[OTA] Free heap: %u bytes\n", freeHeap);

  if (freeHeap < 20000) {
    Serial.println(F("[OTA] Insufficient memory"));
    return false;
  }

  // -------- 1) Télécharger le manifest --------
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  Serial.print(F("[OTA] GET manifest: "));
  Serial.println(FW_MANIFEST_URL);

  if (!http.begin(*client, String(FW_MANIFEST_URL))) {
    Serial.println(F("[OTA] http.begin() failed"));
    return false;
  }

  int code = http.GET();
  Serial.printf("[OTA] HTTP code: %d\n", code);

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  // Parser le JSON
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("[OTA] JSON error: %s\n", err.c_str());
    return false;
  }

  const char* model   = doc["model"]   | "";
  const char* version = doc["version"] | "";
  const char* url     = doc["url"]     | "";

  Serial.printf("[OTA] model=%s version=%s\n", model, version);

  // Vérifier le modèle
  if (strcmp(model, FW_MODEL) != 0) {
    Serial.println(F("[OTA] Model mismatch"));
    return false;
  }

  // Vérifier si mise à jour nécessaire
  if (strcmp(version, FW_VERSION) == 0) {
    Serial.println(F("[OTA] Already up to date"));
    return false;
  }

  Serial.println(F("[OTA] New version available, updating..."));
  Serial.printf("[OTA] URL: %s\n", url);

  // -------- 2) Effectuer la mise à jour OTA --------
  // Créer un nouveau client pour la mise à jour
  std::unique_ptr<BearSSL::WiFiClientSecure> updateClient(new BearSSL::WiFiClientSecure);
  updateClient->setInsecure();
  updateClient->setBufferSizes(2048, 1024);

  // Configurer ESPhttpUpdate
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(false);  // Contrôle manuel du reboot

  // Callbacks pour le progrès
  ESPhttpUpdate.onStart([]() {
    Serial.println(F("[OTA] Update started"));
  });

  ESPhttpUpdate.onEnd([]() {
    Serial.println(F("[OTA] Update finished"));
  });

  ESPhttpUpdate.onProgress([](int cur, int total) {
    Serial.printf("[OTA] Progress: %d%%\r", (cur * 100) / total);
  });

  ESPhttpUpdate.onError([](int err) {
    Serial.printf("[OTA] Error: %d - %s\n", err, ESPhttpUpdate.getLastErrorString().c_str());
  });

  // Lancer la mise à jour
  t_httpUpdate_return ret = ESPhttpUpdate.update(*updateClient, String(url));

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] Update failed: %s\n", ESPhttpUpdate.getLastErrorString().c_str());
      return false;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println(F("[OTA] No updates"));
      return false;

    case HTTP_UPDATE_OK:
      Serial.println(F("[OTA] Update OK! Rebooting..."));
      delay(1000);
      ESP.restart();
      return true;
  }

  return false;
}

