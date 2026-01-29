/*
 * DisplayTest.ino
 *
 * Direct Arduino_GFX test for JC3248W535EN (ESP32-S3 + AXS15231B)
 *
 * Pins (QSPI):
 * CS: 45, SCK: 47, D0: 21, D1: 48, D2: 40, D3: 39
 * Backlight: 1
 *
 * IMPORTANT:
 * 1. Requires "Arduino_GFX" library by MoonOnOurNation.
 * 2. Board settings: OPI PSRAM, 16MB Flash.
 */

#include <Arduino_GFX_Library.h>

#define GFX_BL 1

Arduino_DataBus *bus =
    new Arduino_ESP32QSPI(45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */,
                          40 /* D2 */, 39 /* D3 */);

Arduino_GFX *gfx =
    new Arduino_AXS15231B(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */,
                          false /* IPS */, 320 /* width */, 480 /* height */
    );

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting Native Display Test...");

  // Init Display
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->fillScreen(BLACK);
  delay(500);

  Serial.println("Red...");
  gfx->fillScreen(RED);
  delay(500);

  Serial.println("Green...");
  gfx->fillScreen(GREEN);
  delay(500);

  Serial.println("Blue...");
  gfx->fillScreen(BLUE);
  delay(500);

  gfx->fillScreen(BLACK);
  gfx->setCursor(10, 50);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->println("Native GFX Test");
  gfx->setTextSize(2);
  gfx->println("QSPI Mode");
}

void loop() {
  gfx->fillCircle(random(gfx->width()), random(gfx->height()), 10,
                  random(0xFFFF));
  delay(100);
}
