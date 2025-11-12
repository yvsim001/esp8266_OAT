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

  delay(300000);
}

// --- OTA PULL (HTTPS, insecure pour démarrer) ---
bool httpCheckAndUpdate() {
  // -------- 1) Télécharger le manifest (client #1) --------
  std::unique_ptr<BearSSL::WiFiClientSecure> cli1(new BearSSL::WiFiClientSecure);
  cli1->setInsecure();
  cli1->setTimeout(15000); // 15 s

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  Serial.print(F("[OTA] GET manifest: "));
  Serial.println(FW_MANIFEST_URL);

  if (!http.begin(*cli1, String(FW_MANIFEST_URL))) {
    Serial.println(F("[OTA] http.begin() failed"));
    return false;
  }

  int code = http.GET();
  Serial.printf("[OTA] HTTP code: %d (%s)\n", code, http.errorToString(code).c_str());
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  // DynamicJsonDocument = moins de risques que Static trop juste
  DynamicJsonDocument doc(1536); // large pour 3 champs + marge
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end(); // libère tout ce qui touche au manifest
  if (err) {
    Serial.printf("[OTA] JSON error: %s\n", err.c_str());
    return false;
  }

  const char* model   = doc["model"]   | "";
  const char* version = doc["version"] | "";
  const char* url     = doc["url"]     | "";

  Serial.printf("[OTA] model=%s version=%s url=%s\n", model, version, url);

  if (strcmp(model, FW_MODEL) != 0) {
    Serial.println(F("[OTA] Model mismatch"));
    return false;
  }
  if (strcmp(version, FW_VERSION) == 0) {
    Serial.println(F("[OTA] Deja a jour"));
    return false;
  }

  // Petit garde-fou sur la taille URL (logs + early return si énorme)
  if (strlen(url) > 230) { // marge confortable pour un buffer 256 si jamais
    Serial.println(F("[OTA] URL trop longue, abort"));
    return false;
  }

  // -------- 2) Faire l’update (client #2, NE PAS réutiliser cli1) --------
  std::unique_ptr<BearSSL::WiFiClientSecure> cli2(new BearSSL::WiFiClientSecure);
  cli2->setInsecure();
  cli2->setTimeout(30000); // 30 s pour le binaire

  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true); // ou false si tu veux voir le log de fin

  yield(); // nourrir le WDT avant opération longue
  t_httpUpdate_return ret = ESPhttpUpdate.update(*cli2, String(url), String(FW_VERSION));
  yield();

  Serial.printf("[OTA] result=%d (%s)\n", ret, ESPhttpUpdate.getLastErrorString().c_str());
  return (ret == HTTP_UPDATE_OK);
}
