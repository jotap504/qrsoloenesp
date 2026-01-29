#ifndef MERCADOPAGOCLIENT_H
#define MERCADOPAGOCLIENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

class MercadoPagoClient {
public:
  MercadoPagoClient(const char *accessToken);
  void setAccessToken(String token);
  String createPreference(float amount, const char *title,
                          const char *externalReference);
  String checkPaymentStatus(const char *externalReference);

private:
  String _accessToken;
};

#endif
