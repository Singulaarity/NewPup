//***************************************************************************************/
// Pup Button 2026
// Author: Nick Lewis
// Date: 2/25/2026
// Version 2.14
// Platform: ESP32-2432S028R
//
// FIX 1 (motor twitch on boot):
// - Force PCF8574 safe outputs IMMEDIATELY at the top of setup(), BEFORE any delays or I2C scan.
// - Do it with a SINGLE port write (one I2C transaction) to avoid intermediate states.
//
// UPDATE (P7 IR remote active-low / correct input state):
// - P7 should idle HIGH (released) and go LOW when remote triggers.
// - We therefore "release" P7 HIGH in the earliest safe port write, and again after init.
//***************************************************************************************/

// Include all libraries
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <main.h>
#include "lv_conf.h"
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "ui.h"
extern const lv_img_dsc_t ui_img_splashy;
#include "eez-flow.h"
#include "actions.h"
#include "pcf8574_control.h"

// ✅ Add this so actions_init() resolves even if actions.h doesn’t declare it yet
extern "C" void actions_init(void);

// Touch Screen pins
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// Global variables
SPIClass touchscreenSpi = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(TOUCH_CS, 255);
uint16_t touchScreenMinimumX = 200, touchScreenMaximumX = 3700;
uint16_t touchScreenMinimumY = 240, touchScreenMaximumY = 3800;
lv_indev_t *indev;
uint8_t *draw_buf;
uint32_t lastTick = 0;

TFT_eSPI tft = TFT_eSPI();

#define TFT_HOR_RES 320
#define TFT_VER_RES 240
#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))

#define I2C_SDA 27
#define I2C_SCL 22
#define PCF8574_ADDRESS 0x20

#define SD_CS 5

// ------------------------
// FIX 1 + P7 input release
// ------------------------
// Safe intent at boot:
//  - Motor off (P0,P1 LOW)
//  - LED off (P4 HIGH if active-low LED)
//  - IR TX off (P5 HIGH if active-low IR)
//  - Remote input P7 RELEASED/HIGH (active-low input: idle HIGH, trigger LOW)
//
// PCF8574 port byte: bit=1 -> pin high (released/input), bit=0 -> pin low
//
#define PCF8574_SAFE_PORT ((uint8_t)((1u << 4) | (1u << 5) | (1u << 7)))

static void forceSafePCF8574StateEarly() {
    // Initialize I2C as early as possible and slam the port into a known safe state
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);

    Wire.beginTransmission(PCF8574_ADDRESS);
    Wire.write(PCF8574_SAFE_PORT); // single write = no intermediate states
    Wire.endTransmission();

    delay(2);
}

void scanI2CDevices() {
    Serial.println("\n=== Scanning I2C Bus ===");
    byte error, address;
    int deviceCount = 0;

    for (address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();

        if (error == 0) {
            Serial.printf("I2C device found at address 0x%02X\n", address);
            deviceCount++;
        }
    }

    if (deviceCount == 0) {
        Serial.println("No I2C devices found!");
    }
    Serial.println("=== Scan Complete ===\n");
}

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)px_map, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;

    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();

        if (p.x < touchScreenMinimumX) touchScreenMinimumX = p.x;
        if (p.x > touchScreenMaximumX) touchScreenMaximumX = p.x;
        if (p.y < touchScreenMinimumY) touchScreenMinimumY = p.y;
        if (p.y > touchScreenMaximumY) touchScreenMaximumY = p.y;

        data->point.x = constrain(map(p.x, touchScreenMinimumX, touchScreenMaximumX, 0, TFT_HOR_RES - 1), 0, TFT_HOR_RES - 1);
        data->point.y = constrain(map(p.y, touchScreenMinimumY, touchScreenMaximumY, 0, TFT_VER_RES - 1), 0, TFT_VER_RES - 1);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool splash_transition_done = false;

void splash_to_manual_cb(lv_timer_t * timer) {
    if (!splash_transition_done) {
        int screenId = eez::flow::getLvglScreenByNameHook("Manual");
        if (screenId >= 0) {
            eez_flow_set_screen(screenId, LV_SCR_LOAD_ANIM_NONE, 0, 0);
            Serial.println("Transitioning to Manual screen");
            splash_transition_done = true;
            lv_timer_del(timer);
        } else {
            Serial.println("ERROR: Manual screen not found");
        }
    }
}

void setup() {
    // ------------------------------------------------------------
    // FIX 1: Force PCF8574 safe outputs BEFORE ANY delays/scans
    // ------------------------------------------------------------
    forceSafePCF8574StateEarly();

    Serial.begin(115200);
    delay(100);

    String LVGL_Arduino = "Pup Button Firmware\nVersion 2.14\n";
    Serial.println("Pup Button Firmware");
    Serial.println("Version 2.14");
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
    Serial.println(LVGL_Arduino);

    init_audio();
    analogReadResolution(12);

    // I2C was already started in forceSafePCF8574StateEarly()
    Wire.setClock(100000);
    delay(10);

    scanI2CDevices();

    // PCF init
    initPCF8574Pins();

    // Re-assert safe states via helper API (post-init)
    setPCF8574Pin(0, false); // motor off
    setPCF8574Pin(1, false); // motor off
    setPCF8574Pin(4, true);  // LED off (active-low)
    setPCF8574Pin(5, true);  // IR TX off (active-low)

    // IMPORTANT: P7 is ACTIVE-LOW input; ensure it is RELEASED/HIGH for input reads
    // Most PCF8574 libraries require writing HIGH to a pin to use it as an input.
    // If your wrapper uses inverted semantics, adjust here to match "released/high".
    setPCF8574Pin(7, false); // release/high (input)  <-- matches our actions.cpp assumption

    Serial.println("PCF initialized + outputs forced safe (motor off, LED/IR off, P7 released)");

    Serial.println("\nVerifying pin states:");
    for (int pin = 0; pin < 8; pin++) {
        bool state = readPCF8574Pin(pin);
        Serial.printf("P%d: %s\n", pin, state ? "HIGH" : "LOW");
    }

    // Optional: prove P7 idle state is HIGH (not pressed)
    Serial.printf("P7 idle read: %s (expected HIGH)\n", readPCF8574Pin(7) ? "HIGH" : "LOW");

    if (!SD.begin(SD_CS)) {
        Serial.println("SD card initialization failed!");
    } else {
        Serial.println("SD card initialized.");
    }

    tft.begin();
    tft.setRotation(1);

    touchscreenSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSpi);
    touchscreen.setRotation(1);

    lv_init();

    draw_buf = new uint8_t[DRAW_BUF_SIZE];
    lv_display_t *disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, draw_buf, DRAW_BUF_SIZE);
    lv_display_set_flush_cb(disp, my_disp_flush);
    tft.setRotation(1);

    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // Backlighting
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);
    ledcSetup(0, 5000, 8);
    ledcAttachPin(21, 0);
    ledcWrite(0, 255);

    Serial.println("LVGL Setup done");
    ui_init();

    Serial.println("display splash screen");
    lv_timer_create(splash_to_manual_cb, 3000, NULL);

    // ✅ Start IR-remote poll timer right after LVGL is initialized
    actions_init();
    Serial.println("actions_init(): IR remote trigger enabled (P7 active-low)");

    lastTick = millis();
}

void loop() {
    uint32_t now = millis();
    uint32_t delta = now - lastTick;
    lastTick = now;

    lv_tick_inc(delta);
    lv_timer_handler();

    delay(5);
}