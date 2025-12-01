#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include "SPIFFS.h"
#include "mbedtls/sha256.h"
#include <ArduinoJson.h>

// WiFi credentials
const char* ssid = "Elab";
const char* password = "2026summit";

// Manifest URL
const char* manifestURL = "https://raw.githubusercontent.com/darkoinc/SSM_OTA_Host/refs/heads/main/manifest.json";

// Current firmware version
const char* currentVersion = "1.0.1";

// OTA settings
const int maxRetries = 3;

// Utility: compute SHA256 of a file in SPIFFS
String sha256File(File &file) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256

    uint8_t buffer[1024];
    while(file.available()){
        size_t len = file.read(buffer, sizeof(buffer));
        mbedtls_sha256_update(&ctx, buffer, len);
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    String hashStr = "";
    for(int i=0;i<32;i++){
        if(hash[i] < 16) hashStr += "0";
        hashStr += String(hash[i], HEX);
    }
    hashStr.toLowerCase();
    return hashStr;
}

// Download and verify firmware
bool downloadAndVerifyFirmware(const String &url, const String &expectedSha) {
    for(int attempt = 1; attempt <= maxRetries; attempt++) {
        Serial.printf("[OTA] Download attempt %d\n", attempt);

        HTTPClient http;
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.begin(url);
        int httpCode = http.GET();

        if(httpCode != HTTP_CODE_OK) {
            Serial.printf("[OTA] HTTP GET failed: %d\n", httpCode);
            http.end();
            delay(1000);
            continue;
        }

        int contentLength = http.getSize();
        if(contentLength <= 0) {
            Serial.println("[OTA] Invalid content length");
            http.end();
            return false;
        }

        if(!SPIFFS.begin(true)) {
            Serial.println("[OTA] SPIFFS mount failed");
            http.end();
            return false;
        }

        File file = SPIFFS.open("/pending_firmware.bin", FILE_WRITE);
        if(!file) {
            Serial.println("[OTA] Failed to open SPIFFS for writing");
            http.end();
            return false;
        }

        WiFiClient *stream = http.getStreamPtr();
        int written = 0;
        uint8_t buff[512];
        while(http.connected() && written < contentLength) {
            size_t availableBytes = stream->available();
            if(availableBytes) {
                size_t toRead = availableBytes > sizeof(buff) ? sizeof(buff) : availableBytes;
                int readBytes = stream->readBytes(buff, toRead);
                file.write(buff, readBytes);
                written += readBytes;
                Serial.printf("[OTA] Downloaded %d/%d bytes\n", written, contentLength);
            }
            delay(1);
        }

        file.close();
        http.end();

        // Verify SHA256
        File verifyFile = SPIFFS.open("/pending_firmware.bin");
        String computedSha = sha256File(verifyFile);
        verifyFile.close();

        Serial.printf("[SHA256] Computed: %s\n", computedSha.c_str());
        Serial.printf("[SHA256] Expected: %s\n", expectedSha.c_str());

        if(computedSha == expectedSha) {
            Serial.println("[OTA] SHA256 verified!");
            return true;
        } else {
            Serial.println("[OTA] SHA256 mismatch! Deleting file and retrying...");
            SPIFFS.remove("/pending_firmware.bin");
        }
    }
    return false;
}

// Apply firmware
bool applyFirmware() {
    File updateFile = SPIFFS.open("/pending_firmware.bin");
    if(!updateFile) {
        Serial.println("[OTA] No firmware file to apply");
        return false;
    }

    if(Update.begin(updateFile.size())) {
        size_t written = Update.writeStream(updateFile);
        if(written == updateFile.size()) {
            Serial.println("[OTA] Firmware written successfully. Rebooting...");
            if(Update.end()) {
                updateFile.close();
                SPIFFS.remove("/pending_firmware.bin");
                ESP.restart();
                return true;
            } else {
                Serial.printf("[OTA] Update end failed: %s\n", Update.errorString());
            }
        } else {
            Serial.println("[OTA] Write size mismatch");
        }
    } else {
        Serial.printf("[OTA] Update begin failed: %s\n", Update.errorString());
    }
    updateFile.close();
    return false;
}

// Fetch manifest
bool fetchManifest(String &version, String &url, String &sha256) {
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(manifestURL);
    int httpCode = http.GET();

    if(httpCode != HTTP_CODE_OK) {
        Serial.printf("[Manifest] HTTP GET failed: %d\n", httpCode);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if(error) {
        Serial.printf("[Manifest] JSON parse failed: %s\n", error.c_str());
        return false;
    }

    version = doc["version"].as<String>();
    url     = doc["url"].as<String>();
    sha256  = doc["sha256"].as<String>();

    Serial.printf("[Manifest] version=%s url=%s sha256=%s\n", version.c_str(), url.c_str(), sha256.c_str());
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.print(F("Current FW Version: "));
    Serial.println(F(currentVersion));

    WiFi.begin(ssid, password);
    Serial.println("[WiFi] Connecting...");
    while(WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected!");

    String availableVersion, firmwareURL, expectedSha;
    if(fetchManifest(availableVersion, firmwareURL, expectedSha)) {
        if(availableVersion != currentVersion) {
            Serial.printf("[BootCheck] New version available: %s (current: %s). Downloading...\n", availableVersion.c_str(), currentVersion);
            if(downloadAndVerifyFirmware(firmwareURL, expectedSha)) {
                applyFirmware();
            } else {
                Serial.println("[BootCheck] Download or verification failed.");
            }
        } else {
            Serial.println("[BootCheck] Firmware up-to-date.");
        }
    } else {
        Serial.println("[BootCheck] Failed to fetch manifest.");
    }
}

void loop() {
    // Optional: add periodic update checks here later
}
