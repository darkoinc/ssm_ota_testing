#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Losant.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "time.h"

#define WIFI_SSID       "Elab"
#define WIFI_PASS       "2026summit"

#define LOSANT_DEVICE_ID     "691254353103dc7662730a47"
#define LOSANT_ACCESS_KEY    "73143249-63e0-4cb3-8ad1-9774bd2dc51c"
#define LOSANT_ACCESS_SECRET "5d6b577fffb2fb2c282cb154dbb04f9971baa396c626fc1d5a848b4eb87c8959"

#define CURRENT_FIRMWARE_VERSION "1.0.0"

// Local timezone offset (e.g. UTC−5)
#define TZ_OFFSET_SEC  (-18000)
#define DST_OFFSET_SEC (3600)

// Time of day to apply updates
#define UPDATE_HOUR   15
#define UPDATE_MINUTE 42

WiFiClientSecure wifiClient;
LosantDevice device(LOSANT_DEVICE_ID);
String pendingFirmwarePath = "/pending_firmware.bin";
bool updateScheduled = false;

void setupTime() {
  configTime(TZ_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.printf("[Time] Current local time: %02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    Serial.println("[Time] Failed to obtain time");
  }
}

String getFinalURL(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String currentURL = url;
  int redirectCount = 0;

  while (redirectCount < 5) {
    if (!https.begin(client, currentURL)) return "";
    int code = https.GET();
    if (code >= 300 && code < 400) {
      currentURL = https.getLocation();
      Serial.println("[OTA] Redirected to: " + currentURL);
      https.end();
      redirectCount++;
    } else {
      https.end();
      break;
    }
  }
  return currentURL;
}

bool downloadFirmware(const String& firmwareUrl) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  Serial.println("[OTA] Resolving final firmware URL...");
  String finalURL = getFinalURL(firmwareUrl);
  if (finalURL == "") {
    Serial.println("[OTA] Failed to resolve redirect chain.");
    return false;
  }

  Serial.println("[OTA] Starting download from: " + finalURL);
  if (!https.begin(client, finalURL)) {
    Serial.println("[OTA] HTTPS begin failed!");
    return false;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA] HTTP GET failed, code: %d\n", httpCode);
    https.end();
    return false;
  }

  int contentLength = https.getSize();
  Serial.printf("[OTA] Downloading firmware (%d bytes)...\n", contentLength);

  File fwFile = SPIFFS.open(pendingFirmwarePath, FILE_WRITE);
  if (!fwFile) {
    Serial.println("[SPIFFS] Failed to open file for writing");
    https.end();
    return false;
  }

  WiFiClient *stream = https.getStreamPtr();
  uint8_t buff[256];
  int written = 0;
  while (https.connected() && (written < contentLength)) {
    size_t size = stream->available();
    if (size) {
      int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
      fwFile.write(buff, c);
      written += c;
    }
    delay(1);
  }

  fwFile.close();
  https.end();

  Serial.printf("[OTA] Firmware saved to SPIFFS (%d bytes)\n", written);
  return true;
}

void applyUpdateNow() {
  File fwFile = SPIFFS.open(pendingFirmwarePath);
  if (!fwFile) {
    Serial.println("[OTA] No firmware file found for immediate update.");
    return;
  }

  Serial.println("[OTA] Applying pending firmware now...");
  if (!Update.begin(fwFile.size())) {
    Serial.println("[OTA] Not enough space for update!");
    fwFile.close();
    return;
  }

  size_t written = Update.writeStream(fwFile);
  fwFile.close();

  if (Update.end() && Update.isFinished()) {
    Serial.println("[OTA] Update complete! Rebooting...");
    SPIFFS.remove(pendingFirmwarePath); // ✅ delete to free space
    delay(2000);
    ESP.restart();
  } else {
    Serial.printf("[OTA] Update failed: %s\n", Update.errorString());
  }
}

void checkAndApplyScheduledUpdate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  if (timeinfo.tm_hour == UPDATE_HOUR && timeinfo.tm_min == UPDATE_MINUTE) {
    if (SPIFFS.exists(pendingFirmwarePath)) {
      applyUpdateNow();
    }
  }
}

void handleCommand(LosantCommand *command) {
  Serial.printf("[Losant] Command received: %s\n", command->name);

  if (strcmp(command->name, "ota_update") == 0) {
    String firmwareUrl = (*command->payload)["url"].as<String>();
    String newVersion  = (*command->payload)["version"].as<String>();

    Serial.printf("[OTA] New version: %s\n", newVersion.c_str());
    Serial.printf("[OTA] Current version: %s\n", CURRENT_FIRMWARE_VERSION);

    if (newVersion != CURRENT_FIRMWARE_VERSION) {
      Serial.println("[OTA] New version detected — downloading now, will install at scheduled time.");
      if (downloadFirmware(firmwareUrl)) {
        updateScheduled = true;
      }
    } else {
      Serial.println("[OTA] Already on latest version.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed");
  }

  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected!");

  setupTime();

  wifiClient.setInsecure();
  device.connectSecure(wifiClient, LOSANT_ACCESS_KEY, LOSANT_ACCESS_SECRET);
  device.onCommand(&handleCommand);

  Serial.println("[Losant] Connected to Losant!");

  // ✅ Apply pending firmware immediately if found
  if (SPIFFS.exists(pendingFirmwarePath)) {
    Serial.println("[OTA] Pending firmware found at boot — applying immediately.");
    applyUpdateNow();
  }
}

void loop() {
  device.loop();

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 60000) {
    lastCheck = millis();

    if (updateScheduled || SPIFFS.exists(pendingFirmwarePath)) {
      checkAndApplyScheduledUpdate();
    }
  }
}
