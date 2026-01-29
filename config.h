#ifndef CONFIG_H
#define CONFIG_H

// WiFi Credentials (Default)
const char *WIFI_SSID = "Fibrasky";
const char *WIFI_PASS = "corsa000";

// Hardware Pins
const int PIN_LED = -1;    // Built-in LED disabled (GPIO2 needed for Audio I2S LRC)
const int PIN_RELAY = 14; // Relay on GPIO14 (GPIO4 conflicts with Touch SDA)
const int PIN_CONFIG_BUTTON = 0; // Hold for 5s to enter AP Mode

// Google Sheets Logging
const char *GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/"
                                "AKfycbyXry0AVjUcz0V9VGMNM1sDmUS8l1vqD5BMYUx6or"
                                "daEShoUYLVAVvsaIZ2YHPe230n0A/exec";

// Mercado Pago Credentials (To be filled by user)
// Get your Access Token from: https://www.mercadopago.com.ar/developers/panel
const char *MP_ACCESS_TOKEN =
    "APP_USR-5058881085546506-011409-dbce6c4c48b10642c20730b240a96896-77633508";

// System Settings
// Secure OTA Configuration
// 32-byte AES Key (64 hex characters) - CHANGE THIS IN PRODUCTION
const char *OTA_AES_KEY = "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4";
// Secret Token for update requests
const char *OTA_AUTH_TOKEN = "private_update_token_12345";
// Secret code to trigger AP Mode from keypad
const int AP_SECRET_CODE = 987654;

#endif
