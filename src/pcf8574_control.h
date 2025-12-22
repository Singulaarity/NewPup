#ifndef PCF8574_CONTROL_H
#define PCF8574_CONTROL_H

#include <Arduino.h>

// Pin definitions
#define PIN_MOTOR_IN1    0  // Output: H-bridge IN1 control
#define PIN_MOTOR_IN2    1  // Output: H-bridge IN2 control
#define PIN_ROT_DETECT   2  // Input: Rotation detection switch
#define PIN_BUTTON       3  // Input: Button input
#define PIN_LED          4  // Output: LED control
#define PIN_IR_TX        5  // Output: IR transmitter control
#define PIN_IR_RX        6  // Input: IR receiver signal
#define PIN_REMOTE_RX    7  // Input: Remote control IR receiver

#ifdef __cplusplus
extern "C" {
#endif

// Function declarations
void initPCF8574Pins();
bool readPCF8574Pin(uint8_t pin);
void setPCF8574Pin(uint8_t pin, bool state);
void logPCFPortP3();  // optional debug

#ifdef __cplusplus
}
#endif

#endif // PCF8574_CONTROL_H
