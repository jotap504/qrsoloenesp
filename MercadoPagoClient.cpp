#include "MercadoPagoClient.h"
#include <WiFiClientSecure.h>

MercadoPagoClient::MercadoPagoClient(const char *accessToken) {
  _accessToken = accessToken;
}

void MercadoPagoClient::setAccessToken(String token) { _accessToken = token; }

String MercadoPagoClient::createPreference(float amount, const char *title,
                                           const char *externalReference) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.mercadopago.com/checkout/preferences");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(_accessToken));

  StaticJsonDocument<512> doc;
  doc["external_reference"] = externalReference;
  JsonArray items = doc.createNestedArray("items");
  JsonObject item = items.createNestedObject();
  item["title"] = title;
  item["quantity"] = 1;
  item["unit_price"] = amount;
  item["currency_id"] = "ARS";

  // Optional: Auto-return to the ESP32 IP or a success page if needed
  // doc["back_urls"]["success"] = "http://esp32-ip/success";

  String requestBody;
  serializeJson(doc, requestBody);

  int httpResponseCode = http.POST(requestBody);
  String response = "Error";

  if (httpResponseCode >= 200 && httpResponseCode < 300) {
    String payload = http.getString();
    DynamicJsonDocument resDoc(4096);
    DeserializationError error = deserializeJson(resDoc, payload);

    if (!error) {
      if (resDoc.containsKey("init_point")) {
        response = resDoc["init_point"].as<String>();
        Serial.println("Preference created: " + response);
      } else {
        Serial.println("Error: 'init_point' not found in response");
        Serial.println("Payload: " + payload);
      }
    } else {
      Serial.print("JSON Deserialization Error: ");
      Serial.println(error.c_str());
      Serial.println("Payload was: " + payload);
    }
  } else {
    Serial.print("MP HTTP Error: ");
    Serial.println(httpResponseCode);
    if (httpResponseCode > 0) {
      Serial.println("Response: " + http.getString());
    }
  }

  http.end();
  return response;
}

String MercadoPagoClient::checkPaymentStatus(const char *externalReference) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url =
      "https://api.mercadopago.com/v1/payments/search?external_reference=" +
      String(externalReference);
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + String(_accessToken));

  int httpResponseCode = http.GET();
  String status = "pending";

  if (httpResponseCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument resDoc(4096);
    DeserializationError error = deserializeJson(resDoc, payload);

    if (!error) {
      JsonArray results = resDoc["results"];
      if (results.size() > 0) {
        status = results[0]["status"].as<String>();
        Serial.println("MP Search: Found payment with status: " + status);
      } else {
        Serial.println("MP Search: No payments found for reference: " + String(externalReference));
      }
    } else {
      Serial.print("JSON Search Deserialization Error: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.print("Error checking status: ");
    Serial.println(httpResponseCode);
  }

  http.end();
  return status;
}
