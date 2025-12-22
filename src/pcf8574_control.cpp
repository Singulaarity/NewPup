#include "pcf8574_control.h"
#include <Wire.h>

#define PCF8574_ADDRESS 0x20
// Keep currentPinState accurate; never read the expander just to modify
static uint8_t currentPinState = 0xFF;  // 1 = released (input/high), 0 = driven low

// Define which pins are inputs (mask) so we only force those back to input.
static constexpr uint8_t INPUT_PINS_MASK =
    (1 << PIN_BUTTON) | (1 << PIN_ROT_DETECT) | (1 << PIN_IR_RX) | (1 << PIN_REMOTE_RX);

// Ensure input pins cannot be latched LOW
static void writePort(uint8_t newState) {
    uint8_t requested = newState;
    // Force all input bits HIGH (released)
    newState |= INPUT_PINS_MASK;

    // If caller tried to clear button bit, note it
    if ((requested & (1 << PIN_BUTTON)) == 0 && (currentPinState & (1 << PIN_BUTTON)) != 0) {
        Serial.println("[WARN] Attempt to latch BUTTON (P3) LOW blocked; forcing HIGH.");
    }

    if (newState == currentPinState) return;
    currentPinState = newState;
    Wire.beginTransmission(PCF8574_ADDRESS);
    Wire.write(currentPinState);
    Wire.endTransmission();
}

// Add helper to guarantee P3 (button) latch is HIGH (input/released) if it was ever cleared.
static inline void ensureButtonReleased() {
    const uint8_t mask = (1 << PIN_BUTTON);
    if ((currentPinState & mask) == 0) {
        // Re‑assert only the button bit (other bits unchanged)
        uint8_t newState = currentPinState | mask;
        Wire.beginTransmission(PCF8574_ADDRESS);
        Wire.write(newState);
        Wire.endTransmission();
        currentPinState = newState;
        Serial.println("[FIX] P3 latch re‑released (set HIGH).");
    }
}

void initPCF8574Pins() {
    writePort(0xFF); // release all pins once
    ensureButtonReleased(); // make sure P3 is not stuck low
    Serial.println("PCF8574 initialized (all pins HIGH, P3 input)");
}

// Update: do NOT write the port after reading; just return the bit.
bool readPCF8574Pin(uint8_t pin) {
    Wire.requestFrom(PCF8574_ADDRESS, 1);
    if (Wire.available()) {
        uint8_t value = Wire.read();
        return (value & (1 << pin)) != 0;
    }
    return true;  // default HIGH if read fails
}

// Add this helper to read the whole port (for debug)
bool readPCF8574Port(uint8_t &portValue) {
    Wire.requestFrom(PCF8574_ADDRESS, 1);
    if (Wire.available()) {
        portValue = Wire.read();
        return true;
    }
    portValue = 0xFF;
    return false;
}

// Debug helper: dump cached vs live
void debugDumpPCF(const char *tag) {
    uint8_t live;
    bool ok = readPCF8574Port(live);
    Serial.print("[PCF] "); Serial.print(tag);
    Serial.print(" cached=0x"); Serial.print(currentPinState, HEX);
    Serial.print(" live=");
    if (ok) { Serial.print("0x"); Serial.print(live, HEX); }
    else Serial.print("READ_FAIL");
    Serial.print(" BTNbit(live)=");
    Serial.println( (live >> PIN_BUTTON) & 1 );
}

// Optional combined read (returns bit and also live byte via ref)
bool readPCF8574PinDebug(uint8_t pin, uint8_t &liveByte) {
    Wire.requestFrom(PCF8574_ADDRESS, 1);
    if (Wire.available()) {
        liveByte = Wire.read();
        return (liveByte & (1 << pin)) != 0;
    }
    liveByte = 0xFF;
    return true;
}

// Optional: explicit restore (call if you ever suspect latch corruption)
void restoreInputPinsHigh() {
    uint8_t forced = currentPinState | INPUT_PINS_MASK;
    if (forced != currentPinState) {
        writePort(forced);
        Serial.println("[FIX] Restored input latch bits HIGH.");
    }
}

// Safe single-pin set (leave rest of file same, but make sure it calls writePort)
void setPCF8574Pin(uint8_t pin, bool highRelease) {
    if (!highRelease && (INPUT_PINS_MASK & (1 << pin))) {
        static bool warned = false;
        if (!warned) {
            Serial.print("[WARN] Ignored drive LOW on input pin "); Serial.println(pin);
            warned = true;
        }
        return;
    }
    uint8_t newState = highRelease
        ? (currentPinState |  (1 << pin))
        : (currentPinState & ~(1 << pin));

    writePort(newState);
}

// Optional helper you can call from train_dispense_tick:
void logPCFPortP3() {
    ensureButtonReleased(); // auto-fix before logging / reading
    uint8_t portByte;
    if (readPCF8574Port(portByte)) {
        Serial.print("PORT=0x"); Serial.print(portByte, HEX);
        Serial.print(" P3(bit3)="); Serial.println( (portByte >> 3) & 1 );
    } else {
        Serial.println("PORT READ FAIL");
    }
}
