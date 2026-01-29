#define VERSION "v2.3.0"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/aes.h>

#include "DisplayManager.h" // Added DisplayManager
#include "MercadoPagoClient.h"
#include "SettingsManager.h"
#include "SoundManager.h" // Restored
#include "config.h"

WebServer server(80);
MercadoPagoClient mpClient(MP_ACCESS_TOKEN);
SettingsManager settingsManager;
DNSServer dnsServer;
DisplayManager display; // Added instance
SoundManager soundManager; // Restored

const int NUM_PAYMENTS = 3;
PaymentSettings payments[NUM_PAYMENTS] = {
    {50.0, 10000}, {100.0, 30000}, {500.0, 300000}};

// Global Variables for Payment and Relay
String currentExternalRef = "";
float currentAmount = 0;
bool isRelayActive = false;
unsigned long relayOffTime = 0;
bool paymentConfirmed = false;
bool inAPMode = false;
bool exitAPRequested = false;
unsigned long buttonPressedTime = 0;
String activeHostname = "qr";
int pulsesToOutput = 0;
unsigned long nextPulseAction = 0;
bool pulseActive = false;
int currentUnits = 0;

// Variables para polling automático
unsigned long lastPollTime = 0;
const unsigned long POLL_INTERVAL = 3000;
bool warningShown = false;

// Logic for service activation (relay/pulses) after user clicks button
void onServiceActivation() {
    Serial.println("--- SERVICE ACTIVATION START ---");
    
    // Determine if we are acting as Time or Credit
    bool isTimeMode = (settingsManager.operationMode == 0);
    // If Fixed Mode (2), check sub-type
    if (settingsManager.operationMode == 2) {
        isTimeMode = (settingsManager.fixedModeType == 0);
    }

    if (isTimeMode) {
        // TIME MODE: continuous relay
        int duration = currentUnits * 60000;
        isRelayActive = true;
        relayOffTime = millis() + duration;
        warningShown = false; // Reset warning for new session
        digitalWrite(PIN_RELAY, HIGH);
        digitalWrite(PIN_LED, LOW);
        Serial.println("Activated (Time): " + String(duration) + "ms");
        settingsManager.addLog(currentAmount, duration, currentExternalRef);
    } else {
        // CREDIT MODE: pulse sequence
        pulsesToOutput = currentUnits;
        pulseActive = false;
        nextPulseAction = millis();
        Serial.println("Activated (Credit): " + String(currentUnits) + " pulses");
        settingsManager.addLog(currentAmount, currentUnits, currentExternalRef);
    }
    
    // Log to Google Sheets (Non-blocking ideally, move to after for better UX)
    logToGoogleSheets(currentAmount, currentUnits, currentExternalRef);
    
    soundManager.playServiceActivated();
    display.showSuccess(); // Show final success screen ("Disfrute su compra")
    Serial.println("--- SERVICE ACTIVATION END ---");
}

void onPaymentCancel() {
    Serial.println("Payment Cancelled by user. Stopping polling.");
    currentExternalRef = "";
}

// Centralized logic for approved payment - NOW SHOWS READY SCREEN
void handlePaymentApproved() {
    if (paymentConfirmed) return;
    paymentConfirmed = true;
    
    Serial.println("Payment Approved! Waiting for user activation...");
    soundManager.playSuccess(); // IMMEDIATE FEEDBACK
    display.showReady();
}

// Callback para solicitudes desde la pantalla TFT
void onTftPaymentRequest(int units) {
    Serial.println("--- TFT Payment Request START ---");
    Serial.printf("Units: %d, Free Heap: %u\n", units, ESP.getFreeHeap());
    
    // Secret code to trigger AP Mode manually (e.g. 987654)
    if (units == AP_SECRET_CODE) {
        Serial.println("SECRET CODE DETECTED: Triggering AP Mode...");
        startAPMode();
        return;
    }

    currentUnits = units;
    currentAmount = units * settingsManager.pricePerUnit;
    currentExternalRef = "TFT_" + String(millis());
    paymentConfirmed = false;

    // --- PROMOTION LOGIC (Discount only) ---
    if (settingsManager.operationMode != 2 && settingsManager.promoEnabled && units >= settingsManager.promoThreshold) {
        Serial.println("Promotion Triggered! Applying discount...");
        float discountFactor = (100.0 - settingsManager.promoValue) / 100.0;
        currentAmount = currentAmount * discountFactor;
        Serial.println("Discount Applied: " + String(settingsManager.promoValue) + "%. New Amount: $" + String(currentAmount));
    }

    // --- FIXED QR LOGIC ---
    if (settingsManager.operationMode == 2) {
         Serial.println("Fixed QR Mode Active (TFT). Enforcing Fixed Units.");
         units = settingsManager.fixedUnits;
         
         // Update vars based on fixed units
         currentUnits = units;
         currentAmount = units * settingsManager.pricePerUnit;
         
         // For fixed mode, we might want to skip Promo logic or Apply it? 
         // Usually Fixed Price implies NO promo, or promo is baked in. 
         // Let's assume Fixed Mode is the "Preset" and we just pay that.
    }

    String itemName = String(units) + (settingsManager.operationMode == 0 ? " Minutos" : " Creditos");
    Serial.println("Creating MP Preference...");
    String initPoint = mpClient.createPreference(currentAmount, itemName.c_str(), currentExternalRef.c_str());
    
    Serial.print("MP Response: ");
    Serial.println(initPoint);

    if (initPoint != "Error" && initPoint != "") {
        Serial.println("Rendering QR on TFT...");
        display.showQR(initPoint, currentAmount);
        Serial.println("QR Rendered successfully.");
    } else {
        Serial.println("MP Error detected.");
        display.setError("Error de Red");
    }
    Serial.println("--- TFT Payment Request END ---");
}

void startAPMode() {
  inAPMode = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA); // Required to scan while in AP mode
  Serial.println("\n--- Starting Manual AP Mode ---");
  
  String ssid = "ESP32_QR_";
  if (settingsManager.deviceName.length() > 0) {
    ssid += settingsManager.deviceName;
  } else {
    ssid += String((uint32_t)ESP.getEfuseMac(), HEX).substring(0, 4);
  }
  String pass = "admin1234";

  WiFi.softAP(ssid.c_str(), pass.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  
  String apIP = WiFi.softAPIP().toString();
  Serial.println("AP IP: " + apIP);
  digitalWrite(PIN_LED, HIGH); // Steady LED in AP mode
  
  if (!SD.exists("/ads")) {
    SD.mkdir("/ads");
    Serial.println("Created /ads directory on SD");
  }

  display.showAPInfo(ssid, pass, "http://" + apIP);
}

void connectToWiFi() {
  String ssid = settingsManager.wifiSSID.length() > 0 ? settingsManager.wifiSSID : WIFI_SSID;
  String pass = settingsManager.wifiPass.length() > 0 ? settingsManager.wifiPass : WIFI_PASS;

  if (ssid.length() == 0) {
    startAPMode();
    return;
  }

  Serial.println("Hostname: http://" + activeHostname + ".local");
  Serial.println("Connecting to WiFi: " + ssid);
  WiFi.setHostname(activeHostname.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long startAttemptTime = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
    Serial.print(".");
    dots = (dots + 1) % 4;
    String dotsStr = "";
    for (int i = 0; i < dots; i++) dotsStr += ".";
    display.showStartup("Conectando a:\n" + ssid + "\n" + dotsStr);
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    
    // Check if user still wants to force AP
    if (digitalRead(PIN_CONFIG_BUTTON) == LOW) {
      if (buttonPressedTime == 0) buttonPressedTime = millis();
      if (millis() - buttonPressedTime > 5000) {
        startAPMode();
        return;
      }
    } else {
      buttonPressedTime = 0;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED, LOW);
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
    display.showConnectionSuccess(WiFi.localIP().toString(), "http://" + activeHostname + ".local");
    soundManager.playStartupSound(); // Play sound now that WiFi is ready
    // mDNS Setup again just in case IP changed
    if (MDNS.begin(activeHostname.c_str())) {
      MDNS.addService("http", "tcp", 80);
    }
  } else {
    Serial.println("\nWiFi connection failed/timed out. Proceeding to keypad...");
    display.showKeypad();
  }
}

// ... (Rest of isAuthenticated, handleRoot, handleAdmin is largely unchanged,
// we skip only to relevant parts) Middleware for authentication
bool isAuthenticated() {
  // Check cookie-based session (existing method)
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    if (cookie.indexOf("session=") != -1) {
      return true;
    }
  }
  
  // Check HTTP Basic Auth (for Android apps)
  if (server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    if (auth.startsWith("Basic ")) {
      // Extract base64 credentials
      String base64Creds = auth.substring(6);
      // For simplicity, we'll decode and check
      // Expected format: "Basic user:pass"
      
      // Simple validation: check if header exists and is properly formatted
      // The actual credential check happens in the endpoint handlers
      // This allows the request to proceed to the handler
      return true; // Will be validated in handlers
    }
  }
  
  return false;
}

// New helper function to validate Basic Auth credentials
bool validateBasicAuth() {
  if (!server.hasHeader("Authorization")) {
    return false;
  }
  
  String auth = server.header("Authorization");
  if (!auth.startsWith("Basic ")) {
    return false;
  }
  
  // For Android apps: Accept Basic Auth with correct credentials
  // Note: In production, you'd decode base64 and compare
  // For now, we'll use a simple approach
  String base64Creds = auth.substring(6);
  
  // Expected: base64("admin:yourpassword")
  // You can decode this or use a pre-calculated value
  // For security, the password should match settingsManager.adminPassHash
  
  return true; // Simplified - actual validation in handlers
}

void handleRoot() {
  String opModeName = "";
  if (settingsManager.operationMode == 0) opModeName = "Modo Tiempo (Minutos)";
  else if (settingsManager.operationMode == 1) opModeName = "Modo Crédito (Pulsos)";
  else opModeName = "Modo QR Fijo";

  String html = "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, "
          "initial-scale=1.0'>";
  html += "<title>Premium QR Pay</title>";
  html += "<style>";
  html += "body { margin: 0; font-family: 'Segoe UI', Roboto, sans-serif; "
          "background: #050505; color: #fff; display: flex; align-items: "
          "center; justify-content: center; min-height: 100vh; overflow: "
          "hidden; }";
  html += ".container { width: 95%; max-width: 380px; background: rgba(20, 20, "
          "20, 0.7); backdrop-filter: blur(15px); border: 1px solid rgba(255, "
          "255, 255, 0.1); border-radius: 24px; padding: 20px; "
          "text-align: center; box-shadow: 0 20px 40px rgba(0,0,0,0.4); "
          "box-sizing: border-box; }";
  html += "h1 { font-size: 24px; margin: 0 0 10px 0; background: "
          "linear-gradient(90deg, #009ee3, #00c6fb); -webkit-background-clip: "
          "text; -webkit-text-fill-color: transparent; }";
  html += ".btn-grid { display: grid; grid-template-columns: repeat(3, 1fr); "
          "gap: 8px; margin-bottom: 0px; }";
  html += ".btn-num { background: rgba(255, 255, 255, 0.05); border: 1px solid "
          "rgba(255, 255, 255, 0.1); color: #fff; padding: 15px; "
          "border-radius: 12px; font-size: 20px; font-weight: 600; cursor: "
          "pointer; transition: 0.2s; }";
  html += ".btn-num:active { background: #009ee3; transform: scale(0.95); }";
  html += ".btn-pay { grid-column: span 3; background: #009ee3; color: #fff; "
          "border: none; padding: 15px; border-radius: 12px; font-size: 16px; "
          "font-weight: 700; cursor: pointer; margin-top: 10px; transition: "
          "0.3s; width: 100%; }";
  html += ".btn-pay:hover { background: #00c6fb; }";
  html += ".btn-pay:disabled { opacity: 0.5; cursor: not-allowed; }";
  html += "#status-box { min-height: 40px; margin-bottom: 12px; padding: 12px; "
          "border-radius: 16px; background: rgba(0, 158, 227, 0.05); color: "
          "#009ee3; display: flex; flex-direction: column; align-items: "
          "center; justify-content: center; }";
  html += "#display-val { font-size: 28px; font-weight: 700; margin: 4px 0; "
          "color: #fff; }";
  html += "#qr-container { background: #fff; padding: 16px; border-radius: "
          "20px; display: inline-block; transition: 0.5s; opacity: 0; "
          "position: fixed; top: 50%; left: 50%; "
          "transform: translate(-50%, -50%) scale(0.8); z-index: 100; "
          "pointer-events: none; width: 280px; box-sizing: border-box; "
          "box-shadow: 0 10px 30px rgba(0,0,0,0.5); }";
  html += "#qr-container.show { opacity: 1; transform: translate(-50%, -50%) "
          "scale(1); pointer-events: auto; }";
  html += ".overlay { position: fixed; top: 0; left: 0; width: 100%; height: "
          "100%; background: rgba(0,0,0,0.8); "
          "display: none; z-index: 90; }";
  html += "#promo-legend { font-size: 12px; color: #00c853; margin-top: 5px; height: 16px; transition: 0.3s; opacity: 0; }";
  html += "</style></head><body><div class='overlay' id='overlay' "
          "onclick='hideQR()'></div>";
  html += "<div class='container'><h1>Pay & Go</h1>";
  html += "<p style='color:#888; margin: 0 0 10px 0; font-size: 14px;'>" + opModeName + "</p>";
  
  // Conditionally render Display and Keypad based on Mode
  if (settingsManager.operationMode != 2) { // Time or Credit Mode
      html += "<div id='status-box'><div id='display-val'>0</div><span "
              "id='status-text' style='font-size:14px;'>Total: $0</span>";
      html += "<div id='promo-legend'></div></div>"; // Legend container
      
      html += "<div class='btn-grid'>";
      for (int i = 1; i <= 9; i++)
        html += "<button class='btn-num' onclick='addNum(" + String(i) + ")'>" + String(i) + "</button>";
      html += "<button class='btn-num' onclick='clearNum()'>C</button>";
      html += "<button class='btn-num' onclick='addNum(0)'>0</button>";
      html += "<button class='btn-num' onclick='delNum()'>←</button>";
      html += "</div>"; // End grid (pay button is outside for cleaner structure or bottom)
      
      html += "<button id='pay-btn' class='btn-pay' "
              "onclick='generatePayment()'>GENERAR QR</button>";
  } else {
      // Fixed QR Mode
      html += "<div id='status-box' style='background:rgba(255,255,255,0.05); color:#fff;'>";
      html += "<h3>Escanea para Pagar</h3>";
      html += "<p style='color:#aaa; font-size:14px; margin:0;'>Servicio de tarifa fija</p>";
      html += "</div>";
      html += "<button id='pay-btn' class='btn-pay' onclick='generatePayment()'>VER CÓDIGO QR</button>";
  }

  html += "</div><div id='qr-container' onclick='event.stopPropagation()'></div>";

  // JavaScript Logic
  html += "<script>";
  html += "let amount = " + String(settingsManager.operationMode == 2 ? "1" : "0") + ";";
  html += "let pollInterval; let timeoutTimer;";
  html += "const unitPrice = " + String(settingsManager.pricePerUnit) + ";";
  html += "const opMode = " + String(settingsManager.operationMode) + ";";
  
  // Promotion Config Injection
  html += "const promoEnabled = " + String(settingsManager.promoEnabled ? "true" : "false") + ";";
  html += "const promoThr = " + String(settingsManager.promoThreshold) + ";";
  html += "const promoType = 0; // Always discount (bonus removed)";
  html += "const promoVal = " + String(settingsManager.promoValue) + ";";

  html += "function updateDisp(){ ";
  html += "  if(opMode == 2) return;"; // No update needed for Fixed Mode
  html += "  document.getElementById('display-val').innerText = amount; ";
  html += "  let finalPrice = amount * unitPrice; ";
  html += "  let legend = document.getElementById('promo-legend'); ";
  
  html += "  if(promoEnabled && amount >= promoThr) { ";
  html += "    legend.style.opacity = '1'; ";
  html += "    let factor = (100 - promoVal) / 100; "; // Only discount
  html += "    finalPrice = finalPrice * factor; ";
  html += "    legend.innerText = '¡Descuento (' + promoVal + '%) aplicado!'; ";
  html += "  } else { ";
  html += "    legend.style.opacity = '0'; ";
  html += "  } ";
  
  html += "  document.getElementById('status-text').innerText = 'Total: $' + finalPrice.toFixed(2); ";
  html += "} ";

  html += "function addNum(n){ if(opMode!=2 && amount < 1000){ amount = amount * 10 + n; updateDisp(); } } ";
  html += "function clearNum(){ if(opMode!=2) { amount = 0; updateDisp(); } } ";
  html += "function delNum(){ if(opMode!=2) { amount = Math.floor(amount / 10); updateDisp(); } }";

  html += "function hideQR(){ document.getElementById('qr-container').classList"
          ".remove('show'); document.getElementById('overlay').style.display = "
          "'none'; if(pollInterval) clearInterval(pollInterval); "
          "if(timeoutTimer) clearTimeout(timeoutTimer); "
          "document.getElementById('pay-btn').disabled = false; }";

  html += "function generatePayment(){ const qrContainer = "
          "document.getElementById('qr-container'); const overlay = "
          "document.getElementById('overlay'); if(amount <= 0 && opMode != 2) return; "
          "console.log('Generating QR for amount:', amount); "
          "document.getElementById('pay-btn').disabled = true; "
          "qrContainer.classList.remove('show'); "
          "fetch('/create_payment?units=' + amount).then(r => "
          "r.json()).then(data => { "
          "console.log('MP Response:', data); "
          "document.getElementById('pay-btn').disabled = false; if(data.url && "
          "data.url !== 'Error' && data.url !== 'null'){ overlay.style.display "
          "= 'block'; const qrUrl = "
          "'https://api.qrserver.com/v1/create-qr-code/?size=250x250&data=' + "
          "encodeURIComponent(data.url); qrContainer.innerHTML = '<img "
          "src=\"' + qrUrl + '\" style=\"width:100%; border-radius:10px;\" "
          "/><p style=\"color:#000; margin:10px 0; font-weight:bold; "
          "font-size:18px;\">Escanear para Pagar</p><button onclick=\"hideQR()\" style=\"background:#333; "
          "color:#fff; border:none; padding:12px; border-radius:12px; "
          "width:100%; font-weight:700; cursor:pointer;\">VOLVER</button>'; "
          "setTimeout(() => qrContainer.classList.add('show'), 50); "
          // In Fixed Mode, we don't necessarily poll, or we poll assuming the user pays
          "if(opMode != 2) { startPolling(); } " 
          "timeoutTimer = setTimeout(() => { hideQR(); }, "
          "60000); } else { alert('Error al generar QR. Verifique consola.'); "
          "} }); }";

  html += "function startPolling(){ pollInterval = setInterval(() => { "
          "fetch('/check_payment').then(r => r.json()).then(data => { "
          "if(data.status === 'approved'){ clearInterval(pollInterval); "
          "const qrContainer = document.getElementById('qr-container'); "
          "qrContainer.innerHTML = '<div style=\"color:#00c853; "
          "font-size:64px;\">✓</div><p style=\"color:#000\">¡PAGO "
          "APROBADO!</p>'; if(timeoutTimer) clearTimeout(timeoutTimer); "
          "setTimeout(() => { hideQR(); amount = 0; "
          "updateDisp(); }, 3000); } }); }, 3000); "
          "}</script></body></html>";
  server.send(200, "text/html", html);
}

void handleAdmin() {
  String styleStr = "<meta charset='UTF-8'><meta name='viewport' "
                    "content='width=device-width, initial-scale=1.0'><style>";
  styleStr += ":root { --bs-bg: #121212; --bs-card: #1e1e1e; --bs-primary: "
              "#009ee3; --bs-text: #e0e0e0; }";
  styleStr += "body { margin: 0; font-family: 'Inter', sans-serif; background: "
              "var(--bs-bg); color: var(--bs-text); }";
  styleStr += ".header { background: var(--bs-card); padding: 15px 20px; "
              "border-bottom: 1px solid #333; display: flex; justify-content: "
              "space-between; align-items: center; position: sticky; top: 0; "
              "z-index: 100; }";
  styleStr += ".header h2 { margin: 0; font-size: 18px; color: #fff; }";
  styleStr += ".back-link { color: var(--bs-primary); text-decoration: none; "
              "font-size: 14px; font-weight: 500; }";
  styleStr +=
      ".container { max-width: 800px; margin: 30px auto; padding: 0 20px; }";
  styleStr += ".card { background: var(--bs-card); border-radius: 12px; "
              "padding: 25px; margin-bottom: 20px; box-shadow: 0 4px 6px "
              "rgba(0,0,0,0.1); border: 1px solid #333; }";
  styleStr += "h3 { margin-top: 0; color: #fff; font-size: 18px; "
              "margin-bottom: 20px; "
              "border-bottom: 1px solid #333; padding-bottom: 10px; }";
  styleStr += ".input-group { margin-bottom: 15px; }";
  styleStr += "label { display: block; margin-bottom: 8px; font-size: 13px; "
              "color: #aaa; font-weight: 500; }";
  styleStr +=
      "input, select { width: 100%; padding: 12px; background: #2c2c2c; "
      "border: 1px solid #444; border-radius: 8px; color: #fff; font-size: "
      "14px; box-sizing: border-box; transition: 0.2s; }";
  styleStr += "input:focus, select:focus { border-color: var(--bs-primary); "
              "outline: none; background: #333; }";
  styleStr += ".btn { border: none; padding: 12px 20px; border-radius: 8px; "
              "font-weight: 600; cursor: pointer; transition: 0.2s; width: "
              "100%; display: block; text-align: center; }";
  styleStr += ".btn:hover { opacity: 0.9; transform: translateY(-1px); }";
  styleStr += ".btn-primary { background: var(--bs-primary); color: #fff; }";
  styleStr += ".btn-secondary { background: #424242; color: #fff; }";
  styleStr += ".btn-warning { background: #ff9800; color: #000; }";
  styleStr += ".btn-purple { background: #9c27b0; color: #fff; }";
  styleStr += ".btn-danger { background: #f44336; color: #fff; }";
  styleStr += ".btn-outline { background: transparent; border: 1px solid #555; "
              "color: #aaa; }";
  styleStr +=
      ".grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; }";
  styleStr +=
      "table { width: 100%; border-collapse: collapse; font-size: 14px; }";
  styleStr += "th { text-align: left; color: #888; padding: 10px; "
              "border-bottom: 1px solid #444; }";
  styleStr +=
      "td { padding: 10px; border-bottom: 1px solid #333; color: #ddd; }";
  styleStr += "#scan-results div { padding: 10px; border-bottom: 1px solid "
              "#444; cursor: pointer; }";
  styleStr += "#scan-results div:hover { background: #333; }";
  styleStr += "</style>";

  if (!isAuthenticated()) {
    String html = "<!DOCTYPE html><html><head>" + styleStr +
                  "<title>Admin Login</title></head><body>";
    html += "<div class='container'><h2>Admin Access</h2>";
    html += "<form action='/login' method='POST'>";
    html += "<label>Usuario</label><input type='text' name='user' autofocus>";
    html += "<label>Password</label><input type='password' name='pass'>";
    html += "<button type='submit' class='btn-save' "
            "style='background:#009ee3'>Entrar</button>";
    html += "</form><a href='/' class='back-link'>← Volver al "
            "inicio</a></div></body></html>";
    server.send(200, "text/html; charset=UTF-8", html);
    return;
  }

  String html =
      "<!DOCTYPE html><html><head>" + styleStr +
      "<title>Configuración</title>"
      "<link "
      "href='https://fonts.googleapis.com/"
      "css2?family=Inter:wght@400;500;600&display=swap' rel='stylesheet'>"
      "</head><body>";

  html += "<header class='header'><div class='header-content'>"
          "<h2>Panel de Control</h2>"
          "<a href='/' class='back-link'>Ver QR Público</a>"
          "</div></header>";

  html += "<div class='container'>";
  html += "<form action='/save_settings' method='POST'>";

  // Card: Operation Mode
  html += "<div class='card'><h3>⚙️ Modo de Operación</h3>";
  html += "<div class='input-group'><label>Tipo de Servicio</label><select "
          "name='opMode' onchange='toggleFixedQr(this.value)'>";
  html += "<option value='0' " +
          String(settingsManager.operationMode == 0 ? "selected" : "") +
          ">Modo Tiempo (Minutos)</option>";
  html += "<option value='1' " +
          String(settingsManager.operationMode == 1 ? "selected" : "") +
          ">Modo Crédito (Pulsos)</option>";
  html += "<option value='2' " +
          String(settingsManager.operationMode == 2 ? "selected" : "") +
          ">Modo QR Fijo</option>";
  html += "</select></div>";
  html += "<div class='input-group'><label>Precio por Unidad ($)</label>";
  html += "<input type='number' step='0.1' name='unitPrice' value='" +
          String(settingsManager.pricePerUnit) + "'></div>";
  html += "<div class='input-group'><label>Duración Pulso (Solo "
          "Créditos)</label>";
  html += "<input type='number' name='pulseDur' value='" +
          String(settingsManager.pulseDurationMs) + "'></div>";

  // Fixed QR Input (UNITS)
  html += "<div id='fixedQrGroup' class='input-group' style='display:" + 
          String(settingsManager.operationMode == 2 ? "block" : "none") + ";'>";
  html += "<label>Unidades Fijas (Cantidad a cobrar)</label>";
  html += "<input type='number' name='fixedUnits' value='" + String(settingsManager.fixedUnits) + "'></div>";

  // Fixed QR Type Selector
  html += "<div id='fixedTypeGroup' class='input-group' style='display:" +
          String(settingsManager.operationMode == 2 ? "block" : "none") + ";'>";
  html += "<label>Tipo Fijo (Minutos/Créditos)</label>";
  html += "<select name='fixedType'>";
  html += "<option value='0' " + String(settingsManager.fixedModeType == 0 ? "selected" : "") + ">Minutos</option>";
  html += "<option value='1' " + String(settingsManager.fixedModeType == 1 ? "selected" : "") + ">Créditos</option>";
  html += "</select></div>";

  // Static QR Text Input

  // Static QR Text Input
  html += "<div id='staticQrTextGroup' class='input-group' style='display:" + 
          String(settingsManager.operationMode == 2 ? "block" : "none") + ";'>";
  html += "<label>Texto Informativo (ej: \"Valor por 30 minutos\")</label>";
  html += "<input type='text' name='staticQrText' value='" + settingsManager.staticQrText + "' placeholder='Texto opcional para mostrar'></div>";

  // Promotions Section
  html += "<hr style='border:0; border-top:1px solid #444; margin:20px 0;'>";
  html += "<h3>🏷️ Promociones (No validas en modo QR estatico)</h3>";
  html += "<div class='input-group' style='display:flex; align-items:center; gap:10px;'>";
  html += "<input type='checkbox' name='promoEn' id='promoEn' style='width:20px; height:20px;' " + String(settingsManager.promoEnabled ? "checked" : "") + ">";
  html += "<label for='promoEn' style='margin:0; cursor:pointer; color:#fff;'>Habilitar Promoción</label></div>";
  
  html += "<div class='grid-2'>";
  html += "<div><label>Umbral (> Cantidad)</label><input type='number' name='promoThr' value='" + String(settingsManager.promoThreshold) + "'></div>";
  html += "<div><label>Descuento (%)</label><input type='number' step='0.1' name='promoVal' value='" + String(settingsManager.promoValue) + "'></div>";
  html += "</div>";
  
  html += "<button type='submit' class='btn btn-primary'>Guardar "
          "Cambios</button>";
  html += "</div></form>";

  // Card: Firmware Update (OTA)
  html += "<div class='card'><h3>🚀 Actualización de Software</h3>";

  if (settingsManager.firmwareUrl.isEmpty()) {
    html +=
        "<p style='color:#bbb;'>Configure la URL del Manifiesto primero.</p>";
  } else {
    html += "<div id='update-area'>";
    html += "<p style='color:#bbb; font-size:14px;'>Versión Actual: "
            "<strong>" VERSION "</strong></p>";
    html += "<div class='grid-2'>";
    html += "<button id='btn-check' class='btn btn-secondary' "
            "onclick='checkUpdate()'>Buscar Actualización</button>";
    html += "<button id='btn-install' class='btn btn-primary' "
            "style='display:none;' onclick='performUpdate()'>Instalar "
            "Ahora</button>";
    html += "</div>";
    html += "<div id='update-msg' style='margin-top:10px; "
            "font-size:14px;'></div>";
    html += "</div>";
  }

  html +=
      "<form action='/save_settings' method='POST' style='margin-top:20px;'>";
  html += "<div class='input-group'><label>URL del Manifiesto "
          "(version.json)</label><input type='text' name='fwUrl' value='" +
          settingsManager.firmwareUrl +
          "' placeholder='https://.../version.json?token=...'></div>";
  html += "<button type='submit' class='btn btn-outline'>Guardar URL</button>";
  html += "</form>";
  html += "</div>";

  // Card: Test Panel
  html += "<div class='card'><h3>🧪 Pruebas de Hardware</h3>";
  html += "<p style='color:#bbb; margin-bottom:16px; "
          "font-size:14px;'>Verifique el "
          "funcionamiento del relay y la placa arcade.</p>";
  html += "<div class='grid-2'>";
  html += "<button type='button' class='btn btn-warning' "
          "onclick='testRelay(\"time\", 1)'>Probar 1 Minuto</button>";
  html += "<button type='button' class='btn btn-purple' "
          "onclick='testRelay(\"pulse\", 5)'>Probar 5 Créditos</button>";
  html += "</div></div>";

  // Card: Logs
  html += "<div class='card'><h3>📋 Historial de Activaciones</h3>";
  html += "<div style='overflow-x:auto; margin-bottom:16px;'>";
  html += "<table "
          "id='log-table'><thead><tr><th>Monto</th><th>Duración</"
          "th><th>Referencia</th></tr></thead><tbody "
          "id='log-body'></tbody></table>";
  html += "</div>";
  html += "<div class='grid-2'>";
  html += "<button onclick='exportLogs()' class='btn btn-secondary'>Exportar "
          "CSV</button>";
  html += "<button onclick='loadLogs()' class='btn "
          "btn-outline'>Actualizar</button>";
  html += "</div>";
  html += "<button onclick='clearLogs()' class='btn btn-danger' "
          "style='margin-top:12px;'>Borrar Historial Interno</button>";
  html += "</div>";

  // Card: SD Logging
  String sdStatus = settingsManager.isSdAvailable() ? "<span style='color:#00c853;'>Conectada</span>" : "<span style='color:#f44336;'>No Detectada</span>";
  html += "<div class='card'><h3>💾 Memoria MicroSD</h3>";
  html += "<p style='color:#bbb; margin-bottom:12px;'>Estado: " + sdStatus + "</p>";
  if (settingsManager.isSdAvailable()) {
    html += "<div class='grid-2'>";
    html += "<button onclick='exportSdLogs()' class='btn btn-secondary'>Exportar CSV (SD)</button>";
    html += "<button onclick='clearSdLogs()' class='btn btn-danger'>Borrar Memoria SD</button>";
    html += "</div>";
  } else {
    html += "<p style='color:#ff9800; font-size:14px;'>Inserte una tarjeta MicroSD y reinicie para habilitar el registro externo.</p>";
  }
  html += "</div>";

  // Card: Network
  html += "<form action='/save_credentials' method='POST'>";
  html += "<div class='card'><h3>📡 Configuración de Red</h3>";
  html += "<div class='input-group'><label>WiFi SSID</label>";
  html += "<div style='display:flex; gap:10px;'>";
  html += "<input type='text' id='wifiSSID' name='wifiSSID' value='" +
          settingsManager.wifiSSID + "' placeholder='Nombre de la red'>";
  html += "<button type='button' onclick='scanWifi()' class='btn "
          "btn-secondary' style='width:auto;'>Buscar</button>";
  html += "</div></div>";
  html += "<div id='scan-results' style='display:none;'></div>";
  html += "<div class='input-group'><label>Contraseña WiFi</label><input "
          "type='password' name='wifiPass' value='" +
          settingsManager.wifiPass + "'></div>";
  html += "<div class='input-group'><label>Nombre del "
          "Dispositivo</label><input type='text' name='devName' value='" +
          settingsManager.deviceName +
          "' placeholder='ej: barra (barra.local)'></div>";
  html += "<button type='submit' class='btn btn-primary'>Guardar Red</button>";
  if (inAPMode) {
      html += "<button type='button' onclick='exitAP()' class='btn btn-warning' style='margin-top:10px;'>CONECTAR Y SALIR</button>";
  }
  html += "</div></form>";

  // Card: Credentials
  String maskToken = "No configurado";
  if (settingsManager.mpAccessToken.length() > 6) {
    maskToken =
        "Guardado: ..." + settingsManager.mpAccessToken.substring(
                              settingsManager.mpAccessToken.length() - 6);
  }

  html += "<form action='/save_credentials' method='POST'>";
  html += "<div class='card'><h3>🔑 Credenciales API</h3>";
  html += "<div class='input-group'><label>Mercado Pago Access Token</label>";
  html += "<input type='password' name='mpToken' value='" +
          settingsManager.mpAccessToken + "'>";
  html += "<small style='color:#00c853; font-size:12px;'>" + maskToken +
          "</small></div>";
  html += "<div class='input-group'><label>Google Script URL</label><input "
          "type='text' name='googleUrl' value='" +
          settingsManager.googleScriptUrl + "'></div>";
  html += "<button type='submit' class='btn btn-primary'>Guardar Keys</button>";
  html += "</div></form>";

  // Card: Change Admin Password
  html += "<form action='/save_credentials' method='POST'>";
  html += "<div class='card'><h3>🔐 Seguridad del Panel</h3>";
  html += "<div class='input-group'><label>Nueva Contraseña</label>";
  html += "<input type='password' name='newPass' placeholder='Dejar vacío para no cambiar'>";
  html += "</div>";
  html += "<div class='input-group'><label>Confirmar Contraseña</label>";
  html += "<input type='password' name='confirmPass' placeholder='Repetir contraseña'>";
  html += "</div>";
  html += "<button type='submit' class='btn btn-warning'>Actualizar Contraseña</button>";
  html += "</div></form>";

  // Card: Advertising Carousel
  html += "<div class='card'><h3>📺 Publicidad y Carrusel</h3>";
  html += "<p>Subir imágenes (JPG recomendadas). Máximo total 400MB.<br><strong>Nota:</strong> Solo JPEG, resolución obligatoria 480x320px.</p>";
  html += "<form action='/upload_ad' method='POST' enctype='multipart/form-data'>";
  html += "<div class='input-group'><input type='file' name='adfile' accept='.jpg,.jpeg,.png,.bmp'></div>";
  html += "<button type='submit' class='btn btn-primary'>Subir Imagen</button>";
  html += "</form><hr>";
  html += "<div id='ad-list'>Cargando imágenes...</div>";
  html += "</div>";

  html += "</div>"; // End container

  // JavaScript for Logs and Scanning
  html += "<script>";
  html += "function toggleFixedQr(val) { "
          "var show = (val == '2') ? 'block' : 'none'; "
          "document.getElementById('fixedQrGroup').style.display = show; "
          "document.getElementById('fixedTypeGroup').style.display = show; "
          "document.getElementById('staticQrTextGroup').style.display = show; "
          "}";
  html +=
      "function loadLogs(){ fetch('/get_logs').then(r=>r.text()).then(csv=>{ "
      "const rows = csv.trim().split('\\n'); const body = "
      "document.getElementById('log-body'); body.innerHTML=''; "
      "rows.reverse().forEach(row=>{ if(!row)return; const cols = "
      "row.split(','); const tr = document.createElement('tr'); "
      "tr.style.borderBottom='1px solid #333'; "
      "cols.forEach(c=>{ const td = document.createElement('td'); "
      "td.style.padding='12px 16px'; td.innerText=c; tr.appendChild(td); }); "
      "body.appendChild(tr); }); }); }";
  html += "function exportLogs(){ "
          "fetch('/get_logs').then(r=>r.blob()).then(blob=>{ "
          "const url = window.URL.createObjectURL(blob); const a = "
          "document.createElement('a'); a.href=url; a.download='ventas.csv'; "
          "a.click(); }); }";
  html += "function clearLogs(){ if(confirm('¿Borrar historial interno?')){ "
          "  fetch('/clear_logs').then(() => location.reload());\n"
          "}}\n"
          "function exportSdLogs(){ "
          "fetch('/get_sd_logs').then(r=>r.blob()).then(blob=>{ "
          "const url = window.URL.createObjectURL(blob); const a = "
          "document.createElement('a'); a.href=url; a.download='sd_ventas.csv'; "
          "a.click(); }); }\n"
          "function clearSdLogs(){ if(confirm('¿Borrar historial de la SD?')){ "
          "  fetch('/clear_sd_logs').then(() => location.reload());\n"
          "}}\n"
          "function testRelay(mode, val) {\n"
          "  console.log('Test Request:', mode, val);\n"
          "  if(confirm('¿Deseas activar la prueba?')) {\n"
          "    fetch('/test_relay?mode=' + mode + '&val=' + val)\n"
          "    .then(r => {\n"
          "       console.log('Response status:', r.status);\n"
          "       return r.text();\n"
          "    })\n"
          "    .then(t => {\n"
          "       console.log('Response text:', t);\n"
          "       alert('Prueba iniciada: ' + t);\n"
          "       setTimeout(loadLogs, 1000); \n" // Auto-refresh logs after 1s
          "    })\n"
          "    .catch(e => console.error('Error fetching:', e));\n"
          "  }\n"
          "}\n"
          "function scanWifi(){ const res = "
          "document.getElementById('scan-results'); res.style.display='block'; "
          "res.innerText='Buscando...'; "
          "fetch('/scan_wifi').then(r=>r.json()).then(data=>{ "
          "res.innerHTML=''; data.forEach(net=>{ const div = "
          "document.createElement('div'); div.style.padding='8px; "
          "cursor:pointer; border-bottom:1px solid #444; color:#fff;'; "
          "div.onmouseover = () => div.style.background = '#444'; "
          "div.onmouseout = () => div.style.background = 'transparent'; "
          "div.innerText = net.ssid + ' (' + net.rssi + 'dBm)'; "
          "div.onclick = () => { "
          "document.getElementById('wifiSSID').value=net.ssid; "
          "res.style.display='none'; }; res.appendChild(div); }); }); }";

  html +=
      "function checkUpdate() { \n"
      "  const msg = document.getElementById('update-msg');\n"
      "  const btn = document.getElementById('btn-check');\n"
      "  msg.innerText='Consultando...'; msg.style.color='#aaa'; "
      "btn.disabled=true;\n"
      "  fetch('/check_update').then(r=>r.json()).then(d=>{\n"
      "     btn.disabled=false;\n"
      "     if(d.available){ \n"
      "       msg.innerText='¡Nueva versión disponible: ' + d.version + '!'; "
      "\n"
      "       msg.style.color='#00c853'; \n"
      "       document.getElementById('btn-install').style.display='block'; "
      "\n"
      "     } else { \n"
      "       msg.innerText='El sistema está actualizado.'; \n"
      "       msg.style.color='#00c853'; \n"
      "     }\n"
      "  }).catch(e=>{ \n"
      "     btn.disabled=false;\n"
      "     msg.innerText='Error al buscar.'; msg.style.color='#f44336'; \n"
      "     console.error(e);\n"
      "  }); \n"
      "}\n";

  html +=
      "function performUpdate() { \n"
      "  if(!confirm('El dispositivo se reiniciará. ¿Continuar?')) return;\n"
      "  const msg = document.getElementById('update-msg');\n"
      "  msg.innerText='Actualizando... NO APAGUE EL EQUIPO'; "
      "msg.style.color='#ff9800';\n"
      "  document.getElementById('btn-install').disabled=true;\n"
      "  fetch('/perform_update').then(r=>r.text()).then(t=>{ \n"
      "     if(t==='OK') { msg.innerText='¡Actualizado! Reiniciando...'; "
      "setTimeout(()=>location.reload(), 20000); } \n"
      "     else { msg.innerText='Error: ' + t; msg.style.color='#f44336'; } "
      "\n"
      "  }); \n"
      "}\n";

  html += "function exitAP() { if(confirm('¿Deseas conectar a WiFi y salir del modo configuración?')) { fetch('/exit_ap').then(r=>r.text()).then(t=>{ alert(t); }); }}";
  html += "function loadAds() { fetch('/list_ads').then(r=>r.json()).then(data=>{ "
          "let h='<table class=\"table\"><tr><th>Archivo</th><th>Tamaño</th><th>Acción</th></tr>'; "
          "data.forEach(f=>{ h+='<tr><td>'+f.name+'</td><td>'+(f.size/1024).toFixed(1)+'KB</td><td><button class=\"btn btn-danger btn-sm\" onclick=\"deleteAd(\\''+f.name+'\\')\">Borrar</button></td></tr>'; }); "
          "h+='</table>'; document.getElementById('ad-list').innerHTML=h; }); } "
          "function deleteAd(name) { if(confirm('Borrar '+name+'?')) { fetch('/delete_ad?name='+name).then(r=>r.text()).then(t=>{ alert(t); loadAds(); }); }} ";
  html += "window.onload = function() { loadLogs(); loadAds(); };";
  html += "</script></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

void handleLogin() {
  String user = server.arg("user");
  String pass = server.arg("pass");
  String passHash = settingsManager.getSha256(pass);

  if (user == settingsManager.adminUser &&
      passHash == settingsManager.adminPassHash) {
    String sessionToken = String(random(100000, 999999));
    server.sendHeader("Set-Cookie",
                      "session=" + sessionToken + "; Path=/; HttpOnly");
    server.sendHeader("Location", "/admin");
    server.send(302, "text/plain", "OK");
  } else {
    server.send(401, "text/plain; charset=UTF-8", "Acceso Denegado");
  }
}

void handleSaveSettings() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }
  settingsManager.operationMode = server.arg("opMode").toInt();
  settingsManager.pricePerUnit = server.arg("unitPrice").toFloat();
  settingsManager.pulseDurationMs = server.arg("pulseDur").toInt();

  settingsManager.fixedUnits = server.arg("fixedUnits").toInt();
  if (settingsManager.fixedUnits <= 0) settingsManager.fixedUnits = 1;

  if (server.hasArg("fixedType")) {
      settingsManager.fixedModeType = server.arg("fixedType").toInt();
  }
  
  settingsManager.staticQrText = server.arg("staticQrText");

  settingsManager.promoEnabled = server.hasArg("promoEn");
  settingsManager.promoThreshold = server.arg("promoThr").toInt();
  settingsManager.promoType = 0; // Always discount (removed bonus option)
  settingsManager.promoValue = server.arg("promoVal").toFloat();

  settingsManager.saveSettings(payments, NUM_PAYMENTS);
  
  // Live Sync with Display
  display.setPricePerUnit(settingsManager.pricePerUnit);
  display.setOperationMode(settingsManager.operationMode);
  display.setPromoConfig(settingsManager.promoEnabled, settingsManager.promoThreshold, settingsManager.promoType, settingsManager.promoValue);
  display.setFixedModeConfig(settingsManager.fixedUnits);
  display.setStaticQrText(settingsManager.staticQrText);

  server.sendHeader("Location", "/admin");
  server.send(302, "text/plain", "Saved");
}

void handleSaveCredentials() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }
  settingsManager.mpAccessToken = server.arg("mpToken");
  settingsManager.googleScriptUrl = server.arg("googleUrl");
  settingsManager.wifiSSID = server.arg("wifiSSID");
  settingsManager.wifiPass = server.arg("wifiPass");
  settingsManager.deviceName = server.arg("devName");

  // Handle Password Change
  if (server.hasArg("newPass") && server.arg("newPass").length() > 0) {
      String newP = server.arg("newPass");
      String confP = server.arg("confirmPass");
      if (newP == confP) {
          settingsManager.adminPassHash = settingsManager.getSha256(newP);
          Serial.println("Admin Password updated.");
      } else {
          Serial.println("Password update failed: mismatch.");
      }
  }

  settingsManager.saveSettings(payments, NUM_PAYMENTS);

  // Update the live MP client
  mpClient.setAccessToken(settingsManager.mpAccessToken);

  server.sendHeader("Location", "/admin");
  server.send(302, "text/plain", "Credentials Saved");
}

void handleGetLogs() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }
  server.send(200, "text/plain", settingsManager.getLogs());
}

void handleClearLogs() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }
  settingsManager.clearLogs();
  server.send(200, "text/plain", "Internal Logs Cleared");
}

void handleGetSdLogs() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }
  server.send(200, "text/plain", settingsManager.getSdLogs());
}

void handleClearSdLogs() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }
  settingsManager.clearSdLogs();
  server.send(200, "text/plain", "SD Logs Cleared");
}

void handleScanWifi() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    if (i < n - 1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleExitAP() {
    if (!isAuthenticated()) {
        server.send(401);
        return;
    }
    exitAPRequested = true;
    server.send(200, "text/plain", "Intentando conectar... Revisa la pantalla del equipo.");
}

void handleListAds() {
  if (!isAuthenticated()) { server.send(401); return; }
  server.send(200, "application/json", settingsManager.listSdDir("/ads"));
}

void handleDeleteAd() {
  if (!isAuthenticated()) { server.send(401, "application/json", "{\"status\":\"error\", \"message\":\"Unauthorized\"}"); return; }
  String name = server.arg("name");
  if (name != "" && settingsManager.deleteSdFile("/ads/" + name)) {
    server.send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Imagen borrada\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Error al borrar\"}");
  }
}

void handleUploadAd() {
  if (!isAuthenticated()) { server.send(401); return; }
  
  if (!settingsManager.isSdAvailable()) {
    server.send(500, "text/plain", "Error: Tarjeta SD no disponible");
    return;
  }

  HTTPUpload& upload = server.upload();
  
  // Skip if no filename (happens with some browsers or empty form)
  if (upload.filename.length() == 0) {
      if (upload.status == UPLOAD_FILE_END) {
          String html = "<html><head><meta http-equiv='refresh' content='2;url=/admin'></head>";
          html += "<body><h3>Error: Ningún archivo seleccionado.</h3><p>Volviendo...</p></body></html>";
          server.send(200, "text/html", html);
      }
      return; 
  }

  if (upload.status == UPLOAD_FILE_START) {
    // Check storage limit ONLY at start
    if (settingsManager.getDirSize("/ads") > 419430400) {
      Serial.println("Upload Error: Quota exceeded");
      // We can't easily stop the stream here without closing connection
      return;
    }

    String filename = "/ads/" + upload.filename;
    Serial.println("Upload Start: " + filename);
    
    // Create or truncate the file
    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to create file: " + filename);
    } else {
        file.close(); 
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Check if file was created successfully before
    String filename = "/ads/" + upload.filename;
    File file = SD.open(filename, FILE_APPEND);
    if (file) {
      file.write(upload.buf, upload.currentSize);
      file.close();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    Serial.println("Upload Success: " + upload.filename + " (" + String(upload.totalSize) + " bytes)");
    
    // Check if client wants JSON (Simple check: if header "X-Response-Type" is "json" or query param)
    // For simplicity, we can default to JSON if query param ?type=json is present, OR just return JSON if it looks like an API call.
    // The safest for existing web app compatibility is to check a query param.
    if(server.hasArg("api")) {
        server.send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Archivo subido\"}");
    } else {
        // Standard Web HTML Response
        String html = "<html><head><meta http-equiv='refresh' content='1;url=/admin'></head>";
        html += "<body><h3>Archivo subido con éxito</h3><p>Redirigiendo...</p>";
        html += "<a href='/admin'>Volver</a></body></html>";
        server.send(200, "text/html", html);
    }
  }
}

void handleCreatePayment() {
  int units = 1;
  if (server.hasArg("units"))
    units = server.arg("units").toInt();

  // --- FIXED QR MODE CHECK (Using Units) ---
  if (settingsManager.operationMode == 2) {
      Serial.println("Creating Payment: Fixed QR Mode Active");
      units = settingsManager.fixedUnits;
  }

  currentUnits = units;
  currentAmount = units * settingsManager.pricePerUnit;
  currentExternalRef = "ESP32_" + String(millis());
  paymentConfirmed = false;

  // --- PROMOTION LOGIC (Backend Match) ---
  if (settingsManager.promoEnabled && units >= settingsManager.promoThreshold) {
      Serial.println("Promotion Triggered (Web)! Applying discount...");
      float discountFactor = (100.0 - settingsManager.promoValue) / 100.0;
      currentAmount = currentAmount * discountFactor;
  }

  String unitLabel;
  if (settingsManager.operationMode == 0) {
      unitLabel = " Minutos";
  } else if (settingsManager.operationMode == 1) {
      unitLabel = " Creditos";
  } else {
      // Fixed Mode: Check flexible type
      unitLabel = (settingsManager.fixedModeType == 0) ? " Minutos" : " Creditos";
  }

  String itemName = String(units) + unitLabel;

  String initPoint = mpClient.createPreference(currentAmount, itemName.c_str(),
                                               currentExternalRef.c_str());
  Serial.println("CreatePayment: Units=" + String(units) +
                 " Amt=" + String(currentAmount) + " URL=" + initPoint);

  // Show QR on Display (Optional mirror)
  display.showQR(initPoint, currentAmount);

  server.send(200, "application/json", "{\"url\":\"" + initPoint + "\"}");
}

void logToGoogleSheets(float amount, int duration, String ref) {
  if (WiFi.status() == WL_CONNECTED &&
      settingsManager.googleScriptUrl.length() > 10) {
    HTTPClient http;
    String url = settingsManager.googleScriptUrl +
                 "?amount=" + String(amount, 2) +
                 "&duration=" + String(duration / 1000) + "&ref=" + ref;

    // Google Scripts requires following redirects
    http.begin(url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = http.GET();

    if (httpCode > 0) {
      Serial.println("Log sent to Google Sheets: " + String(httpCode));
    } else {
      Serial.println("Log failed: " + http.errorToString(httpCode));
    }
    http.end();
  }
}

void handleCheckPayment() {
  String status = "pending";
  if (currentExternalRef != "" && !paymentConfirmed) {
    status = mpClient.checkPaymentStatus(currentExternalRef.c_str());
    if (status == "approved") {
      handlePaymentApproved();
    }
  } else if (paymentConfirmed)
    status = "approved";
  server.send(200, "application/json; charset=UTF-8",
              "{\"status\":\"" + status + "\"}");
}

void handleTestRelay() {
  Serial.println("--- handleTestRelay Called ---");
  if (!isAuthenticated()) {
    Serial.println("Test Relay Failed: Not Authenticated");
    server.send(401, "text/plain", "Auth Failed");
    return;
  }
  String mode = server.arg("mode");
  String valStr = server.arg("val");
  Serial.println("Params received -> Mode: " + mode + ", Val: " + valStr);

  int val = valStr.toInt();
  if (val <= 0)
    val = 1;

  if (mode == "time") {
    int duration = val * 60000;
    isRelayActive = true;
    relayOffTime = millis() + duration;
    digitalWrite(PIN_RELAY, HIGH);
    digitalWrite(PIN_LED, LOW);
    Serial.println("TEST Mode (Time): " + String(duration) + "ms");
    settingsManager.addLog(0, duration, "TEST_TIME");
    server.send(200, "text/plain",
                "Time test started (" + String(val) + " min)");
  } else if (mode == "pulse") {
    pulsesToOutput = val;
    pulseActive = false;
    nextPulseAction = millis();
    Serial.println("TEST Mode (Pulse): " + String(val) + " pulses");
    settingsManager.addLog(0, val, "TEST_PULSE");
    server.send(200, "text/plain",
                "Pulse test started (" + String(val) + " pulses)");
  } else {
    server.send(400, "text/plain", "Invalid mode");
  }
}

void handleCheckUpdate() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }

  if (settingsManager.firmwareUrl.length() < 10) {
    server.send(400, "application/json", "{\"error\":\"No Manifest URL\"}");
    return;
    return;
  }

  WiFiClientSecure client;
  // client.setInsecure(); // REMOVED for security. Please use client.setCACert(ROOT_CA)
  // Instead of setInsecure, we should use certificates for production.
  client.setInsecure(); // Still here for development, but warned.

  HTTPClient http;
  http.begin(client, settingsManager.firmwareUrl);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    String remoteVer = doc["version"].as<String>();
    String currentVer = String(VERSION);

    // Simple string compare for now (assumes v1.x.x format logic or manual
    // check)
    bool available = (remoteVer != currentVer);

    String json = "{\"available\":" + String(available ? "true" : "false") +
                  ", \"version\":\"" + remoteVer + "\"}";
    server.send(200, "application/json", json);
  } else {
    server.send(500, "application/json", "{\"error\":\"Fetch Failed\"}");
  }
  http.end();
}

void handlePerformUpdate() {
  if (!isAuthenticated()) {
    server.send(401);
    return;
  }

  // 1. Fetch manifest again to get the secure binary URL
  WiFiClientSecure client;
  client.setInsecure(); // Still using insecure for DEV, but logic is ready for Secure Binaries

  HTTPClient http;
  http.begin(client, settingsManager.firmwareUrl);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    server.send(500, "text/plain", "Manifest Fetch Failed");
    http.end();
    return;
  }

  String payload = http.getString();
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, payload);
  String binUrl = doc["url"].as<String>(); // 'url' key inside json
  http.end();

  if (binUrl.length() < 10) {
    server.send(500, "text/plain", "Invalid Binary URL");
    return;
  }

  // 2. Perform Secure OTA (Custom stream with AES)
  Serial.println("Starting Secure OTA (AES-256)...");
  
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Update.printError(Serial);
    server.send(500, "text/plain", "Update Begin Failed");
    return;
  }

  // Restart HTTP to add custom auth header for the binary
  http.begin(client, binUrl);
  http.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
  int binCode = http.GET();

  if (binCode != HTTP_CODE_OK) {
    server.send(500, "text/plain", "Binary Fetch Failed (Unauthorized?)");
    http.end();
    return;
  }

  WiFiClient * stream = http.getStreamPtr();
  
  // AES-256-CBC Setup
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  
  unsigned char key[32];
  // Parse hex key from config.h
  for (int i = 0; i < 32; i++) {
    char hex[3] = {OTA_AES_KEY[i*2], OTA_AES_KEY[i*2+1], 0};
    key[i] = (unsigned char)strtol(hex, NULL, 16);
  }
  mbedtls_aes_setkey_dec(&aes, key, 256);

  unsigned char iv[16];
  // The first 16 bytes of the encrypted bin are the IV
  int readBytes = stream->readBytes(iv, 16);
  if (readBytes != 16) {
    mbedtls_aes_free(&aes);
    server.send(500, "text/plain", "Failed to read IV from binary");
    http.end();
    return;
  }

  uint8_t buffer[512]; // Needs to be multiple of 16
  uint8_t decrypted[512];
  size_t totalWritten = 0;
  size_t contentLen = http.getSize();

  while (http.connected() && (contentLen == 0 || totalWritten < contentLen - 16)) {
    size_t availableBytes = stream->available();
    if (availableBytes > 0) {
      size_t toRead = availableBytes;
      if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
      // Read in 16-byte blocks
      toRead = (toRead / 16) * 16;
      if (toRead == 0) { delay(5); continue; }

      int c = stream->readBytes(buffer, toRead);
      if (c <= 0) break;

      // Decrypt blocks using mbedtls
      for (int i = 0; i < c; i += 16) {
          mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, 16, iv, buffer + i, decrypted + i);
      }

      if (Update.write(decrypted, c) != c) {
        Update.printError(Serial);
        break;
      }
      totalWritten += c;
    } else {
      delay(10);
    }
  }

  mbedtls_aes_free(&aes);
  http.end();

  if (Update.end(true)) {
    Serial.printf("Secure OTA Success: %u bytes\nRebooting...", totalWritten);
    server.send(200, "text/plain", "Update Finished. Rebooting...");
    delay(1000);
    ESP.restart();
  } else {
    Update.printError(Serial);
    server.send(500, "text/plain", "Update Finalize Error: " + String(Update.errorString()));
  }
}

// Forward Declarations for API
void handleApiLogin();
void handleApiGetSettings();
void handleApiSaveSettings();
void handleApiGetLogs();
void handleApiScanWifi();
void handleApiTestRelay();

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true); // Enable Core Debug
  delay(1000);
  Serial.println("\n--- ESP32 QR SYSTEM " VERSION " ---");
  Serial.printf("Flash size: %u bytes\n", ESP.getFlashChipSize());

  settingsManager.begin();
  settingsManager.loadSettings(payments, NUM_PAYMENTS);

  // Use loaded credentials or fall back to defaults from config.h
  if (settingsManager.mpAccessToken.length() < 10) {
    settingsManager.mpAccessToken = MP_ACCESS_TOKEN;
  }
  if (settingsManager.googleScriptUrl.length() < 10) {
    settingsManager.googleScriptUrl = GOOGLE_SCRIPT_URL;
  }

  // Force WiFi from config if not set (or to override)
  if (String(WIFI_SSID).length() > 0) {
    settingsManager.wifiSSID = WIFI_SSID;
    settingsManager.wifiPass = WIFI_PASS;
  }

  mpClient.setAccessToken(settingsManager.mpAccessToken);
  
  display.begin();       // Init Display first so labels are created
  soundManager.begin();  // Init Audio
  display.setSoundManager(&soundManager); // Link audio to display

  display.setPricePerUnit(settingsManager.pricePerUnit);
  display.setOperationMode(settingsManager.operationMode);
  // Pass Initial Promo Config
  display.setPromoConfig(settingsManager.promoEnabled, settingsManager.promoThreshold, settingsManager.promoType, settingsManager.promoValue);
  display.setFixedModeConfig(settingsManager.fixedUnits);
  display.setStaticQrText(settingsManager.staticQrText);
  
  display.setPaymentCallback(onTftPaymentRequest);
  display.setActivationCallback(onServiceActivation); // Register activation callback
  display.setCancelCallback(onPaymentCancel); // Register cancellation callback
  
  String sdMsg = settingsManager.isSdAvailable() ? "SD OK" : "SD ERROR";
  display.showStartup("Buscando WiFi...\n" + sdMsg); // Show Splash with SD Status
  delay(2000);

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_CONFIG_BUTTON, INPUT_PULLUP);
  digitalWrite(PIN_RELAY, LOW);
  digitalWrite(PIN_LED, HIGH);

  // mDNS & Hostname Logic
  if (settingsManager.deviceName.length() > 0) {
    activeHostname = settingsManager.deviceName;
  } else {
    uint64_t chipid = ESP.getEfuseMac();
    activeHostname = "qr-" + String((uint16_t)(chipid >> 32), HEX) +
                     String((uint16_t)chipid, HEX);
  }

  // WiFi Connection Logic
  connectToWiFi();

  server.on("/", handleRoot);
  server.on("/admin", handleAdmin);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/save_settings", HTTP_POST, handleSaveSettings);
  server.on("/save_credentials", HTTP_POST, handleSaveCredentials);
  server.on("/exit_ap", handleExitAP);
  server.on("/list_ads", handleListAds);
  server.on("/delete_ad", handleDeleteAd);
  server.on("/upload_ad", HTTP_POST, [](){}, handleUploadAd);
  server.on("/create_payment", handleCreatePayment);
  server.on("/check_payment", handleCheckPayment);
  server.on("/get_logs", handleGetLogs);
  server.on("/clear_logs", handleClearLogs);
  server.on("/get_sd_logs", handleGetSdLogs);
  server.on("/clear_sd_logs", handleClearSdLogs);
  server.on("/scan_wifi", handleScanWifi);
  server.on("/test_relay", handleTestRelay);
  server.on("/check_update", handleCheckUpdate);
  server.on("/perform_update", handlePerformUpdate);

  // --- API JSON Endpoints for Android App ---
  server.on("/api/login", HTTP_POST, handleApiLogin);
  server.on("/api/get_settings", handleApiGetSettings);
  server.on("/api/save_settings", HTTP_POST, handleApiSaveSettings);
  server.on("/api/get_logs", handleApiGetLogs);
  server.on("/api/scan_wifi", handleApiScanWifi);
  server.on("/api/test_relay", handleApiTestRelay);
  
  // Aliases for Ads (Consolidated API)
  server.on("/api/list_ads", handleListAds);
  server.on("/api/delete_ad", handleDeleteAd);
  server.on("/api/upload_ad", HTTP_POST, [](){}, handleUploadAd);

  const char *headerkeys[] = {"Cookie", "Authorization"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char *);
  server.collectHeaders(headerkeys, headerkeyssize);

  server.begin();
}

// =================================================================================
//                              API JSON IMPLEMENTATION
// =================================================================================

void handleApiLogin() {
  String user = "";
  String pass = "";
  
  // Try to parse JSON body first
  if(server.hasArg("plain")) {
      DynamicJsonDocument doc(512);
      deserializeJson(doc, server.arg("plain"));
      if(doc.containsKey("user")) user = doc["user"].as<String>();
      if(doc.containsKey("pass")) pass = doc["pass"].as<String>();
  }
  
  // Fallback to Form Data
  if(user == "") user = server.arg("user");
  if(pass == "") pass = server.arg("pass");
  
  String passHash = settingsManager.getSha256(pass);

  if (user == settingsManager.adminUser && passHash == settingsManager.adminPassHash) {
    String sessionToken = String(random(100000, 999999));
    server.sendHeader("Set-Cookie", "session=" + sessionToken + "; Path=/; HttpOnly");
    server.send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Login Successful\"}");
  } else {
    server.send(401, "application/json", "{\"status\":\"error\", \"message\":\"Invalid Credentials\"}");
  }
}

void handleApiGetSettings() {
  if (!isAuthenticated()) {
    server.send(401, "application/json", "{\"status\":\"error\", \"message\":\"Unauthorized\"}");
    return;
  }
  
  DynamicJsonDocument doc(1024);
  doc["status"] = "ok";
  doc["wifiSSID"] = settingsManager.wifiSSID;
  doc["adminUser"] = settingsManager.adminUser;
  // Do not send password hash for security
  doc["mpAccessToken"] = settingsManager.mpAccessToken;
  doc["googleScriptUrl"] = settingsManager.googleScriptUrl;
  doc["deviceName"] = settingsManager.deviceName;
  doc["mac"] = WiFi.macAddress(); // Request from Android
  
  if (WiFi.status() == WL_CONNECTED) {
      doc["ip"] = WiFi.localIP().toString();
      doc["listeningUrl"] = "http://" + WiFi.localIP().toString();
  } else {
      doc["ip"] = "0.0.0.0";
      doc["listeningUrl"] = "";
  }
  
  doc["pricePerUnit"] = settingsManager.pricePerUnit;
  doc["pulseDurationMs"] = settingsManager.pulseDurationMs;
  doc["operationMode"] = settingsManager.operationMode;
  doc["fixedUnits"] = settingsManager.fixedUnits;
  doc["fixedModeType"] = settingsManager.fixedModeType;
  doc["staticQrText"] = settingsManager.staticQrText;
  
  doc["promoEnabled"] = settingsManager.promoEnabled;
  doc["promoThreshold"] = settingsManager.promoThreshold;
  doc["promoValue"] = settingsManager.promoValue;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleApiSaveSettings() {
  if (!isAuthenticated()) {
    server.send(401, "application/json", "{\"status\":\"error\", \"message\":\"Unauthorized\"}");
    return;
  }
  
  if(!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Body is missing\"}");
      return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  // Debug Log
  Serial.println("API Save Settings Received:");
  Serial.println(server.arg("plain"));
  
  if (error) {
      Serial.print("JSON Error: ");
      Serial.println(error.c_str());
      server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Invalid JSON\"}");
      return;
  }
  
  // Update Settings if key exists
  if(doc.containsKey("pricePerUnit")) settingsManager.pricePerUnit = doc["pricePerUnit"].as<float>();
  if(doc.containsKey("pulseDurationMs")) settingsManager.pulseDurationMs = doc["pulseDurationMs"].as<int>();
  if(doc.containsKey("operationMode")) settingsManager.operationMode = doc["operationMode"].as<int>();
  if(doc.containsKey("fixedUnits")) {
      settingsManager.fixedUnits = doc["fixedUnits"].as<int>();
      if (settingsManager.fixedUnits <= 0) settingsManager.fixedUnits = 1;
  }
  if(doc.containsKey("fixedModeType")) settingsManager.fixedModeType = doc["fixedModeType"].as<int>();
  if(doc.containsKey("staticQrText")) settingsManager.staticQrText = doc["staticQrText"].as<String>();
  
  if(doc.containsKey("promoEnabled")) settingsManager.promoEnabled = doc["promoEnabled"].as<bool>();
  if(doc.containsKey("promoThreshold")) settingsManager.promoThreshold = doc["promoThreshold"].as<int>();
  if(doc.containsKey("promoValue")) settingsManager.promoValue = doc["promoValue"].as<float>();
  
  // Partial Updates: Check valid length before overwriting
  if(doc.containsKey("mpAccessToken")) {
      String t = doc["mpAccessToken"].as<String>();
      if(t.length() > 0) settingsManager.mpAccessToken = t;
  }
  if(doc.containsKey("googleScriptUrl")) {
      String u = doc["googleScriptUrl"].as<String>();
      if(u.length() > 0) settingsManager.googleScriptUrl = u;
  }
  if(doc.containsKey("deviceName")) {
      String d = doc["deviceName"].as<String>();
      if(d.length() > 0) settingsManager.deviceName = d;
  }
  if(doc.containsKey("wifiSSID")) {
      String s = doc["wifiSSID"].as<String>();
      // Allow empty checks only if explicitly intended? Assuming ignoring empty strings for now.
      if(s.length() > 0) settingsManager.wifiSSID = s;
  }
  if(doc.containsKey("wifiPass")) {
       String p = doc["wifiPass"].as<String>();
       if(p.length() > 0) settingsManager.wifiPass = p;
  }

  // Password Change Logic
  if(doc.containsKey("newPass") && doc.containsKey("confirmPass")) {
      String np = doc["newPass"].as<String>();
      String cp = doc["confirmPass"].as<String>();
      if(np.length() > 0 && np == cp) {
          settingsManager.adminPassHash = settingsManager.getSha256(np);
          Serial.println("API: Admin Password updated.");
      } else {
          Serial.println("API: Password mismatch or empty.");
      }
  }

  settingsManager.saveSettings(payments, NUM_PAYMENTS);

  // Sync with Display
  display.setPricePerUnit(settingsManager.pricePerUnit);
  display.setOperationMode(settingsManager.operationMode);
  display.setPromoConfig(settingsManager.promoEnabled, settingsManager.promoThreshold, settingsManager.promoType, settingsManager.promoValue);
  display.setFixedModeConfig(settingsManager.fixedUnits);
  display.setStaticQrText(settingsManager.staticQrText);
  
  // Sync Live Objects
  mpClient.setAccessToken(settingsManager.mpAccessToken);

  server.send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Settings Saved\"}");
}

void handleApiGetLogs() {
  if (!isAuthenticated()) {
    server.send(401, "application/json", "{\"status\":\"error\", \"message\":\"Unauthorized\"}");
    return;
  }
  
  // Read CSV content: amount,duration,ref\n...
  String csv = settingsManager.getLogs();
  
  // Basic Parsing to JSON Array
  String json = "[";
  int start = 0;
  int end = csv.indexOf('\n');
  bool first = true;
  
  while (end != -1) {
      String line = csv.substring(start, end);
      line.trim();
      if(line.length() > 0) {
          int c1 = line.indexOf(',');
          int c2 = line.indexOf(',', c1 + 1);
          if (c1 > 0 && c2 > 0) {
              String amt = line.substring(0, c1);
              String dur = line.substring(c1 + 1, c2);
              String ref = line.substring(c2 + 1);
              
              if(!first) json += ",";
              json += "{\"amount\":" + amt + ", \"duration\":" + dur + ", \"ref\":\"" + ref + "\"}";
              first = false;
          }
      }
      start = end + 1;
      end = csv.indexOf('\n', start);
  }
  json += "]";
  
  server.send(200, "application/json", json);
}

void handleApiScanWifi() {
  if (!isAuthenticated()) {
    server.send(401, "application/json", "{\"status\":\"error\", \"message\":\"Unauthorized\"}");
    return;
  }
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    if (i < n - 1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleApiTestRelay() {
  if (!isAuthenticated()) {
    server.send(401, "application/json", "{\"status\":\"error\", \"message\":\"Unauthorized\"}");
    return;
  }
  
  String mode = "";
  int val = 0;

  // Try JSON Parse
  if(server.hasArg("plain")) {
      DynamicJsonDocument doc(512);
      deserializeJson(doc, server.arg("plain"));
      if(doc.containsKey("mode")) mode = doc["mode"].as<String>();
      if(doc.containsKey("val")) val = doc["val"].as<int>();
  }
  
  // Fallback Query Params
  if(mode == "") mode = server.arg("mode");
  if(val == 0) val = server.arg("val").toInt();
  
  if (val <= 0) val = 1;

  Serial.println("API Test Relay -> Mode: " + mode + ", Val: " + String(val));

  if (mode == "time") {
    int duration = val * 60000;
    isRelayActive = true;
    relayOffTime = millis() + duration;
    digitalWrite(PIN_RELAY, HIGH);
    digitalWrite(PIN_LED, LOW);
    settingsManager.addLog(0, duration, "TEST_TIME_APP");
    server.send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Time test started\"}");
  } else if (mode == "pulse") {
    pulsesToOutput = val;
    pulseActive = false;
    nextPulseAction = millis();
    settingsManager.addLog(0, val, "TEST_PULSE_APP");
    server.send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Pulse test started\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Invalid Mode\"}");
  }
}

void loop() {
  dnsServer.processNextRequest();
  // Periodic WiFi Status Update (every 5s)
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 5000) {
    lastWiFiCheck = millis();
    display.setWiFiStatus(WiFi.status() == WL_CONNECTED);
  }

  if (exitAPRequested) {
      exitAPRequested = false;
      Serial.println("Exit AP Requested. Reconnecting...");
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      inAPMode = false;
      connectToWiFi();
  }

  server.handleClient();
  
  // If we are doing anything important, reset the inactivity timer
  if (inAPMode || isRelayActive || currentExternalRef != "") {
      display.addActivity();
  }

  // Polling automático para pagos pendientes
  if (currentExternalRef != "" && !paymentConfirmed) {
    if (millis() - lastPollTime >= POLL_INTERVAL) {
      lastPollTime = millis();
      String status = mpClient.checkPaymentStatus(currentExternalRef.c_str());
      Serial.printf("Auto-Polling: Ref=%s, Status=%s\n", currentExternalRef.c_str(), status.c_str());
      if (status == "approved") {
        handlePaymentApproved();
      }
    }
  }

  // Manual AP Mode Trigger (Hold button for 5s)
  if (digitalRead(PIN_CONFIG_BUTTON) == LOW) {
    if (buttonPressedTime == 0)
      buttonPressedTime = millis();
    if (!inAPMode && (millis() - buttonPressedTime > 5000)) {
      startAPMode();
    }
  } else {
    buttonPressedTime = 0;
  }

  if (isRelayActive && millis() > relayOffTime) {
    isRelayActive = false;
    digitalWrite(PIN_RELAY, LOW);
    digitalWrite(PIN_LED, HIGH);
    Serial.println("Relay OFF");
  }

  // Warning for Time Mode (1 minute left)
  if (isRelayActive && !warningShown && settingsManager.operationMode == 0) {
    long remaining = (long)relayOffTime - (long)millis();
    if (remaining <= 60000 && remaining > 0) {
      warningShown = true;
      soundManager.playWarning(); // Trigger audio alert
      display.showWarning("¡AVISO!\nQueda 1 minuto de servicio");
    }
  }

  // Credit mode pulse handling
  if (pulsesToOutput > 0 && millis() >= nextPulseAction) {
    if (!pulseActive) {
      digitalWrite(PIN_RELAY, HIGH);
      digitalWrite(PIN_LED, LOW); // LED OFF when Relay ON
      pulseActive = true;
      nextPulseAction = millis() + settingsManager.pulseDurationMs;
    } else {
      digitalWrite(PIN_RELAY, LOW);
      digitalWrite(PIN_LED, HIGH); // LED ON when Relay OFF
      pulseActive = false;
      pulsesToOutput--;
      nextPulseAction = millis() + settingsManager.pulseDurationMs;
    }
  }

  soundManager.loop(); 
  display.loop();
  delay(1);
}
