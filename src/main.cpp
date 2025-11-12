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
#define FW_VERSION "v1.1.0"
#endif
#ifndef FW_MANIFEST_URL
#define FW_MANIFEST_URL "https://raw.githubusercontent.com/yvsim001/esp8266_OTA/gh-pages/manifest.json"
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
  WiFi.setSleep(false);                    // <- évite les micro-coupures pendant OTA
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);          // 3 min pour saisir SSID/MdP
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

  digitalWrite(LED, HIGH);  // éteint
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

  // JSON : doc dynamique large (sécurise la marge)
  DynamicJsonDocument doc(1536);
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

  // Garde-fou sur la taille URL
  if (strlen(url) > 230) {
    Serial.println(F("[OTA] URL trop longue, abort"));
    return false;
  }

  // -------- 2) OTA MANUELLE (plus robuste que ESPhttpUpdate sur ESP8266) --------
  // (1) Client TLS dédié, buffers + timeouts
  std::unique_ptr<BearSSL::WiFiClientSecure> cli2(new BearSSL::WiFiClientSecure);
  cli2->setInsecure();               // (ancrer un cert = mieux en prod)
  cli2->setBufferSizes(2048, 1024);  // records TLS plus gros (ok sur D1 mini)
  cli2->setTimeout(45000);           // 45 s pour un .bin

  // (2) HTTP client configuré
  HTTPClient http2;
  http2.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http2.setTimeout(45000);
  http2.useHTTP10(true); // HTTP/1.0 limite les soucis de chunked/keep-alive

  // (3) Lancer la requête
  Serial.printf("[OTA] Download: %s\n", url);
  if (!http2.begin(*cli2, String(url))) {
    Serial.println(F("[OTA] http.begin() failed (bin)"));
    return false;
  }
  int rc = http2.GET();
  Serial.printf("[OTA] GET bin rc=%d (%s)\n", rc, http2.errorToString(rc).c_str());
  if (rc != HTTP_CODE_OK) { http2.end(); return false; }

  // (4) Récup taille & MD5 (optionnel via manifest)
  int contentLength = http2.getSize(); // -1 si chunked
  const char* md5 = doc["md5"] | "";
  if (md5[0]) {
    Serial.printf("[OTA] MD5: %s\n", md5);
    Update.setMD5(md5);
  }

  // (5) Préparer l’Update
  Serial.printf("[OTA] free heap before: %u\n", ESP.getFreeHeap());

  size_t expected =
      (contentLength > 0) ? (size_t)contentLength
#ifdef UPDATE_SIZE_UNKNOWN
                          : (size_t)UPDATE_SIZE_UNKNOWN
#else
                          : (size_t)ESP.getFreeSketchSpace()
#endif
      ;

  bool canBegin = Update.begin(expected);  // ESP8266 core: besoin d'une taille
  if (!canBegin) {
    Serial.printf("[OTA] Update.begin failed (%s)\n", Update.getErrorString());
    Update.printError(Serial);
    http2.end();
    return false;
  }

  // (6) Ecrire le flux OTA avec yield() réguliers
  WiFiClient *stream = http2.getStreamPtr();
  uint8_t buf[1024];
  uint32_t written = 0;
  uint32_t lastYield = millis();

  while (http2.connected()) {
    size_t avail = stream->available();
    if (avail) {
      size_t n = stream->readBytes(buf, (avail > sizeof(buf)) ? sizeof(buf) : avail);
      size_t w = Update.write(buf, n);
      written += w;
      if (w != n) {
        Serial.printf("[OTA] write short (%u/%u)\n", (unsigned)w, (unsigned)n);
        http2.end();
        Update.end();   // pas d'Update.abort() sur cette core
        return false;
      }
    } else {
      // Nourrir le WDT si pas de données un petit moment
      if (millis() - lastYield > 20) { yield(); lastYield = millis(); }
    }

    // Fin de flux ?
    if (!avail && stream->available() == 0 && stream->peek() == -1 && !stream->connected()) break;
  }

  Serial.printf("[OTA] written: %u bytes\n", written);
  http2.end();

  if (!Update.end()) {
    Serial.printf("[OTA] Update.end failed (%s)\n", Update.getErrorString());
    Update.printError(Serial);
    return false;
  }
  if (!Update.isFinished()) {
    Serial.println(F("[OTA] Update not finished"));
    return false;
  }

  Serial.println(F("[OTA] Update OK, reboot..."));
  ESP.restart();
  return true;
}
