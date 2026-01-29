#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

struct PaymentSettings {
  float amount;
  int durationMs;
};

class SettingsManager {
public:
  SettingsManager();
  bool begin();
  bool saveSettings(PaymentSettings settings[], int size);
  bool loadSettings(PaymentSettings settings[], int size);
  void addLog(float amount, int duration, String ref);
  String getLogs();
  void clearLogs();
  
  String getSdLogs();
  void clearSdLogs();
  bool isSdAvailable() { return _sdAvailable; }

  // New Image Management Methods
  String listSdDir(String path);
  bool deleteSdFile(String path);
  size_t getDirSize(String path);

  String adminUser = "admin";
  // Default hash for "admin" is:
  // 8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918
  String adminPassHash =
      "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918";

  String mpAccessToken = "";
  String googleScriptUrl = "";
  String wifiSSID = "";
  String wifiPass = "";
  String deviceName = "";
  int operationMode = 0; // 0: Time, 1: Credit
  float pricePerUnit = 10.0;
  int pulseDurationMs = 100;
  String firmwareUrl = "";

  // Fixed QR Mode Settings
  int fixedUnits = 1;
  int fixedModeType = 0; // 0: Time (Minutes), 1: Credit
  String staticQrText = "";

  // Promotion Settings
  bool promoEnabled = false;
  int promoThreshold = 0; // Units (minutes or credits)
  int promoType = 0;      // Always 0 (Discount %) - Bonus removed
  float promoValue = 0.0; // Percentage value

  static String getSha256(String input);

private:
  const char *_filename = "/settings.json";
  const char *_logFilename = "/logs.csv";
  const char *_sdLogFilename = "/logs_sd.csv";
  bool _sdAvailable = false;
  SPIClass _sdSPI;
};

#endif
