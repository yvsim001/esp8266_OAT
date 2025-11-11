#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

#ifndef FW_MODEL
#define FW_MODEL "esp8266-power"
#endif
#ifndef FW_VERSION
#define FW_VERSION "v1.0.0"
#endif
#ifndef FW_MANIFEST_URL
#define FW_MANIFEST_URL "https://raw.githubusercontent.com/yvsim001/esp8266_OAT/main/manifest.json"
#endif

// --- PROTOTYPES ---
bool httpCheckAndUpdate();

// --- SETUP / LOOP OBLIGATOIRES ---
void setup() {
  Serial.begin(115200);
  delay(200);

  // Connexion Wi-Fi minimale (remplace si tu utilises WiFiManager)
  WiFi.mode(WIFI_STA);
  WiFi.begin("DasNetz", "-philipplucas-");            // <-- mets ton Wi-Fi provisoirement
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());

  // Rien d’autre d’obligatoire ici pour le test de lien.
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();

  // test: check OTA toutes les 30s
  if (now - last > 30000UL) {
    Serial.println("Check OTA...");
    httpCheckAndUpdate();
    last = now;
  }
  delay(10);
}

// --- OTA PULL (HTTPS, insecure pour démarrer) ---
bool httpCheckAndUpdate() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // <--- important

  Serial.print("[OTA] GET: "); Serial.println(FW_MANIFEST_URL);
  if (!http.begin(*client, String(FW_MANIFEST_URL))) return false;

  int code = http.GET();
  Serial.print("[OTA] HTTP code: "); Serial.println(code);
  if (code != 200) { http.end(); return false; }

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { Serial.print("[OTA] JSON error: "); Serial.println(err.c_str()); return false; }

  const char* model   = doc["model"]   | "";
  const char* version = doc["version"] | "";
  const char* url     = doc["url"]     | "";
  Serial.printf("[OTA] model=%s version=%s url=%s\n", model, version, url);

  if (String(model) != FW_MODEL) return false;
  if (String(version) == FW_VERSION) { Serial.println("[OTA] déjà à jour"); return false; }

  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = ESPhttpUpdate.update(*client, String(url), String(FW_VERSION));
  Serial.printf("[OTA] result=%d (%s)\n", ret, ESPhttpUpdate.getLastErrorString().c_str());
  return (ret == HTTP_UPDATE_OK);
}
