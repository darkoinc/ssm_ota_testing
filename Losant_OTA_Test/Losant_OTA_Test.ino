#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Losant.h>
#include <ArduinoJson.h>

// ==============================
// User Configuration
// ==============================
#define WIFI_SSID       "Elab"
#define WIFI_PASS       "2026summit"

#define LOSANT_DEVICE_ID     "690dfa233555b34f9d0cb8bd"
#define LOSANT_ACCESS_KEY    "73143249-63e0-4cb3-8ad1-9774bd2dc51c"
#define LOSANT_ACCESS_SECRET "5d6b577fffb2fb2c282cb154dbb04f9971baa396c626fc1d5a848b4eb87c8959"

// Current firmware version
#define CURRENT_FIRMWARE_VERSION "1.0.0"

// ==============================
// Globals
// ==============================
WiFiClientSecure wifiClient;
LosantDevice device(LOSANT_DEVICE_ID);

// ==============================
// OTA Update Function
// ==============================
void performOTA(const String& firmwareUrl) {
  Serial.println("[OTA] Starting update from: " + firmwareUrl);

  WiFiClientSecure client;
  client.setInsecure(); // ⚠️ In production, validate your cert.

  HTTPClient https;
  if (!https.begin(client, firmwareUrl)) {
    Serial.println("[OTA] HTTPS begin failed!");
    return;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA] HTTP GET failed, code: %d\n", httpCode);
    https.end();
    return;
  }

  int contentLength = https.getSize();
  bool canBegin = Update.begin(contentLength);

  if (!canBegin) {
    Serial.println("[OTA] Not enough space for update!");
    https.end();
    return;
  }

  WiFiClient *stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (Update.end()) {
    if (Update.isFinished()) {
      Serial.println("[OTA] Update complete! Rebooting...");
      https.end();
      delay(2000);
      ESP.restart();
    } else {
      Serial.println("[OTA] Update failed to finish!");
    }
  } else {
    Serial.printf("[OTA] Update error: %s\n", Update.errorString());
  }

  https.end();
}

// ==============================
// Losant Command Handler
// ==============================
void handleCommand(LosantCommand *command) {
  Serial.printf("[Losant] Command received: %s\n", command->name);

  if (strcmp(command->name, "ota_update") == 0) {
    String firmwareUrl = (*command->payload)["url"].as<String>();
    String newVersion  = (*command->payload)["version"].as<String>();

    Serial.printf("[OTA] New version: %s\n", newVersion.c_str());
    Serial.printf("[OTA] Current version: %s\n", CURRENT_FIRMWARE_VERSION);

    if (newVersion != CURRENT_FIRMWARE_VERSION) {
      Serial.println("[OTA] New version detected, starting update...");
      performOTA(firmwareUrl);
    } else {
      Serial.println("[OTA] Already on latest version.");
    }
  }
}

// ==============================
// Setup
// ==============================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected!");

  wifiClient.setInsecure(); // for testing
  device.connectSecure(wifiClient, LOSANT_ACCESS_KEY, LOSANT_ACCESS_SECRET);
  device.onCommand(&handleCommand);

  Serial.println("[Losant] Connected to Losant!");

  DynamicJsonDocument state(256);
  JsonObject root = state.to<JsonObject>();
  root["status"] = "online";
  root["version"] = CURRENT_FIRMWARE_VERSION;
  device.sendState(root);

  Serial.println(F("Device Updated"));
}

// ==============================
// Main Loop
// ==============================
void loop() {
  device.loop();

  static unsigned long lastPing = 0;
  if (millis() - lastPing > 60000) {
    lastPing = millis();
    DynamicJsonDocument heartbeat(64);
    JsonObject hb = heartbeat.to<JsonObject>();
    hb["heartbeat"] = true;
    device.sendState(hb);
  }
}
