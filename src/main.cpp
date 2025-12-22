//***************************************************************************************/
// Pup Button 2025
// Author: Dustin Wish
// Date: 8/7/2025
// Version 1.35
// Platform: ESP32-2432S028R
//
//    _______________
//    '------._.------'\
//      \_______________\
//     .'|            .'|
//    .'_____________.' .|
//    |              |   |
//    |  Scooby _.-. | . |
//    |  *     (_.-' |   |
//    |    Snacks    |  .|
//    | *          * |  .'
//    |______________|.' 
//
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
//#include <PCF8574.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "ui.h"
extern const lv_img_dsc_t ui_img_splashy;
#include "eez-flow.h"
#include "actions.h"  
#include "pcf8574_control.h" // Include the new control header

// Touch Screen pins
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// Global variables
SPIClass touchscreenSpi = SPIClass(VSPI);
//XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ); -- old code
XPT2046_Touchscreen touchscreen(TOUCH_CS, 255); // -- from ino
uint16_t touchScreenMinimumX = 200, touchScreenMaximumX = 3700;
uint16_t touchScreenMinimumY = 240, touchScreenMaximumY = 3800;
lv_indev_t *indev;      //Touchscreen input device
uint8_t *draw_buf;      //draw_buf is allocated on heap
uint32_t lastTick = 0;  //Used to track the tick timer

// old tft call
TFT_eSPI tft = TFT_eSPI();  // Use pins from User_Setup.h

// Screen resolution
// landscape (matches a 320×240 UI)
#define TFT_HOR_RES 320
#define TFT_VER_RES 240
#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))

//***************************************************************************************/
// I2C for PCF8574
#define I2C_SDA 27
#define I2C_SCL 22
// PCF8574 I2C address (usually 0x20)
#define PCF8574_ADDRESS 0x20
// Create PCF8574 object
//PCF8574 pcf8574(PCF8574_ADDRESS);
#define SD_CS 5


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
        } else {
            Serial.printf("Error %d at address 0x%02X\n", error, address);
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

// Remove the extra transition functions and keep just the splash screen callback
void splash_to_manual_cb(lv_timer_t * timer) {
    if (!splash_transition_done) {
        int screenId = eez::flow::getLvglScreenByNameHook("Manual");
        if (screenId >= 0) {
            eez_flow_set_screen(screenId, LV_SCR_LOAD_ANIM_NONE, 0, 0);
            Serial.println("Transitioning to Manual screen");
            splash_transition_done = true;
            
            // Delete the timer after transition
            lv_timer_del(timer);
        } else {
            Serial.println("ERROR: Manual screen not found");
        }
    }
}

// Add these debug helpers at the top
#define PCF8574_RETRY_COUNT 3
#define PCF8574_INIT_DELAY 200  // Increased delay between retries

void setup() {
  Serial.begin(115200);
  String LVGL_Arduino = "Pup Button Firmware\nVersion 1.0\n";
  Serial.println("Pup Button Firmware");
  Serial.println("Version 1.0");
  LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
  Serial.println(LVGL_Arduino);
  // Initialize I2C
  delay(1000); // Give serial time to initialize
  init_audio(); 
  analogReadResolution(12);  // ESP32 default is 12-bit (0-4095)
  // Always initialize I2C with correct pins first!
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);  
  delay(10); // Add a small delay after I2C initialization
 
   // Scan and show available I2C devices
   // debugging the PF8574 
    scanI2CDevices(); 

    // Initialize PCF8574 pins with proper configuration
    initPCF8574Pins();     // <- add here
   // pcf8574_boot_selfcheck(); // optional debug (remove later)

    
    // Verify initial states
    Serial.println("\nVerifying pin states:");
    for (int pin = 0; pin < 8; pin++) {
        bool state = readPCF8574Pin(pin);
        Serial.printf("P%d: %s\n", pin, state ? "HIGH" : "LOW");
    }
   
    // Initialize SD card
    if (!SD.begin(SD_CS)) {
        Serial.println("SD card initialization failed!");
    } else {
        Serial.println("SD card initialized.");
    }
   tft.begin();
   tft.setRotation(1);

  // Initialize touchscreen
  touchscreenSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSpi);
  touchscreen.setRotation(1); // Set rotation to match display orientation

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
  ledcSetup(0, 5000, 8);    // Channel 0, 5000 Hz, 8-bit resolution
  ledcAttachPin(21, 0);     // Attach pin 21 to channel 0
  ledcWrite(0, 255);        // Set maximum brightness

    Serial.println("LVGL Setup done");
    ui_init();

    Serial.println("display splash screen");
    // Create timer for splash screen transition
    lv_timer_create(splash_to_manual_cb, 3000, NULL);

    //    Wire.begin();
    setPCF8574Pin(0, false); // IN1 HIGH (motor off)
    setPCF8574Pin(1, false); // IN2 HIGH (motor off)
    Serial.println("turn motor off");
    // Initialize teh SD card
    //SD.begin(SD_CS); // Initialize SD card}
    setPCF8574Pin(4, true); // LED OFF
   
}
void loop() {
    lv_timer_handler();  // LVGL event loop
    lv_tick_inc(millis() - lastTick);
    lastTick = millis();
    
    delay(5);
}
