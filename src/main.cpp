#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>  // <-- WiFiManager

#ifndef FW_MODEL
#define FW_MODEL "esp8266-power"
#endif
#ifndef FW_VERSION
#define FW_VERSION "v1.0.0"
#endif
#ifndef FW_MANIFEST_URL
#define FW_MANIFEST_URL "https://raw.githubusercontent.com/yvsim001/esp8266_OAT/gh-pages/manifest.json"
#endif

// Blink LED intégrée (D4 / GPIO2, active LOW)
const int LED = LED_BUILTIN;  // équivaut à GPIO2 sur D1 mini

// --- PROTOTYPE ---
bool httpCheckAndUpdate();

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  delay(200);
  Serial.println();
  Serial.println(F("[BOOT]"));

  // ---- WiFi via WiFiManager (portail captif) ----
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);            // 3 min pour saisir SSID/MdP
  bool ok = wm.autoConnect("ESP8266-Setup"); // AP = ESP8266-Setup (sans mdp)
  if (!ok) {
    Serial.println(F("[WiFi] Echec config. Reboot..."));
    delay(1000);
    ESP.restart();
  }
  Serial.print(F("[WiFi] OK: ")); Serial.println(WiFi.localIP());

  // (Facultatif) régler l’heure NTP pour les timestamps si besoin
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Premier check OTA au démarrage
  httpCheckAndUpdate();
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();

  // Check OTA toutes les 60 s (mets 600000 pour 10 min)
  if (now - last > 60000UL) {
    Serial.println(F("[OTA] Check..."));
    httpCheckAndUpdate();
    last = now;
  }

  //digitalWrite(LED, LOW);   // allume
  //delay(500);
  digitalWrite(LED, HIGH);  // éteint
  //delay(500);

  delay(10);
}

// --- OTA PULL (HTTPS, insecure pour démarrer) ---
bool httpCheckAndUpdate() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // simple pour démarrer (à durcir ensuite)

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // suivre redirs (GitHub)

  Serial.print(F("[OTA] GET manifest: ")); Serial.println(FW_MANIFEST_URL);
  if (!http.begin(*client, String(FW_MANIFEST_URL))) {
    Serial.println(F("[OTA] http.begin() failed"));
    return false;
  }

  int code = http.GET();
  Serial.printf("[OTA] HTTP code: %d (%s)\n", code, http.errorToString(code).c_str());
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[OTA] JSON error: %s\n", err.c_str());
    return false;
  }

  const char* model   = doc["model"]   | "";
  const char* version = doc["version"] | "";
  const char* url     = doc["url"]     | "";

  Serial.printf("[OTA] model=%s version=%s url=%s\n", model, version, url);
  if (String(model) != FW_MODEL) { Serial.println(F("[OTA] Model mismatch")); return false; }
  if (String(version) == FW_VERSION) { Serial.println(F("[OTA] Deja a jour")); return false; }

  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = ESPhttpUpdate.update(*client, String(url), String(FW_VERSION));
  Serial.printf("[OTA] result=%d (%s)\n", ret, ESPhttpUpdate.getLastErrorString().c_str());

  return (ret == HTTP_UPDATE_OK);
}
