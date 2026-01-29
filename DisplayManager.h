#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "AXS15231B_touch.h"
#include <TJpg_Decoder.h>

// Colores base
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_BLUE  0x001F
#define C_GREEN 0x07E0

class DisplayManager {
public:
  DisplayManager();
  void begin();
  void loop();
  
  typedef void (*PaymentRequestCallback)(int units);
  void setPaymentCallback(PaymentRequestCallback cb) { _paymentCallback = cb; }

  typedef void (*ActivationCallback)();
  void setActivationCallback(ActivationCallback cb) { _activationCallback = cb; }
  void setCancelCallback(ActivationCallback cb) { _cancelCallback = cb; }

  void showStartup(String msg);
  void showAPInfo(String ssid, String pass, String url);
  void showConnectionSuccess(String ip, String url);
  void showKeypad();
  void showQR(String url, float amount);
  void showReady();
  void showSuccess();
  void setError(String msg);
  void showWarning(String msg);
  void showAds();
  void stopAds();
  void addActivity();
  
  void setPricePerUnit(float price);
  void setOperationMode(int mode);
  void setPromoConfig(bool enabled, int threshold, int type, float value);
  void setFixedModeConfig(int units);
  void setStaticQrText(String text);
  void setWiFiStatus(bool connected);
  void setSoundManager(void * mgr) { _soundManager = mgr; }

private:
  Arduino_DataBus *bus;
  Arduino_GFX *gfx;
  Arduino_Canvas *canvas;
  AXS15231B_Touch *touch;

  // OBJETOS LVGL
  lv_obj_t * scr_startup;
  lv_obj_t * scr_keypad;
  lv_obj_t * scr_qr;
  lv_obj_t * scr_ready;
  lv_obj_t * scr_success;
  lv_obj_t * scr_ads;
  
  lv_obj_t * img_ad;
  
  lv_obj_t * lbl_status; // For startup/AP info
  
  // UI Elements
  lv_obj_t * cont_amount;
  lv_obj_t * btnm;
  lv_obj_t * btn_gen;

  lv_obj_t * lbl_amount;
  lv_obj_t * lbl_total;
  lv_obj_t * lbl_price_display; // Total $ label on keypad
  lv_obj_t * lbl_unit_price;    // "Valor Minuto/Credito" label
  lv_obj_t * lbl_promo_info;    // "10% OFF" label
  lv_obj_t * lbl_wifi_status;   // "Offline" warning
  lv_obj_t * qr_canvas;
  lv_color_t * qr_buffer = nullptr;
  
  String currentAmountStr = "0";
  float unitPrice = 0; // This seems redundant with _pricePerUnit, I'll use _pricePerUnit
  float _pricePerUnit = 0;
  int _opMode = 0; // 0: Time, 1: Credit, 2: Fixed QR
  
  // Promo Settings
  bool _promoEnabled = false;
  int _promoThreshold = 0;
  int _promoType = 0;
  float _promoValue = 0;
  
  // Fixed Mode Settings
  int _fixedUnits = 1;
  String _staticQrText = "";
  lv_obj_t * lbl_static_qr_text = nullptr;
  
  PaymentRequestCallback _paymentCallback = nullptr;
  ActivationCallback _activationCallback = nullptr;
  ActivationCallback _cancelCallback = nullptr;
  
  unsigned long _infoStartTime = 0;
  bool _showingInfo = false;
  
  unsigned long _lastActivity = 0;
  bool _isShowingAds = false;
  
  // Sound Hook
  void * _soundManager = nullptr;

  int _currentAdIndex = 0;
  lv_timer_t * _adTimer = nullptr;
  
  lv_image_dsc_t _ad_img_dsc;
  uint16_t * _ad_buffer = nullptr;

  void initLVGL();
  void createStartupUI();
  void createKeypadUI();
  void createQRUI();
  void createReadyUI();
  void createSuccessUI();
  void createAdsUI();
  
  static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);
  static void event_handler_num(lv_event_t * e);
  static void event_handler_gen(lv_event_t * e);
};

#endif
