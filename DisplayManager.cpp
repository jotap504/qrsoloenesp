#include "DisplayManager.h"
#include "SoundManager.h"

#include "qrcode.h"
#include <FS.h>
#include <SD.h>

// Global pointer for static event handlers
DisplayManager* instance = nullptr;

// Subclass to force PSRAM allocation for the large canvas buffer
class Arduino_Canvas_PSRAM : public Arduino_Canvas {
public:
    Arduino_Canvas_PSRAM(int16_t w, int16_t h, Arduino_G *output, int16_t out_x = 0, int16_t out_y = 0, uint8_t r = 0)
        : Arduino_Canvas(w, h, output, out_x, out_y, r) {}
    
    bool begin(int32_t speed = GFX_NOT_DEFINED) override {
        if (!_framebuffer) {
            size_t s = (size_t)_width * _height * 2;
            _framebuffer = (uint16_t *)ps_malloc(s);
            if (_framebuffer) Serial.println("Canvas allocated in PSRAM");
            else Serial.println("Canvas PSRAM allocation failed, falling back to SRAM");
        }
        return Arduino_Canvas::begin(speed);
    }
};

DisplayManager::DisplayManager() {
    instance = this;
    bus = new Arduino_ESP32QSPI(45, 47, 21, 48, 40, 39);
    gfx = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, 320, 480);
    canvas = new Arduino_Canvas_PSRAM(320, 480, gfx, 0, 0, 0);
    touch = new AXS15231B_Touch(8, 4, 3, 0x3B, 1); // SDA=4, SCL=8, INT=3
}

void DisplayManager::begin() {
    pinMode(1, OUTPUT);
    digitalWrite(1, HIGH); // Backlight

    if (!canvas->begin()) {
        Serial.println("Display Init Failed!");
        return;
    }
    canvas->setRotation(1); // 480x320
    canvas->fillScreen(C_BLACK);
    canvas->flush();

    if (!touch->begin()) {
        Serial.println("Touch Init Failed!");
    } else {
        touch->setOffsets(0, 320, 320, 0, 480, 480);
        touch->setRotation(1);
    }

    initLVGL();
    createStartupUI();
    createKeypadUI();
    createQRUI();
    createReadyUI();
    createSuccessUI();
    createAdsUI();
    
    // Allocate Ad Buffer in PSRAM (480x320x2 bytes)
    _ad_buffer = (uint16_t *)ps_malloc(480 * 320 * sizeof(uint16_t));
    if (!_ad_buffer) _ad_buffer = (uint16_t *)malloc(480 * 320 * sizeof(uint16_t));
    
    // Setup LVGL Image Descriptor for the Ad
    _ad_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    _ad_img_dsc.header.w = 480;
    _ad_img_dsc.header.h = 320;
    _ad_img_dsc.header.stride = 480 * 2;
    _ad_img_dsc.data_size = 480 * 320 * 2;
    _ad_img_dsc.data = (const uint8_t *)_ad_buffer;

    // Initialize TJpgDec
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tft_output);

    // Initial load: keep it on scr_startup until WiFi flow decides
    lv_screen_load(scr_startup);
    _lastActivity = millis();
}

void DisplayManager::initLVGL() {
    lv_init();
    
#if LV_USE_LOG
    lv_log_register_print_cb([](lv_log_level_t level, const char * buf) {
        Serial.printf("LVGL: %s\n", buf);
    });
#endif
    
    uint32_t screenWidth = canvas->width();
    uint32_t screenHeight = canvas->height();
    uint32_t bufSize = screenWidth * screenHeight / 10;
    
    lv_color_t *buf = (lv_color_t *)ps_malloc(bufSize * sizeof(lv_color_t));
    if (!buf) buf = (lv_color_t *)malloc(bufSize * sizeof(lv_color_t)); // Fallback
    
    lv_display_t *disp = lv_display_create(screenWidth, screenHeight);
    lv_display_set_user_data(disp, this);
    lv_display_set_flush_cb(disp, [](lv_display_t *d, const lv_area_t *a, uint8_t *px) {
        uint32_t w = lv_area_get_width(a);
        uint32_t h = lv_area_get_height(a);
        instance->canvas->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, w, h);
        if (lv_display_flush_is_last(d)) instance->canvas->flush();
        lv_disp_flush_ready(d);
    });
    lv_display_set_buffers(disp, buf, NULL, bufSize * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, [](lv_indev_t *i, lv_indev_data_t *data) {
        if (instance->touch->touched()) {
            uint16_t x, y;
            instance->touch->readData(&x, &y);
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
            Serial.printf("Touch detected at X:%d, Y:%d\n", x, y);
            instance->addActivity(); // Reset inactivity on any touch
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });
}

void DisplayManager::createStartupUI() {
    scr_startup = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_startup, lv_color_black(), 0);

    // Branding
    lv_obj_t * b_title = lv_label_create(scr_startup);
    lv_label_set_text(b_title, "Pag.ar"); // Updated branding
    lv_obj_set_style_text_color(b_title, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(b_title, &lv_font_montserrat_32, 0);
    lv_obj_align(b_title, LV_ALIGN_TOP_MID, 0, 40);

    lbl_status = lv_label_create(scr_startup);
    lv_label_set_text(lbl_status, "Iniciando...");
    lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_status, 300);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 20);
}

void DisplayManager::createKeypadUI() {
    scr_keypad = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_keypad, lv_color_black(), 0);

    // Branding Superior
    lv_obj_t * b_title = lv_label_create(scr_keypad);
    lv_label_set_text(b_title, "Pag.ar"); // Updated branding
    lv_obj_set_style_text_color(b_title, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(b_title, &lv_font_montserrat_32, 0);
    lv_obj_align(b_title, LV_ALIGN_TOP_LEFT, 20, 10);

    // Titulo secundario
    lv_obj_t * title = lv_label_create(scr_keypad);
    lv_label_set_text(title, "Mercado Pago");
    lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align_to(title, b_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);

    // WiFi Status indicator
    lbl_wifi_status = lv_obj_create(scr_keypad);
    lv_obj_set_size(lbl_wifi_status, 12, 12);
    lv_obj_set_style_radius(lbl_wifi_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lbl_wifi_status, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_bg_opa(lbl_wifi_status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lbl_wifi_status, 0, 0);
    lv_obj_align(lbl_wifi_status, LV_ALIGN_TOP_RIGHT, -15, 15);

    // Display Area (Smaller)
    cont_amount = lv_obj_create(scr_keypad);
    lv_obj_set_size(cont_amount, 180, 50);
    lv_obj_align(cont_amount, LV_ALIGN_TOP_LEFT, 20, 65);
    lv_obj_set_style_bg_color(cont_amount, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(cont_amount, 1, 0);
    lv_obj_set_style_border_color(cont_amount, lv_color_hex(0x333333), 0);

    lbl_amount = lv_label_create(cont_amount);
    lv_label_set_text(lbl_amount, "0");
    lv_obj_set_style_text_font(lbl_amount, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_amount, lv_color_white(), 0);
    lv_obj_center(lbl_amount);

    // Right Side Pricing
    lbl_unit_price = lv_label_create(scr_keypad);
    lv_obj_set_style_text_font(lbl_unit_price, &lv_font_montserrat_20, 0); // INCREASED FONT
    lv_obj_set_style_text_color(lbl_unit_price, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(lbl_unit_price, LV_ALIGN_TOP_RIGHT, -20, 50);
    setOperationMode(_opMode); // Initialize with actual values

    lbl_price_display = lv_label_create(scr_keypad);
    lv_label_set_text(lbl_price_display, "Total: $0.00");
    lv_obj_set_style_text_font(lbl_price_display, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_price_display, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(lbl_price_display, LV_ALIGN_TOP_RIGHT, -20, 75); // Restored alignment
    lbl_promo_info = lv_label_create(scr_keypad);
    lv_label_set_text(lbl_promo_info, "");
    lv_obj_set_style_text_font(lbl_promo_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_promo_info, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(lbl_promo_info, LV_ALIGN_TOP_RIGHT, -20, 110);

    // Teclado (Hide if Fixed Mode)
    btnm = lv_buttonmatrix_create(scr_keypad);
    static const char * btnm_map[] = {"1", "2", "3", "\n",
                                      "4", "5", "6", "\n",
                                      "7", "8", "9", "\n",
                                      "C", "0", "Del", ""};
    lv_buttonmatrix_set_map(btnm, btnm_map);
    lv_obj_set_size(btnm, 320, 190); 
    lv_obj_align(btnm, LV_ALIGN_BOTTOM_LEFT, 10, -5);
    lv_obj_set_style_bg_opa(btnm, 0, 0);
    lv_obj_set_style_border_width(btnm, 0, 0);
    lv_obj_set_style_pad_all(btnm, 5, 0);
    lv_obj_add_event_cb(btnm, event_handler_num, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Boton Generar (Default Position)
    btn_gen = lv_button_create(scr_keypad);
    lv_obj_set_size(btn_gen, 120, 150);
    lv_obj_align(btn_gen, LV_ALIGN_BOTTOM_RIGHT, -10, -25);
    lv_obj_set_style_bg_color(btn_gen, lv_palette_main(LV_PALETTE_BLUE), 0);
    
    lv_obj_t * lbl_gen = lv_label_create(btn_gen);
    lv_label_set_text(lbl_gen, "GENERAR\nQR");
    lv_obj_set_style_text_font(lbl_gen, &lv_font_montserrat_20, 0); // INCREASED FONT
    lv_obj_center(lbl_gen);
    lv_obj_add_event_cb(btn_gen, event_handler_gen, LV_EVENT_CLICKED, NULL);

    lv_obj_center(lbl_gen);
    lv_obj_add_event_cb(btn_gen, event_handler_gen, LV_EVENT_CLICKED, NULL);

    // Static QR Text Label (for Fixed QR mode - shows above the button)
    lbl_static_qr_text = lv_label_create(scr_keypad);
    lv_label_set_text(lbl_static_qr_text, ""); // Will be set dynamically
    lv_obj_set_style_text_font(lbl_static_qr_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_static_qr_text, lv_color_hex(0xFFD700), 0); // Gold color
    lv_obj_set_style_text_align(lbl_static_qr_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_static_qr_text, 300);
    lv_obj_align(lbl_static_qr_text, LV_ALIGN_CENTER, 0, -60); // Above center
    lv_obj_add_flag(lbl_static_qr_text, LV_OBJ_FLAG_HIDDEN); // Hidden by default

    // Initial State Check
    setOperationMode(_opMode);
}

void DisplayManager::createAdsUI() {
    scr_ads = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_ads, lv_color_black(), 0);

    img_ad = lv_image_create(scr_ads);
    lv_obj_set_size(img_ad, 480, 320);
    lv_obj_center(img_ad);
    // Visual test: if image fails, we should see RED
    lv_obj_set_style_bg_color(img_ad, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(img_ad, LV_OPA_COVER, 0);

    // Persistent "Start" button
    lv_obj_t * btn_start = lv_button_create(scr_ads);
    lv_obj_set_size(btn_start, 100, 40);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(btn_start, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t * lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, "INICIAR");
    lv_obj_center(lbl_start);

    auto exit_cb = [](lv_event_t * e) {
        instance->stopAds();
        instance->showKeypad();
    };

    lv_obj_add_event_cb(btn_start, exit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_ads, exit_cb, LV_EVENT_CLICKED, NULL); // Any touch exits
}

void DisplayManager::event_handler_num(lv_event_t * e) {
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(obj);
    const char * txt = lv_buttonmatrix_get_button_text(obj, id);
    
    // Play Click Sound
    if(instance->_soundManager) {
        ((SoundManager*)instance->_soundManager)->playClick();
    }
    
    if (strcmp(txt, "C") == 0) {
        instance->currentAmountStr = "0";
    } else if (strcmp(txt, "Del") == 0) {
        if (instance->currentAmountStr.length() > 1) {
            instance->currentAmountStr.remove(instance->currentAmountStr.length() - 1);
        } else {
            instance->currentAmountStr = "0";
        }
    } else {
        if (instance->currentAmountStr == "0") instance->currentAmountStr = "";
        if (instance->currentAmountStr.length() < 6) {
            instance->currentAmountStr += txt;
        }
    }
    lv_label_set_text(instance->lbl_amount, instance->currentAmountStr.c_str());
    
    // Update total price display with Promo Logic
    int amount = instance->currentAmountStr.toInt();
    float total = amount * instance->_pricePerUnit;
    
    // Reset Promo Label
    if(instance->lbl_promo_info) lv_label_set_text(instance->lbl_promo_info, "");
    
    // Check Promotion
    if (instance->_promoEnabled && amount >= instance->_promoThreshold) {
        float discountFactor = (100.0 - instance->_promoValue) / 100.0;
        total = total * discountFactor;
        
        char pBuf[32];
        sprintf(pBuf, "Desc. %.0f%% Aplicado!", instance->_promoValue);
        if(instance->lbl_promo_info) lv_label_set_text(instance->lbl_promo_info, pBuf);
    }

    char buf[32];
    sprintf(buf, "Total: $%.2f", total);
    lv_label_set_text(instance->lbl_price_display, buf);
    
    instance->addActivity();
}

void DisplayManager::event_handler_gen(lv_event_t * e) {
    if(instance->_soundManager) ((SoundManager*)instance->_soundManager)->playQrGenerated();
    
    // Validate Amount
    if (instance->currentAmountStr == "" || instance->currentAmountStr == "0") return;
    
    float amount = instance->currentAmountStr.toFloat();
    // Validate Minimum
    if (amount < instance->_pricePerUnit) {
         instance->showWarning("Mínimo: $" + String(instance->_pricePerUnit, 0));
         return;
    }
    
    // NOTE: Do NOT apply discount here. Pass raw units to main loop.
    // The main loop calculates the price discount.
    // Making sure we verify against pricePerUnit correctly above logic only checks raw value.

    if (instance->_paymentCallback) {
        // Convert Amount to Units based on PricePerUnit
        int units = (int)(amount / instance->_pricePerUnit);
        if (units < 1) units = 1;

        // Trigger Callback (Main Loop will handle MP creation)
        instance->_paymentCallback(units); 
    }
}

void DisplayManager::showStartup(String msg) {
    if (!lbl_status) return;
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_status, msg.c_str());
    lv_screen_load(scr_startup);
}

void DisplayManager::showAPInfo(String ssid, String pass, String url) {
    if (!lbl_status) return;
    
    char buf[256];
    sprintf(buf, "MODO CONFIGURACION\n\nConectar a Wifi:\n%s\n\nPanel Admin:\n%s", 
            ssid.c_str(), url.c_str());
    
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_status, buf);
    lv_screen_load(scr_startup);
}

void DisplayManager::showConnectionSuccess(String ip, String url) {
    if (!lbl_status) return;
    
    char buf[256];
    sprintf(buf, "CONECTADO EXITOSAMENTE\n\nIP: %s\n\nURL: %s\n\nIniciando teclado...", 
            ip.c_str(), url.c_str());
    
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_status, buf);
    lv_screen_load(scr_startup);
    
    _showingInfo = true;
    _infoStartTime = millis();
}

void DisplayManager::showKeypad() {
    _showingInfo = false;
    lv_screen_load(scr_keypad);
}

void DisplayManager::createQRUI() {
    scr_qr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_qr, lv_color_white(), 0);
    
    // Branding
    lv_obj_t * b_title = lv_label_create(scr_qr);
    lv_label_set_text(b_title, "Pag.ar"); // Updated branding
    lv_obj_set_style_text_color(b_title, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(b_title, &lv_font_montserrat_24, 0);
    lv_obj_align(b_title, LV_ALIGN_TOP_RIGHT, -10, 10);

    lv_obj_t * btn_back = lv_button_create(scr_qr);
    lv_obj_set_size(btn_back, 100, 40);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_label_set_text(lv_label_create(btn_back), "CANCELAR");
    lv_obj_add_event_cb(btn_back, [](lv_event_t*e){ 
        if (instance->_cancelCallback) instance->_cancelCallback();
        lv_screen_load(instance->scr_keypad); 
    }, LV_EVENT_CLICKED, NULL);

    qr_canvas = lv_canvas_create(scr_qr);
    lv_obj_set_size(qr_canvas, 240, 240); // 20% larger than 200
    lv_obj_align(qr_canvas, LV_ALIGN_CENTER, 0, -10);
    
    // Allocate QR buffer in PSRAM
    if (qr_buffer) free(qr_buffer);
    qr_buffer = (lv_color_t *)ps_malloc(240 * 240 * sizeof(lv_color_t));
    if (!qr_buffer) {
        Serial.println("QR Buffer PSRAM failed, using SRAM");
        qr_buffer = (lv_color_t *)malloc(240 * 240 * sizeof(lv_color_t));
    } else {
        Serial.println("QR Buffer allocated in PSRAM");
    }
    lv_canvas_set_buffer(qr_canvas, qr_buffer, 240, 240, LV_COLOR_FORMAT_RGB565);
    lbl_total = lv_label_create(scr_qr);
    lv_obj_set_style_text_color(lbl_total, lv_color_black(), 0);
    lv_obj_set_style_text_font(lbl_total, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_total, LV_ALIGN_BOTTOM_MID, 0, -15);
}

void DisplayManager::showQR(String url, float total) {
    if(_soundManager) ((SoundManager*)_soundManager)->playClick(); // Feedback for QR Ready

    Serial.printf("showQR START. URL length: %d, Free Heap: %u\n", url.length(), ESP.getFreeHeap());
    Serial.flush();
    lv_screen_load(scr_qr);
    String totalStr = "Pagar: $" + String(total, 2);
    lv_label_set_text(lbl_total, totalStr.c_str());
    
    Serial.println("Initializing QR data..."); Serial.flush();
    
    // Choose version based on data length. Version 15 is safer for typical MP URLs.
    uint8_t qrVersion = 15;
    QRCode qrcode;
    uint16_t bufferSize = qrcode_getBufferSize(qrVersion);
    
    // Use PSRAM for the modules buffer too
    uint8_t *qrcodeData = (uint8_t *)ps_malloc(bufferSize);
    if (!qrcodeData) qrcodeData = (uint8_t *)malloc(bufferSize);

    if (qrcode_initText(&qrcode, qrcodeData, qrVersion, ECC_LOW, url.c_str()) == 0) {
        Serial.println("Drawing to canvas..."); Serial.flush();
        lv_canvas_fill_bg(qr_canvas, lv_color_white(), LV_OPA_COVER);
        
        int scale = 240 / qrcode.size;
        int offset = (240 - (qrcode.size * scale)) / 2;
        
        for (uint8_t y = 0; y < qrcode.size; y++) {
            for (uint8_t x = 0; x < qrcode.size; x++) {
                lv_color_t color = qrcode_getModule(&qrcode, x, y) ? lv_color_black() : lv_color_white();
                if (scale == 1) {
                    lv_canvas_set_px(qr_canvas, offset + x, offset + y, color, LV_OPA_COVER);
                } else {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            lv_canvas_set_px(qr_canvas, offset + x * scale + sx, offset + y * scale + sy, color, LV_OPA_COVER);
                        }
                    }
                }
            }
        }
        Serial.println("QR Drawing complete."); Serial.flush();
    } else {
        Serial.println("QR Init Failed! URL might be too long or memory allocation failed.");
        setError("Error: URL too long");
    }
    
    if (qrcodeData) free(qrcodeData);
}

void DisplayManager::createReadyUI() {
    scr_ready = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_ready, lv_color_black(), 0);

    // Branding
    lv_obj_t * b_title = lv_label_create(scr_ready);
    lv_label_set_text(b_title, "Pag.ar"); // Updated branding
    lv_obj_set_style_text_color(b_title, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(b_title, &lv_font_montserrat_32, 0);
    lv_obj_align(b_title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * lbl = lv_label_create(scr_ready);
    lv_label_set_text(lbl, "Pago Recibido");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t * btn_act = lv_button_create(scr_ready);
    lv_obj_set_size(btn_act, 280, 100);
    // Lower button by ~30%: original was centered (y=160), now at y=210 (~30% lower)
    lv_obj_align(btn_act, LV_ALIGN_CENTER, 0, 50); // Moved down from center
    lv_obj_set_style_bg_color(btn_act, lv_palette_main(LV_PALETTE_BLUE), 0);

    lv_obj_t * lbl_act = lv_label_create(btn_act);
    lv_label_set_text(lbl_act, "ACTIVAR SERVICIO");
    lv_obj_set_style_text_font(lbl_act, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_act);

    lv_obj_add_event_cb(btn_act, [](lv_event_t * e){
        if (instance->_activationCallback) {
            instance->_activationCallback();
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_warn = lv_label_create(scr_ready);
    lv_label_set_text(lbl_warn, "Presione para comenzar");
    lv_obj_align(lbl_warn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_text_color(lbl_warn, lv_color_hex(0xAAAAAA), 0);
}

void DisplayManager::createSuccessUI() {
    scr_success = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_success, lv_color_black(), 0);

    // Branding
    lv_obj_t * b_title = lv_label_create(scr_success);
    lv_label_set_text(b_title, "Pag.ar"); // Updated branding
    lv_obj_set_style_text_color(b_title, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(b_title, &lv_font_montserrat_32, 0); // Montserrat_48 not enabled
    lv_obj_align(b_title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * lbl = lv_label_create(scr_success);
    lv_label_set_text(lbl, "¡GRACIAS POR\nSU COMPRA!");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0); // Fix for LVGL v9
    lv_obj_center(lbl);

    lv_obj_t * lbl_msg = lv_label_create(scr_success);
    lv_label_set_text(lbl_msg, "Disfrute su servicio");
    lv_obj_set_style_text_color(lbl_msg, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(lbl_msg, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void DisplayManager::showReady() {
    Serial.println("UI: Showing Ready to Activate Screen...");
    lv_screen_load(scr_ready);
}

void DisplayManager::setError(String msg) {
    lv_screen_load(scr_keypad);
    lv_label_set_text(lbl_amount, msg.c_str());
    lv_obj_set_style_text_color(lbl_amount, lv_palette_main(LV_PALETTE_RED), 0);
    
    lv_timer_create([](lv_timer_t * t){
        if (instance) {
            lv_label_set_text(instance->lbl_amount, instance->currentAmountStr.c_str());
            lv_obj_set_style_text_color(instance->lbl_amount, lv_color_white(), 0);
        }
    }, 3000, NULL);
}

void DisplayManager::showWarning(String msg) {
    Serial.println("UI: Showing Warning: " + msg);
    
    // Create a custom popup using a basic object
    lv_obj_t * mbox = lv_obj_create(lv_screen_active());
    lv_obj_set_size(mbox, 300, 150);
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_border_width(mbox, 2, 0);
    lv_obj_set_style_border_color(mbox, lv_color_white(), 0);
    lv_obj_set_style_radius(mbox, 10, 0);
    lv_obj_set_style_shadow_width(mbox, 20, 0);
    lv_obj_set_style_shadow_color(mbox, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(mbox, LV_OPA_50, 0);

    lv_obj_t * title = lv_label_create(mbox);
    lv_label_set_text(title, "AVISO");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t * body = lv_label_create(mbox);
    lv_label_set_text(body, msg.c_str());
    lv_obj_set_style_text_color(body, lv_color_white(), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(body, 280);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 10);

    // Auto close after 5 seconds as requested
    lv_timer_t * timer = lv_timer_create([](lv_timer_t * t){
        lv_obj_t * obj = (lv_obj_t *)lv_timer_get_user_data(t);
        if (obj) lv_obj_delete(obj);
    }, 5000, mbox);
    lv_timer_set_repeat_count(timer, 1);
}

void DisplayManager::setOperationMode(int mode) {
    _opMode = mode;
    if (!lbl_unit_price || !btnm || !cont_amount || !btn_gen) return;
    
    char buf[64];
    if (_opMode == 0) {
        sprintf(buf, "Valor Minuto: $%.2f", _pricePerUnit);
    } else {
        sprintf(buf, "Valor Credito: $%.2f", _pricePerUnit); // FIXED ENCODING (Credits)
    }
    lv_label_set_text(lbl_unit_price, buf);

    // Toggle UI Elements based on Mode
    if (_opMode == 2) {
        // Fixed Mode: Hide Keypad, Center Generate Button
        lv_obj_add_flag(btnm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cont_amount, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_unit_price, LV_OBJ_FLAG_HIDDEN);
        
        // Resize and Position Generate Button (lowered by 30%)
        lv_obj_set_size(btn_gen, 200, 80);
        lv_obj_align(btn_gen, LV_ALIGN_CENTER, 0, 50); // Lowered from center

        // Adjust Price Label Position (above button)
        lv_obj_align(lbl_price_display, LV_ALIGN_CENTER, 0, -20);
        lv_obj_set_style_text_font(lbl_price_display, &lv_font_montserrat_32, 0);

        // Show Static QR Text if available
        if (lbl_static_qr_text) {
            if (_staticQrText.length() > 0) {
                lv_label_set_text(lbl_static_qr_text, _staticQrText.c_str());
                lv_obj_clear_flag(lbl_static_qr_text, LV_OBJ_FLAG_HIDDEN);
                Serial.println("Fixed Mode: Showing static QR text: " + _staticQrText);
            } else {
                lv_obj_add_flag(lbl_static_qr_text, LV_OBJ_FLAG_HIDDEN);
                Serial.println("Fixed Mode: No static QR text to show");
            }
        }

        // Pre-fill amount
        currentAmountStr = String(_fixedUnits);
    } else {
        // Standard Mode: Show Keypad, Reset Layout
        lv_obj_clear_flag(btnm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cont_amount, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_unit_price, LV_OBJ_FLAG_HIDDEN);
        
        // Hide Static QR Text in standard modes
        if (lbl_static_qr_text) {
             lv_obj_add_flag(lbl_static_qr_text, LV_OBJ_FLAG_HIDDEN);
             lv_label_set_text(lbl_static_qr_text, ""); // Clear text just in case
        }
        
        // Reset Generate Button
        lv_obj_set_size(btn_gen, 120, 150);
        lv_obj_align(btn_gen, LV_ALIGN_BOTTOM_RIGHT, -10, -25);

        // Reset Label Position
        lv_obj_align(lbl_price_display, LV_ALIGN_TOP_RIGHT, -20, 75);
        lv_obj_set_style_text_font(lbl_price_display, &lv_font_montserrat_24, 0);

        // Reset amount if switching back (optional, user experience decision)
        if(currentAmountStr == String(_fixedUnits)) currentAmountStr = "0";
    }
    
    // Fix: Clear Promo Label when switching modes
    if (lbl_promo_info) {
        lv_label_set_text(lbl_promo_info, ""); 
    }
    
    lv_label_set_text(lbl_amount, currentAmountStr.c_str());
}

void DisplayManager::setPricePerUnit(float price) {
    _pricePerUnit = price;
    setOperationMode(_opMode); // Refresh label
}

void DisplayManager::setWiFiStatus(bool connected) {
    if (!lbl_wifi_status) return;
    if (connected) {
        lv_obj_set_style_bg_color(lbl_wifi_status, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        lv_obj_set_style_bg_color(lbl_wifi_status, lv_palette_main(LV_PALETTE_RED), 0);
    }
}

void DisplayManager::setPromoConfig(bool enabled, int threshold, int type, float value) {
    _promoEnabled = enabled;
    _promoThreshold = threshold;
    _promoType = type;
    _promoValue = value;
}

void DisplayManager::setFixedModeConfig(int units) {
    _fixedUnits = units;
    if (_opMode == 2) {
        // Force update to fixed amount
        currentAmountStr = String(_fixedUnits);
        if(lbl_amount) lv_label_set_text(lbl_amount, currentAmountStr.c_str());
        
        // Trigger calculation updates (price display)
        lv_obj_send_event(lbl_amount, LV_EVENT_VALUE_CHANGED, NULL); // Hacky, better call handler logic directly or split it
        // Since we can't easily emit event to handler without obj, we replicate logic:
        float total = _fixedUnits * _pricePerUnit;
        
        // Apply Promotion Logic to Display
        if (_promoEnabled && _fixedUnits >= _promoThreshold) {
            float discountFactor = (100.0 - _promoValue) / 100.0;
            total = total * discountFactor;
            Serial.println("DisplayManager: Showing discounted price in Fixed Mode");
        }

        char buf[32];
        sprintf(buf, "Total: $%.2f", total);
        if(lbl_price_display) lv_label_set_text(lbl_price_display, buf);
    }
}

void DisplayManager::setStaticQrText(String text) {
    _staticQrText = text;
    Serial.println("DisplayManager: setStaticQrText called with: '" + text + "'");
    // Update the label if it exists
    if (lbl_static_qr_text) {
        if (_staticQrText.length() > 0) {
            lv_label_set_text(lbl_static_qr_text, _staticQrText.c_str());
            if (_opMode == 2) {
                lv_obj_clear_flag(lbl_static_qr_text, LV_OBJ_FLAG_HIDDEN);
                Serial.println("DisplayManager: Label updated and made visible");
            } else {
                Serial.println("DisplayManager: Label updated but staying hidden (Not Fixed Mode)");
            }
        } else {
            lv_label_set_text(lbl_static_qr_text, "");
            lv_obj_add_flag(lbl_static_qr_text, LV_OBJ_FLAG_HIDDEN);
            Serial.println("DisplayManager: Label cleared and hidden");
        }
    } else {
        Serial.println("DisplayManager: WARNING - lbl_static_qr_text is NULL!");
    }
}


void DisplayManager::showSuccess() {
    Serial.println("UI: Showing Success Screen...");
    lv_screen_load(scr_success);
    
    lv_timer_t * timer = lv_timer_create([](lv_timer_t * t){
        if (instance) {
            lv_screen_load(instance->scr_keypad);
            instance->currentAmountStr = "0";
            lv_label_set_text(instance->lbl_amount, "0");
            lv_label_set_text(instance->lbl_price_display, "Total: $0.00");
        }
    }, 10000, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

void DisplayManager::loop() {
    static unsigned long lastTick = 0;
    if (millis() - lastTick >= 5) {
        lv_tick_inc(millis() - lastTick);
        lastTick = millis();
    }
    lv_timer_handler();

    if (_showingInfo && (millis() - _infoStartTime > 30000)) {
        showKeypad();
    }

    if (!_isShowingAds && (millis() - _lastActivity > 60000)) {
        showAds();
    }
}

void DisplayManager::addActivity() {
    _lastActivity = millis();
    if (_isShowingAds) stopAds();
}

bool DisplayManager::tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (instance == nullptr || instance->_ad_buffer == nullptr) return false;
    
    // Copy block to our PSRAM buffer
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int targetX = x + i;
            int targetY = y + j;
            if (targetX < 480 && targetY < 320) {
                uint16_t color = bitmap[j * w + i];
                // Byte swap was causing color issues
                // color = (color >> 8) | (color << 8);
                instance->_ad_buffer[targetY * 480 + targetX] = color;
            }
        }
    }
    return true;
}

void DisplayManager::showAds() {
    if (_isShowingAds) return;
    
    Serial.println("ADS: Triggering Advertising Carousel...");
    
    // Check if there are images in /ads
    File root = SD.open("/ads");
    if (!root || !root.isDirectory()) {
        Serial.println("ADS ERROR: /ads directory not found on SD!");
        _lastActivity = millis(); 
        return;
    }
    
    // Quick check for at least one file
    bool hasFiles = false;
    File f = root.openNextFile();
    while(f) {
        if(!f.isDirectory()) {
            hasFiles = true;
            f.close();
            break;
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();

    if (!hasFiles) {
        Serial.println("ADS: No files found in /ads directory.");
        _lastActivity = millis();
        return;
    }

    _isShowingAds = true;
    _currentAdIndex = 0;
    lv_screen_load(scr_ads);
    Serial.println("ADS: Screen loaded.");
    
    // Create timer for rotation
    _adTimer = lv_timer_create([](lv_timer_t * t){
        instance->_isShowingAds = true; 
        
        File root = SD.open("/ads");
        if(!root) {
             Serial.println("ADS: Failed to open /ads for rotation.");
             return;
        }

        int count = 0;
        File file = root.openNextFile();
        String targetFile = "";
        
        // Find next image based on index
        while (file) {
            String fname = String(file.name());
            
            // Fix path construction: if name starts with /, don't add another /
            String fullPath;
            if (fname.startsWith("/")) fullPath = "/ads" + fname; // Usually not the case
            else fullPath = "/ads/" + fname;
             
            // Some SD libraries return full path " /ads/image.jpg", others just "image.jpg"
            // Let's normalize. If fname contains "ads", assume full path.
            if(fname.indexOf("ads/") >= 0) fullPath = fname;
            if(!fullPath.startsWith("/")) fullPath = "/" + fullPath;

            String lowName = fullPath;
            lowName.toLowerCase();
            
            if (!file.isDirectory() && (lowName.endsWith(".jpg") || lowName.endsWith(".jpeg"))) {
                if (count == instance->_currentAdIndex) {
                    targetFile = fullPath;
                }
                count++;
            }
            file.close(); // IMPORTANT: Close handle
            file = root.openNextFile();
        }
        root.close(); // IMPORTANT: Close directory handle
        
        if (targetFile != "" && count > 0) {
            uint16_t jpgW = 0, jpgH = 0;
            TJpgDec.getSdJpgSize(&jpgW, &jpgH, targetFile.c_str());
            Serial.printf("ADS: Decoding [%d/%d]: %s (%dx%d)\n", instance->_currentAdIndex + 1, count, targetFile.c_str(), jpgW, jpgH);
            
            if (jpgW > 0 && jpgH > 0) {
                // Clear buffer to black before decoding new image
                memset(instance->_ad_buffer, 0, 480 * 320 * sizeof(uint16_t));
    
                // Decode directly from SD into _ad_buffer via tft_output callback
                TJpgDec.drawSdJpg(0, 0, targetFile.c_str());
            } else {
                Serial.println("ADS: Failed to get JPG size or invalid image.");
            }
            
            // Refresh image source
            lv_image_set_src(instance->img_ad, &instance->_ad_img_dsc);
            lv_obj_invalidate(instance->img_ad);
            
            instance->_currentAdIndex = (instance->_currentAdIndex + 1) % count;
        } else {
            Serial.println("ADS: No valid target file found or index out of bounds.");
            instance->_currentAdIndex = 0;
        }
    }, 10000, NULL); // 10s per ad
    lv_timer_ready(_adTimer); // Trigger first one immediately
}

void DisplayManager::stopAds() {
    _isShowingAds = false;
    if (_adTimer) {
        lv_timer_delete(_adTimer);
        _adTimer = nullptr;
    }
}
