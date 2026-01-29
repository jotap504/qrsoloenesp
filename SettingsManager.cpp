#include "SettingsManager.h"
#include <mbedtls/sha256.h>

SettingsManager::SettingsManager() {}

bool SettingsManager::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("CRITICAL: LittleFS Mount/Format Failed!");
  } else {
    Serial.printf("LittleFS mounted. Total: %u, Used: %u\n", (uint32_t)LittleFS.totalBytes(), (uint32_t)LittleFS.usedBytes());
  }

  // SD Logic for JC3248W535 (CS: 10, MOSI: 11, SCK: 12, MISO: 13)
  _sdSPI.begin(12, 13, 11, 10); // SCK, MISO, MOSI, SS
  if (!SD.begin(10, _sdSPI)) {
    Serial.println("SD Card Mount Failed");
    _sdAvailable = false;
  } else {
    Serial.println("SD Card Mounted successfully");
    _sdAvailable = true;
    
    // Ensure directories exist
    if (!SD.exists("/ads")) {
      SD.mkdir("/ads");
      Serial.println("Created /ads directory");
    }
    if (!SD.exists("/logs")) {
      SD.mkdir("/logs");
      Serial.println("Created /logs directory");
    }
  }

  return true; // We continue even if SD fails
}

bool SettingsManager::saveSettings(PaymentSettings settings[], int size) {
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("payments");

  for (int i = 0; i < size; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["amount"] = settings[i].amount;
    obj["duration"] = settings[i].durationMs;
  }

  doc["user"] = adminUser;
  doc["passHash"] = adminPassHash;
  doc["mpToken"] = mpAccessToken;
  doc["googleUrl"] = googleScriptUrl;
  doc["wifiSSID"] = wifiSSID;
  doc["wifiPass"] = wifiPass;
  doc["deviceName"] = deviceName;
  doc["opMode"] = operationMode;
  doc["unitPrice"] = pricePerUnit;
  doc["pulseDur"] = pulseDurationMs;
  doc["fwUrl"] = firmwareUrl;
  doc["fixedUnits"] = fixedUnits;
  doc["fixedModeType"] = fixedModeType;
  doc["staticQrText"] = staticQrText;
  doc["promoEn"] = promoEnabled;
  doc["promoThr"] = promoThreshold;
  doc["promoType"] = promoType;
  doc["promoVal"] = promoValue;

  File file = LittleFS.open(_filename, "w");
  if (!file)
    return false;
  serializeJson(doc, file);
  file.close();
  return true;
}

bool SettingsManager::loadSettings(PaymentSettings settings[], int size) {
  if (!LittleFS.exists(_filename))
    return false;

  File file = LittleFS.open(_filename, "r");
  if (!file)
    return false;

  StaticJsonDocument<2048> doc;
  deserializeJson(doc, file);

  JsonArray arr = doc["payments"];
  for (int i = 0; i < (int)size && i < (int)arr.size(); i++) {
    settings[i].amount = arr[i]["amount"];
    settings[i].durationMs = arr[i]["duration"];
  }

  if (doc.containsKey("user"))
    adminUser = doc["user"].as<String>();
  if (doc.containsKey("passHash"))
    adminPassHash = doc["passHash"].as<String>();
  if (doc.containsKey("mpToken"))
    mpAccessToken = doc["mpToken"].as<String>();
  if (doc.containsKey("googleUrl"))
    googleScriptUrl = doc["googleUrl"].as<String>();
  if (doc.containsKey("wifiSSID"))
    wifiSSID = doc["wifiSSID"].as<String>();
  if (doc.containsKey("wifiPass"))
    wifiPass = doc["wifiPass"].as<String>();
  if (doc.containsKey("deviceName"))
    deviceName = doc["deviceName"].as<String>();
  if (doc.containsKey("opMode"))
    operationMode = doc["opMode"];
  if (doc.containsKey("unitPrice"))
    pricePerUnit = doc["unitPrice"];
  if (doc.containsKey("pulseDur"))
    pulseDurationMs = doc["pulseDur"];
  if (doc.containsKey("fwUrl"))
    firmwareUrl = doc["fwUrl"].as<String>();
  if (doc.containsKey("fixedUnits"))
    fixedUnits = doc["fixedUnits"];
  if (doc.containsKey("fixedModeType"))
    fixedModeType = doc["fixedModeType"];
  if (doc.containsKey("staticQrText"))
    staticQrText = doc["staticQrText"].as<String>();
  if (doc.containsKey("promoEn"))
    promoEnabled = doc["promoEn"];
  if (doc.containsKey("promoThr"))
    promoThreshold = doc["promoThr"];
  if (doc.containsKey("promoType"))
    promoType = doc["promoType"];
  if (doc.containsKey("promoVal"))
    promoValue = doc["promoVal"];

  file.close();
  return true;
}

void SettingsManager::addLog(float amount, int duration, String ref) {
  // Log to LittleFS (Internal)
  File file = LittleFS.open(_logFilename, "a");
  if (file) {
    file.printf("%.2f,%d,%s\n", amount, duration / 1000, ref.c_str());
    file.close();
  }

  // Log to SD (External)
  if (_sdAvailable) {
    File sdFile = SD.open(_sdLogFilename, FILE_APPEND);
    if (sdFile) {
      sdFile.printf("%.2f,%d,%s\n", amount, duration / 1000, ref.c_str());
      sdFile.flush(); // Ensure data is physically written
      sdFile.close();
    }
  }
}

String SettingsManager::getLogs() {
  if (!LittleFS.exists(_logFilename))
    return "";
  File file = LittleFS.open(_logFilename, "r");
  if (!file)
    return "";
  String content = file.readString();
  file.close();
  return content;
}

void SettingsManager::clearLogs() {
  if (LittleFS.exists(_logFilename)) {
    LittleFS.remove(_logFilename);
  }
}

String SettingsManager::getSdLogs() {
  if (!_sdAvailable || !SD.exists(_sdLogFilename))
    return "";
  File file = SD.open(_sdLogFilename, FILE_READ);
  if (!file)
    return "";
  String content = file.readString();
  file.close();
  return content;
}

void SettingsManager::clearSdLogs() {
  if (_sdAvailable && SD.exists(_sdLogFilename)) {
    SD.remove(_sdLogFilename);
  }
}

String SettingsManager::listSdDir(String path) {
  if (!_sdAvailable) return "[]";
  File root = SD.open(path);
  if (!root || !root.isDirectory()) return "[]";
  
  String json = "[";
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      if (json.length() > 1) json += ",";
      json += "{\"name\":\"" + String(file.name()) + "\",\"size\":" + String(file.size()) + "}";
    }
    file = root.openNextFile();
  }
  json += "]";
  return json;
}

bool SettingsManager::deleteSdFile(String path) {
  if (!_sdAvailable) return false;
  return SD.remove(path);
}

size_t SettingsManager::getDirSize(String path) {
  if (!_sdAvailable) return 0;
  File root = SD.open(path);
  if (!root || !root.isDirectory()) return 0;
  
  size_t total = 0;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      total += file.size();
    }
    file = root.openNextFile();
  }
  return total;
}

String SettingsManager::getSha256(String input) {
  byte shaResult[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)input.c_str(),
                        input.length());
  mbedtls_sha256_finish(&ctx, shaResult);
  mbedtls_sha256_free(&ctx);

  String hash = "";
  for (int i = 0; i < 32; i++) {
    char buf[3];
    sprintf(buf, "%02x", shaResult[i]);
    hash += buf;
  }
  return hash;
}
