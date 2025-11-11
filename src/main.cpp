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
#define FW_MANIFEST_URL "https://github.com/yvsim001/esp8266_OAT/blob/main/manifest.json"
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
  client->setInsecure(); // simple pour démarrer (à durcir plus tard)

  HTTPClient http;
  if (!http.begin(*client, String(FW_MANIFEST_URL))) return false;

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return false;

  const char* model   = doc["model"]   | "";
  const char* version = doc["version"] | "";
  const char* url     = doc["url"]     | "";

  if (String(model) != FW_MODEL) return false;
  if (String(version) == FW_VERSION) return false;

  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = ESPhttpUpdate.update(*client, String(url), String(FW_VERSION));
  return (ret == HTTP_UPDATE_OK);
}
