#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPIFFS.h>
#include <mbedtls/sha256.h>
#include <ArduinoJson.h>
#include "time.h"

// ====== USER CONFIG ======
const char* ssid = "DarkoAP";
const char* password = "1qazXcvb";

// Current firmware version of the device
const char* currentVersion = "1.0.5";

// Scheduled OTA check time (UTC)
const int SCHEDULED_HOUR_UTC = 15;
const int SCHEDULED_MINUTE_UTC = 47;

// NTP config
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 0;
const int   DAYLIGHT_OFFSET_SEC = 0;

// Max OTA retries
const int MAX_OTA_RETRIES = 3;

// ====== GLOBALS ======
bool bootUpdateApplied = false;
bool scheduledRunToday = false;
int lastScheduledDay = -1;

bool schedulerReady = false; // new flag

// Firmware info structure
struct FirmwareInfo {
  String version;
  String url;
  String sha256;
};

// ====== HELPER FUNCTIONS ======
void initSPIFFS() {
  if(!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Failed to mount SPIFFS!");
  } else {
    Serial.println("[SPIFFS] Mounted successfully.");
  }
}

void initWiFi() {
  Serial.printf("[WiFi] Connecting to %s...\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected!");
}

void initNTP() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.print("[Time] Waiting for NTP sync");
  uint32_t start = millis();
  while (time(nullptr) < 100000) {
    delay(250);
    Serial.print(".");
    if (millis() - start > 15000) { // 15s timeout
      Serial.println("\n[Time] NTP sync timeout, retry later.");
      break;
    }
  }
  Serial.println("\n[Time] NTP initialized.");
}

// Proper semantic version comparison
bool isNewerVersion(const String& newVer, const String& currentVer) {
  int nv[3] = {0,0,0};
  int cv[3] = {0,0,0};
  
  sscanf(newVer.c_str(), "%d.%d.%d", &nv[0], &nv[1], &nv[2]);
  sscanf(currentVer.c_str(), "%d.%d.%d", &cv[0], &cv[1], &cv[2]);

  for(int i=0;i<3;i++){
    if(nv[i] > cv[i]) return true;
    if(nv[i] < cv[i]) return false;
  }
  return false; // versions are equal
}


// Convert SHA256 bytes to hex string
String hashToString(const uint8_t* hash, size_t len) {
  String s;
  for(size_t i=0;i<len;i++){
    if(hash[i] < 16) s += "0";
    s += String(hash[i], HEX);
  }
  s.toLowerCase();
  return s;
}

// Fetch manifest from URL
bool fetchManifest(FirmwareInfo &fw) {
  const char* manifestURL = "https://raw.githubusercontent.com/darkoinc/SSM_OTA_Host/refs/heads/main/manifest.json"; 
  WiFiClientSecure client;
  client.setInsecure(); // Skip cert verification (not for production)
  
  HTTPClient https;
  if(!https.begin(client, manifestURL)) {
    Serial.println("[Manifest] HTTPS begin failed");
    return false;
  }
  
  int httpCode = https.GET();
  if(httpCode != HTTP_CODE_OK) {
    Serial.printf("[Manifest] GET failed: %d\n", httpCode);
    https.end();
    return false;
  }
  
  String payload = https.getString();
  https.end();

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if(error){
    Serial.println("[Manifest] JSON parse failed!");
    return false;
  }

  fw.version = doc["version"].as<String>();
  fw.url = doc["url"].as<String>();
  fw.sha256 = doc["sha256"].as<String>();
  
  Serial.printf("[Manifest] version=%s url=%s sha256=%s\n", fw.version.c_str(), fw.url.c_str(), fw.sha256.c_str());
  return true;
}

// Download firmware to SPIFFS
bool downloadFirmware(const String &url, const String &expectedSha) {
  for(int attempt=1; attempt <= MAX_OTA_RETRIES; attempt++){
    Serial.printf("[OTA] Download attempt %d\n", attempt);
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    
    if(!https.begin(client, url)){
      Serial.println("[OTA] HTTPS begin failed");
      continue;
    }

    int httpCode = https.GET();
    if(httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_MOVED_PERMANENTLY && httpCode != HTTP_CODE_FOUND){
      Serial.printf("[OTA] HTTP GET failed: %d\n", httpCode);
      https.end();
      continue;
    }

    // Follow redirect if needed
    if(httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND){
      String newLocation = https.getLocation();
      Serial.printf("[OTA] Redirected to: %s\n", newLocation.c_str());
      https.end();
      return downloadFirmware(newLocation, expectedSha); // recursive redirect follow
    }

    int contentLength = https.getSize();
    if(contentLength <= 0){
      Serial.println("[OTA] Content-Length invalid");
      https.end();
      continue;
    }

    File fwFile = SPIFFS.open("/pending_firmware.bin", FILE_WRITE);
    if(!fwFile){
      Serial.println("[OTA] Failed to open SPIFFS for writing");
      https.end();
      return false;
    }

    WiFiClient *stream = https.getStreamPtr();
    int bytesRead = 0;
    uint8_t buf[512];
    while(https.connected() && bytesRead < contentLength){
      int len = stream->readBytes(buf, sizeof(buf));
      if(len <= 0) break;
      fwFile.write(buf, len);
      bytesRead += len;
      Serial.printf("[OTA] Downloaded %d/%d bytes\n", bytesRead, contentLength);
    }

    fwFile.close();
    https.end();

    // Verify SHA256
    File verifyFile = SPIFFS.open("/pending_firmware.bin", FILE_READ);
    if(!verifyFile){
      Serial.println("[OTA] Failed to open SPIFFS for verification");
      continue;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);

    while(verifyFile.available()){
      int len = verifyFile.read(buf, sizeof(buf));
      mbedtls_sha256_update_ret(&ctx, buf, len);
    }

    verifyFile.close();

    uint8_t hash[32];
    mbedtls_sha256_finish_ret(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    String hashStr = hashToString(hash, sizeof(hash));
    Serial.printf("[SHA256] Computed: %s\n", hashStr.c_str());
    Serial.printf("[SHA256] Expected: %s\n", expectedSha.c_str());

    if(hashStr == expectedSha){
      Serial.println("[OTA] SHA256 match! Firmware ready.");
      return true;
    } else {
      Serial.println("[OTA] SHA256 mismatch! Retrying...");
      SPIFFS.remove("/pending_firmware.bin");
    }
  }

  Serial.println("[OTA] Failed to download/verify firmware after retries.");
  return false;
}

// Apply firmware from SPIFFS
bool applyFirmware() {
  File fwFile = SPIFFS.open("/pending_firmware.bin", FILE_READ);
  if(!fwFile){
    Serial.println("[OTA] Firmware file not found");
    return false;
  }

  if(!Update.begin(fwFile.size())){
    Serial.println("[OTA] Update begin failed");
    fwFile.close();
    return false;
  }

  size_t written = Update.writeStream(fwFile);
  fwFile.close();

  if(written != Update.size()){
    Serial.println("[OTA] Update write incomplete");
    return false;
  }

  if(!Update.end(true)){
    Serial.println("[OTA] Update finalize failed");
    return false;
  }

  Serial.println("[OTA] Firmware applied successfully! Rebooting...");
  ESP.restart();
  return true;
}

void bootFirmwareCheck() {
  FirmwareInfo fw;
  if(fetchManifest(fw) && isNewerVersion(fw.version, currentVersion)){
    Serial.println("[BootCheck] New firmware available, applying immediately...");
    if(downloadFirmware(fw.url, fw.sha256)){
      applyFirmware();
    }
  } else {
    Serial.println("[BootCheck] No new firmware on boot.");
  }

  // Mark that boot check has run so scheduler doesn't trigger immediately
  bootUpdateApplied = true;
}

void scheduledCheckerLoop() {
  if(!schedulerReady) return; // skip until ready

  if(scheduledRunToday) return;

  time_t now = time(nullptr);
  if(now < 100000) return; // invalid NTP time

  struct tm t;
  gmtime_r(&now, &t);
/*
  Serial.print("t.tm_hour: ");
  Serial.println(t.tm_hour);
  Serial.print("t.tm_min: ");
  Serial.println(t.tm_min);
  Serial.print("SCHEDULED_HOUR_UTC: ");
  Serial.println(SCHEDULED_HOUR_UTC);
  Serial.print("SCHEDULED_MINUTE_UTC: ");
  Serial.println(SCHEDULED_MINUTE_UTC);*/

  if(t.tm_hour == SCHEDULED_HOUR_UTC && t.tm_min >= SCHEDULED_MINUTE_UTC) {
    Serial.printf("[Scheduler] UTC time %02d:%02d reached. Checking for firmware...\n", t.tm_hour, t.tm_min);
    FirmwareInfo fw;
    if(fetchManifest(fw) && isNewerVersion(fw.version, currentVersion)){
      if(downloadFirmware(fw.url, fw.sha256)) applyFirmware();
    }
    scheduledRunToday = true;
  }

  // Reset scheduledRunToday at UTC midnight
  if(t.tm_hour == 0 && t.tm_min == 0){
    scheduledRunToday = false;
  }
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(1000);

  initSPIFFS();
  initWiFi();
  initNTP();
  schedulerReady = true;


  bootFirmwareCheck();
}

// ====== LOOP ======
void loop() {
  scheduledCheckerLoop();
  delay(1000);
}
