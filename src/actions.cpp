#include <Arduino.h>
#include <stdlib.h>
#include "ui.h"
#include "vars.h"   // keep this if it declares any of these; guards below prevent redefinition
#include "actions.h"
#include <SD.h>
#include <FS.h>
#include "driver/dac.h"
#include "pcf8574_control.h"
#include <Wire.h>
#include <IRremoteESP8266.h>

// ---- Restore / fallback definitions (only used if not already provided elsewhere) ----
#ifndef AUDIO_DAC_CHANNEL
#define AUDIO_DAC_CHANNEL DAC_CHANNEL_1   // or DAC_CHANNEL_2 if you wired to GPIO25
#endif

#ifndef WAV_FILE
#define WAV_FILE "/treat.wav"             // SD path to your audio file
#endif

#ifndef CURRENT_SENSOR_PIN
#define CURRENT_SENSOR_PIN 35
#endif

#ifndef ZERO_CURRENT_VOLTAGE
#define ZERO_CURRENT_VOLTAGE 2.50f
#endif

#ifndef SENSITIVITY
#define SENSITIVITY 0.185f
#endif

#ifndef JAM_THRESHOLD_AMPS
#define JAM_THRESHOLD_AMPS 5.5f
#endif

#ifndef TRAIN_MOTOR_RUN_MS
#define TRAIN_MOTOR_RUN_MS 5000UL   // Motor run duration after button press (5 seconds)
#endif
// ---- End fallback definitions ----

// State variables for training dispense
volatile bool train_dispense_stop_requested = false;
static int  train_dispense_state = 0;
static unsigned long state_start_time = 0;
static bool led_blink_state = false;     // single definition
static unsigned long last_blink = 0;     // single definition

// Schedule training state variables
static bool schedule_waiting_for_button = false;
static int schedule_train_state = 0;
static unsigned long schedule_state_start_time = 0;
static bool schedule_led_blink_state = false;
static unsigned long schedule_last_blink = 0;
volatile bool schedule_stop_requested = false;  // Separate stop flag for schedules

int selected_treats_number = 1;
int selected_hours_to_dispense = 2;  // Default to first roller option (2 hours)

// Schedule state variables
int schedule_treats_dispensed = 0;
int schedule_remaining_minutes = 0;
bool schedule_is_running = false;
bool schedule_is_paused = false;
void* schedule_timer = NULL; // Timer reference for schedule management
static unsigned long schedule_start_time = 0;
static unsigned long schedule_pause_time = 0;
static unsigned long last_minute_update = 0;

// Schedule timing arrays
static int scheduled_times[96]; // Max 8 hours * 12 treats = 96 treats
static int total_scheduled_treats = 0;
static int current_treat_index = 0;

// LED control state
static bool led_blink_mode = false;
static bool led_is_on_solid = false;

// Active Dispense and Rotary Switch Variables
bool treatDispensed = false;

bool lastRotary = readPCF8574Pin(2);   // initial rotary state
int  lhTransitions = 0;                // LOW->HIGH transitions seen (only when no treat)

bool stopRequested_NoTreat = false;    // set true after 3 transitions with no treat
bool waitForNextHigh_AfterTreat = false;

bool seenLowAfterTreat = false;        // ensures "next HIGH" after dispense, not "current HIGH"

// Active-low LED helpers (place BEFORE usage)
static inline void led_apply(bool on) {
    // on=true => drive low (active-low LED ON)
    setPCF8574Pin(PIN_LED, !on);
}

static inline void led_set_solid(bool on) {
    led_blink_mode = false;
    if (led_is_on_solid != on) {
        led_is_on_solid = on;
    }
    led_apply(on); // ensure hardware matches
}

static inline void led_blink_tick(unsigned long now) {
    if (!led_blink_mode) return;
    if (now - last_blink >= 250) {
        last_blink = now;
        led_blink_state = !led_blink_state;
        // led_blink_state true => LED ON (choose consistent meaning)
        led_apply(led_blink_state);
    }
}

extern "C" {
    
    // Forward declarations
    void init_audio();
    void play_treat_audio();
    void full_stop();
    void update_schedule_3_ui();
    
    // Function to immediately update schedule 3 UI with current values
    void update_schedule_3_ui() {
        // Update treats per hour display
        if (objects.treats_per_hour) {
            char treats_str[10];
            snprintf(treats_str, sizeof(treats_str), "%d", selected_treats_number);
            lv_label_set_text(objects.treats_per_hour, treats_str);
        }
        
        // Update treats dispensed counter
        if (objects.treats_dispensed) {
            char dispensed_str[10];
            snprintf(dispensed_str, sizeof(dispensed_str), "%d", schedule_treats_dispensed);
            lv_label_set_text(objects.treats_dispensed, dispensed_str);
        }
        
        // Update time left display
        if (objects.schedule_time_left) {
            char time_str[10];
            if (schedule_is_running) {
                // Show live countdown when running
                int hours = schedule_remaining_minutes / 60;
                int minutes = schedule_remaining_minutes % 60;
                snprintf(time_str, sizeof(time_str), "%d:%02d", hours, minutes);
            } else {
                // Show initial time when not running
                snprintf(time_str, sizeof(time_str), "%d:00", selected_hours_to_dispense);
            }
            lv_label_set_text(objects.schedule_time_left, time_str);
        }
        
        Serial.printf("Updated Schedule 3 UI: treats=%d, dispensed=%d, hours=%d, remaining_min=%d\n", 
                     selected_treats_number, schedule_treats_dispensed, selected_hours_to_dispense, schedule_remaining_minutes);
    }
    
    // Working button detection from v1.3 - restored
    static inline bool button_pressed_debounced(unsigned long now_ms) {
        bool raw = !readPCF8574Pin(PIN_BUTTON); // pressed = LOW
        static bool prev = false; static unsigned long t = 0;
        if (raw != prev) { prev = raw; t = now_ms; }
        return raw && (now_ms - t > 30);
    }
    
    static inline bool button_edge_pressed() {
        // Active-low: pressed when pin reads LOW
        bool raw = !readPCF8574Pin(PIN_BUTTON);
        static bool last = false;
        bool edge = raw && !last;   // LOW now, was HIGH before
        last = raw;
        return edge;
    }
    
    void Motor_Start();
    void IR_Start();
    void IR_Stop();
    
    // Schedule algorithm functions
    void generate_schedule_times() {
        total_scheduled_treats = 0;
        current_treat_index = 0;
        
        int treats_per_hour = selected_treats_number;
        int total_hours = selected_hours_to_dispense;
        
        // Calculate guaranteed treats (every 30 min if possible)
        int guaranteed_treats = (treats_per_hour >= 2) ? 2 : treats_per_hour;
        int remaining_treats = treats_per_hour - guaranteed_treats;
        
        for (int hour = 0; hour < total_hours; hour++) {
            int hour_offset = hour * 60; // Convert hour to minutes
            
            // Guaranteed treat at start of hour
            if (guaranteed_treats >= 1) {
                scheduled_times[total_scheduled_treats++] = hour_offset;
            }
            
            // Guaranteed treat at 30 minutes if we have enough treats per hour
            if (guaranteed_treats >= 2) {
                scheduled_times[total_scheduled_treats++] = hour_offset + 30;
            }
            
            // Randomly distribute remaining treats
            for (int i = 0; i < remaining_treats; i++) {
                int random_minute;
                bool valid_time = false;
                int attempts = 0;
                
                // Try to find a valid random time (avoid clustering)
                while (!valid_time && attempts < 20) {
                    if (guaranteed_treats >= 2) {
                        // Choose from 5-25 or 35-55 minute ranges
                        if (random() % 2 == 0) {
                            random_minute = hour_offset + 5 + (random() % 21); // 5-25
                        } else {
                            random_minute = hour_offset + 35 + (random() % 21); // 35-55
                        }
                    } else {
                        // Full hour available except first few minutes
                        random_minute = hour_offset + 5 + (random() % 55); // 5-59
                    }
                    
                    // Check if this time is too close to existing times
                    valid_time = true;
                    for (int j = 0; j < total_scheduled_treats; j++) {
                        if (abs(scheduled_times[j] - random_minute) < 5) {
                            valid_time = false;
                            break;
                        }
                    }
                    attempts++;
                }
                
                if (valid_time) {
                    scheduled_times[total_scheduled_treats++] = random_minute;
                }
            }
        }
        
        // Sort the schedule times
        for (int i = 0; i < total_scheduled_treats - 1; i++) {
            for (int j = i + 1; j < total_scheduled_treats; j++) {
                if (scheduled_times[i] > scheduled_times[j]) {
                    int temp = scheduled_times[i];
                    scheduled_times[i] = scheduled_times[j];
                    scheduled_times[j] = temp;
                }
            }
        }
        
        Serial.println("=== Generated Schedule ===");
        for (int i = 0; i < total_scheduled_treats; i++) {
            int hours = scheduled_times[i] / 60;
            int mins = scheduled_times[i] % 60;
            Serial.printf("Treat %d: %d:%02d\n", i + 1, hours, mins);
        }
    }
    
    void schedule_dispense_treat() {
        Serial.println("=== Schedule Treat Dispense ===");
        // Stop motor and IR
            full_stop();

        // First treat (index 0) dispenses immediately
        if (current_treat_index == 0) {
            Serial.println("First treat - dispensing immediately");
            
            // Play treat sound first, then turn on LED
            play_treat_audio();
            
            // Same dispense logic as manual treat
            setPCF8574Pin(4, false); // LED ON
            Motor_Start();
            IR_Start();
            
            const unsigned long MOTOR_TIMEOUT = 5000; // 5 seconds
            bool treatDispensed = false;
            unsigned long startTime = millis();
            
            while (!treatDispensed && (millis() - startTime < MOTOR_TIMEOUT)) {
                // Current monitoring and jam detection (same as manual)
                int adcValue = analogRead(CURRENT_SENSOR_PIN);
                float voltage = (adcValue / 4095.0) * 4.0;
                float current = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY;

                delay(50);
            }
            
            // Stop motor and IR
            full_stop();
            
            // Play treat sound
            play_treat_audio();
            
            // Keep LED ON for 2 seconds
            delay(2000);
            setPCF8574Pin(4, true); // LED OFF
            
            // Update counter
            schedule_treats_dispensed++;
            
            // Update UI immediately after dispensing
            update_schedule_3_ui();
            
            // Move to next treat
            current_treat_index++;
            
            Serial.printf("First treat dispensed! Total: %d\n", schedule_treats_dispensed);
        } else {
            // Subsequent treats follow training state machine
            Serial.printf("Treat %d - starting training sequence\n", current_treat_index + 1);
            schedule_waiting_for_button = true;
            schedule_train_state = 0; // Start with LED ON for 5s
            schedule_state_start_time = millis();
            schedule_led_blink_state = false;
            schedule_last_blink = millis();
        }
    }
    
    void schedule_timer_tick(lv_timer_t * timer) {
        if (!schedule_is_running || schedule_is_paused) {
            return;
        }
        
        unsigned long now = millis();
        
        // Check for stop request at any time
        if (schedule_stop_requested) {
            schedule_is_running = false;
            schedule_waiting_for_button = false;
            schedule_stop_requested = false; // Reset the flag
            full_stop();
            if (schedule_timer != NULL) {
                lv_timer_del((lv_timer_t*)schedule_timer);
                schedule_timer = NULL;
            }
            Serial.println("=== Schedule STOPPED by user ===");
            return;
        }
        
        // Handle training state machine for subsequent treats
        if (schedule_waiting_for_button) {
            bool start_pressed = button_edge_pressed();
            
            switch (schedule_train_state) {
                case 0: // LED ON for 5s
                    led_set_solid(true);
                    // Play audio once when first entering this state
                    static bool schedule_audio_played_for_case0 = false;
                    if (!schedule_audio_played_for_case0) {
                        play_treat_audio();
                        schedule_audio_played_for_case0 = true;
                    }
                    
                    if (start_pressed) {
                        schedule_train_state = 10; // Skip to motor start
                        schedule_state_start_time = now;
                        schedule_audio_played_for_case0 = false; // Reset for next training cycle
                        Serial.println("Button pressed - starting treat dispense");
                        break;
                    }
                    if (now - schedule_state_start_time > 5000) {
                        schedule_train_state = 1;
                        schedule_state_start_time = now;
                        schedule_audio_played_for_case0 = false; // Reset for next state
                        Serial.println("Moving to LED OFF state");
                    }
                    break;
                
                case 1: // LED OFF for 2s
                    led_set_solid(false);
                    if (start_pressed) {
                        schedule_train_state = 10;
                        schedule_state_start_time = now;
                        Serial.println("Button pressed - starting treat dispense");
                        break;
                    }
                    if (now - schedule_state_start_time > 2000) {
                        schedule_train_state = 2;
                        schedule_state_start_time = now;
                        Serial.println("Moving to second LED ON state");
                    }
                    break;
                
                case 2: // LED ON for 5s
                    led_set_solid(true);
                    // Play audio once when first entering this state
                    static bool schedule_audio_played_for_case2 = false;
                    if (!schedule_audio_played_for_case2) {
                        play_treat_audio();
                        schedule_audio_played_for_case2 = true;
                    }
                    
                    if (start_pressed) {
                        schedule_train_state = 10;
                        schedule_state_start_time = now;
                        schedule_audio_played_for_case2 = false; // Reset for next training cycle
                        Serial.println("Button pressed - starting treat dispense");
                        break;
                    }
                    if (now - schedule_state_start_time > 5000) {
                        schedule_train_state = 3;
                        schedule_state_start_time = now;
                        schedule_last_blink = now;
                        led_blink_mode = true;
                        schedule_led_blink_state = false;
                        schedule_audio_played_for_case2 = false; // Reset for next state
                        Serial.println("Moving to blink state");
                    }
                    break;
                
                case 3: // Blink LED for 5s
                    // Play audio once when first entering blink state
                    static bool schedule_audio_played_for_blink = false;
                    if (!schedule_audio_played_for_blink) {
                        play_treat_audio();
                        schedule_audio_played_for_blink = true;
                    }
                    
                    if (start_pressed) {
                        schedule_train_state = 10;
                        schedule_state_start_time = now;
                        led_blink_mode = false;
                        schedule_audio_played_for_blink = false; // Reset for next training cycle
                        Serial.println("Button pressed - starting treat dispense");
                        break;
                    }
                    if (now - schedule_state_start_time > 5000) {
                        // Timeout - skip this treat and continue schedule
                        schedule_waiting_for_button = false;
                        led_blink_mode = false;
                        led_set_solid(false);
                        schedule_audio_played_for_blink = false; // Reset for next training cycle
                        current_treat_index++; // Skip this treat
                        Serial.println("Button timeout - skipping treat and continuing schedule");
                    } else {
                        // Handle blinking
                        if (now - schedule_last_blink >= 250) {
                            schedule_last_blink = now;
                            schedule_led_blink_state = !schedule_led_blink_state;
                            led_apply(schedule_led_blink_state);
                        }
                    }
                    break;
                
                case 10: // Actually dispense the treat
                    led_blink_mode = false;
                    led_set_solid(false);
                    
                    // Dispense treat using same logic as first treat
                    Serial.println("Dispensing scheduled treat");
                    setPCF8574Pin(4, false); // LED ON
                    Motor_Start();
                    IR_Start();
                    
                    const unsigned long MOTOR_TIMEOUT = 5000;
                    unsigned long startTime = millis();
                    
                    // Run motor for full 5 seconds with jam detection
                    while ((millis() - startTime < MOTOR_TIMEOUT)) {
                        int adcValue = analogRead(CURRENT_SENSOR_PIN);
                        float voltage = (adcValue / 4095.0) * 4.0;
                        float current = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY;
                        

                        delay(50);
                    }
                    
                    full_stop();
                    play_treat_audio();
                    delay(2000);
                    setPCF8574Pin(4, true); // LED OFF
                    
                    // Update counter and UI
                    schedule_treats_dispensed++;
                    update_schedule_3_ui();
                    current_treat_index++;
                    
                    // Reset state machine
                    schedule_waiting_for_button = false;
                    schedule_train_state = 0;
                    
                    Serial.printf("Scheduled treat dispensed! Total: %d\n", schedule_treats_dispensed);
                    break;
            }
            return; // Don't continue with normal timer logic while in training mode
        }
        
        // Normal schedule timing logic
        unsigned long elapsed_total = now - schedule_start_time;
        unsigned long elapsed_minutes = elapsed_total / 60000; // Convert to minutes
        
        // Update remaining time
        int total_minutes = selected_hours_to_dispense * 60;
        schedule_remaining_minutes = total_minutes - elapsed_minutes;
        
        // Update UI with live countdown
        update_schedule_3_ui();
        
        // Debug output every 10 seconds
        static unsigned long last_debug = 0;
        if (now - last_debug >= 10000) {
            Serial.printf("Schedule Debug: elapsed_total=%lu, elapsed_minutes=%lu, remaining=%d\n", 
                         elapsed_total, elapsed_minutes, schedule_remaining_minutes);
            last_debug = now;
        }
        
        // Check if schedule is complete
        if (schedule_remaining_minutes <= 0 || current_treat_index >= total_scheduled_treats) {
            schedule_is_running = false;
            schedule_remaining_minutes = 0;
            full_stop();
            if (schedule_timer != NULL) {
                lv_timer_del((lv_timer_t*)schedule_timer);
                schedule_timer = NULL;
            }
            Serial.println("=== Schedule Complete ===");
            return;
        }
        
        // Check if it's time for next treat
        if (current_treat_index < total_scheduled_treats) {
            int next_treat_time = scheduled_times[current_treat_index];
            
            Serial.printf("Checking treat %d: current_time=%lu min, scheduled=%d min\n", 
                         current_treat_index + 1, elapsed_minutes, next_treat_time);
            
            // Only dispense if we've reached the scheduled time and haven't just dispensed
            if (elapsed_minutes >= next_treat_time) {
                // Ensure we don't dispense too rapidly (minimum 30 seconds between treats)
                static unsigned long last_dispense_time = 0;
                if (now - last_dispense_time >= 30000) { // 30 second minimum interval
                    Serial.printf("Time for treat %d at minute %lu (scheduled: %d)\n", 
                                 current_treat_index + 1, elapsed_minutes, next_treat_time);
                    schedule_dispense_treat();
                    last_dispense_time = now;
                }
            }
        }
    }
    
    void init_audio() {
        // Initialize DAC audio properly - use GPIO 26 for ESP32-2432S028R
        // GPIO 25 is used for touchscreen on this board
        dac_output_enable(DAC_CHANNEL_2); // GPIO 26 = DAC_CHANNEL_2
        dac_output_voltage(DAC_CHANNEL_2, 0); // Start with silence
        Serial.println("DAC audio initialized on GPIO 26 (DAC_CHANNEL_2)");
    }

    void play_treat_audio() {
        Serial.println("Playing simple bell audio");
        
        // Simple, short bell sound - using DAC_CHANNEL_2 (GPIO 26)
        // Quick bell ding
        for (int i = 0; i < 100; i++) {
            dac_output_voltage(DAC_CHANNEL_2, 200);
            delayMicroseconds(500); // 1kHz tone
            dac_output_voltage(DAC_CHANNEL_2, 0);
            delayMicroseconds(500);
        }
        
        // Brief pause
        delay(50);
        
        // Short fade out
        for (int vol = 150; vol > 0; vol -= 30) {
            for (int i = 0; i < 20; i++) {
                dac_output_voltage(DAC_CHANNEL_2, vol);
                delayMicroseconds(500);
                dac_output_voltage(DAC_CHANNEL_2, 0);
                delayMicroseconds(500);
            }
        }
        
        dac_output_voltage(DAC_CHANNEL_2, 0); // Ensure silence
        Serial.println("Simple bell audio complete");
    }
    //********* Stop EVERYTHING  */
    void full_stop() {
        // Stop Motor
        setPCF8574Pin(0, false); // IN1 HIGH
        setPCF8574Pin(1, false); // IN2 HIGH

        // Turn off LED and IR transmitter
        setPCF8574Pin(4, true); // LED OFF
        setPCF8574Pin(5, true); // IR transmitter OFF

        Serial.println("Full stop: Motor and LED OFF");
    }

    //********* Start Motor EVERYTHING  */
    void Motor_Start() {
        // Start Motor Clockwise (IN1 LOW, IN2 HIGH)
        setPCF8574Pin(0, true);
        setPCF8574Pin(1, false);
        Serial.println("Motor ON (CW)");
    }
// Start IR code
// **************************************
static bool beam_initial_state = true;   // P6 initial reading after IR starts

// Debounce state for P3 (button: HIGH=pressed per your wiring)
static bool btn_prev = false;               // last raw level (start at not-pressed)
static unsigned long btn_last_change = 0;   // last change time

static inline bool read_beam_P6() {
    return readPCF8574Pin(6); // true=HIGH, false=LOW
}

// Add just after button_pressed_debounced (or anywhere before train_dispense_tick)

    void IR_Start() {
    // Turn ON status LED and IR transmitter (active-low)
    setPCF8574Pin(4, false); // LED ON
    setPCF8574Pin(5, false); // IR TX ON

    // Let IR RX settle and capture baseline
    delay(150);
    beam_initial_state = read_beam_P6();
    Serial.println("IR Start - checking initial beam state:");
    Serial.print("Initial IR beam state (P6): ");
    Serial.println(beam_initial_state ? "HIGH" : "LOW");
}

    //********* Stop EVERYTHING  */
    void IR_Stop() {
        // Stop Motor
        // Turn on LED and IR transmitter
        setPCF8574Pin(4, true); // LED OFF
        setPCF8574Pin(5, true); // IR transmitter OFF
        Serial.println("IR transmitter and reciever OFF");
    }

    // Training model code
    //************************************************** */
    void train_dispense_tick(lv_timer_t * timer) {
        
        if (train_dispense_stop_requested) {
            full_stop();
            led_set_solid(false); // LED OFF
            lv_timer_del(timer);
            train_dispense_state = 0;
            Serial.println("=== Train Dispense STOPPED ===");
            return;
        }

    unsigned long now = millis();
    bool start_pressed = button_pressed_debounced(now);   // kept for visibility/logging
    bool edge_pressed  = button_edge_pressed();           // NEW: immediate trigger

    // Live port / P3 bit dump
    logPCFPortP3();

    Serial.print("Button state: ");
    Serial.println(readPCF8574Pin(PIN_BUTTON) ? "HIGH" : "LOW");
    Serial.print("Debounced pressed: ");
    Serial.println(start_pressed ? "YES" : "NO");
    Serial.print("Edge pressed: ");
    Serial.println(edge_pressed ? "YES" : "NO");

    switch (train_dispense_state) {
    case 0: // LED ON for 5s
        led_set_solid(true);
        // Play audio once when first entering this state
        static bool audio_played_for_case0 = false;
        if (!audio_played_for_case0) {
            play_treat_audio();
            audio_played_for_case0 = true;
        }
        
        if (edge_pressed) {
            train_dispense_state = 10;
            state_start_time = now;
            audio_played_for_case0 = false; // Reset for next training cycle
            Serial.println("EDGE -> start dispense (from case 0)");
            break;
        }
        if (now - state_start_time > 5000) {
            train_dispense_state = 1;
            state_start_time = now;
            audio_played_for_case0 = false; // Reset for next state
            Serial.println("Moving to LED OFF state");
        }
        break;

    case 1: // LED OFF for 2s
        led_set_solid(false);
        if (edge_pressed) {
            train_dispense_state = 10;
            state_start_time = now;
            Serial.println("EDGE -> start dispense (from case 1)");
            break;
        }
        if (now - state_start_time > 2000) {
            train_dispense_state = 2;
            state_start_time = now;
            Serial.println("Moving to second LED ON state");
        }
        break;

    case 2: // LED ON for 5s
        led_set_solid(true);
        // Play audio once when first entering this state
        static bool audio_played_for_case2 = false;
        if (!audio_played_for_case2) {
            play_treat_audio();
            audio_played_for_case2 = true;
        }
        
        if (edge_pressed) {
            train_dispense_state = 10;
            state_start_time = now;
            audio_played_for_case2 = false; // Reset for next training cycle
            Serial.println("EDGE -> start dispense (from case 2)");
            break;
        }
        if (now - state_start_time > 5000) {
            train_dispense_state = 3;
            state_start_time = now;
            last_blink = now;
            led_blink_mode = true;
            led_blink_state = false;
            audio_played_for_case2 = false; // Reset for next state
            Serial.println("Moving to blink state");
        }
        break;

    case 3: // Blink LED for 5s
        // Play audio once when first entering blink state
        static bool audio_played_for_blink = false;
        if (!audio_played_for_blink) {
            play_treat_audio();
            audio_played_for_blink = true;
        }
        
        if (edge_pressed) {
            train_dispense_state = 10;
            state_start_time = now;
            led_blink_mode = false;
            audio_played_for_blink = false; // Reset for next training cycle
            Serial.println("EDGE -> start dispense (from blink state 3)");
            break;
        }
        if (now - state_start_time > 5000) {
            train_dispense_state = 99;
            led_blink_mode = false;
            led_set_solid(false);
            audio_played_for_blink = false; // Reset for next training cycle
            Serial.println("Blink timeout - ending sequence (no button press) - case 3");
        } else {
            led_blink_tick(now);
        }
        break;

    case 10: // Start motor
        led_blink_mode = false;
        led_set_solid(false); // Ensure LED is OFF when starting motor
        setPCF8574Pin(4, true); // LED OFF
        Motor_Start();
        train_dispense_state = 11;
        state_start_time = now;

        Serial.println("Motor started (fixed 5s run) - case 10");
        break;

    case 11: { // Motor running
        unsigned long elapsed = now - state_start_time;
        int adcValue = analogRead(CURRENT_SENSOR_PIN);
        float voltage = (adcValue / 4095.0f) * 4.0f;
        float current = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY;

        break;
    }

    case 99: // Done
        full_stop();
        led_set_solid(false);
        lv_timer_del(timer);
        train_dispense_state = 0;
        Serial.println("=== Train Dispense Complete ===");
        break;
    }
}
    //************************************************** */
    // Main Code for Treat Dispense Actions
    //************************************************** */
    // Manual Treat Dispense Action
    void action_manual_dispense_treat(lv_event_t * e) {
        Serial.println("\n=== Manual Treat Dispense Started ===");
        // Stop motor and IR
        //full_stop();
        // Play treat sound immediately when button is pressed
        play_treat_audio();
        
        setPCF8574Pin(4, false); // LED ON
        delay(200);
        Motor_Start();
        IR_Start();
        const unsigned long MOTOR_TIMEOUT = 5000; // 5 seconds

        bool treatDispensed = false;
        
        unsigned long startTime = millis();

        // Reset attempt-scoped state (do this every Manual Treat attempt)
        treatDispensed = false;
        lhTransitions = 0;
        stopRequested_NoTreat = false;
        waitForNextHigh_AfterTreat = false;
        seenLowAfterTreat = false;

        // Seed edge detector to current rotary state to avoid a fake edge on first iteration
        lastRotary = readPCF8574Pin(2);

        while ((millis() - startTime) < MOTOR_TIMEOUT) {

            bool rawValue     = readPCF8574Pin(6); // P6 IR Receiver (HIGH=intact, LOW=broken)
            bool rotarySwitch = readPCF8574Pin(2); // P2 Rotary Switch (HIGH=safe-to-stop / blocking position)
            bool beamBroken = !rawValue;

            // --- 1) Treat detection ---
            if (!treatDispensed && beamBroken) {
            treatDispensed = true;

            // Reset transition logic on dispense
            lhTransitions = 0;
            stopRequested_NoTreat = false;

            // Now stop at NEXT HIGH
            waitForNextHigh_AfterTreat = true;

            // If currently HIGH, we must see LOW then HIGH to count as "next HIGH"
            seenLowAfterTreat = !rotarySwitch;

            Serial.println("Beam broken! Treat dispensed.");
            }

            // --- 2) Rotary LOW->HIGH transition counting (ONLY if no treat yet) ---
            if (!treatDispensed) {
                if (!lastRotary && rotarySwitch) {          // LOW -> HIGH edge
                lhTransitions++;
                Serial.print("Rotary LOW->HIGH transitions: ");
                Serial.println(lhTransitions);

                if (lhTransitions >= 3) {
                    stopRequested_NoTreat = true;           // stop at HIGH (now or next time it becomes HIGH)
                }
                }
            }

            // --- 3) Stop conditions ---
            // A) Treat dispensed: stop at the NEXT high position
            if (waitForNextHigh_AfterTreat) {
                if (!seenLowAfterTreat) {
                // We are waiting to see LOW at least once after dispense
                if (!rotarySwitch) seenLowAfterTreat = true;
                } else {
                // Once we've seen LOW, the next HIGH is our stop point
                if (rotarySwitch) {
                    Serial.println("Stopping at NEXT HIGH after treat dispense.");
                    break; // exit loop -> stop motor
                }
                }
            }

            // B) No treat after 3 transitions: stop when rotary is HIGH
            if (stopRequested_NoTreat && rotarySwitch) {
                Serial.println("No treat after 3 LOW->HIGH transitions. Stopping at HIGH.");
                break; // exit loop -> stop motor
            }

            // --- 4) Optional: current monitoring (your existing code) ---
            int adcValue = analogRead(CURRENT_SENSOR_PIN);
            float voltage = (adcValue / 4095.0) * 4.0;
            float current = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY;

            // Save state for edge detection next iteration
            lastRotary = rotarySwitch;
        }

        // Stop motor and IR system
        full_stop();

        // Keep LED ON for 2 seconds after stopping
        delay(2000);
        setPCF8574Pin(4, true); // LED OFF
        Serial.println("LED OFF after 2 seconds");

        Serial.println("=== Manual Treat Dispense Complete ===\n");
    }
    //************************************************** */
    void action_train_dispense_treat(lv_event_t * e) {
        // Stop motor and IR
        full_stop();
        // Initialize PCF8574 to ensure proper button detection
        initPCF8574Pins();
        delay(100); // Allow hardware to settle
        
        Serial.println("=== Train Dispense STARTED ===");
        
        train_dispense_stop_requested = false;
        train_dispense_state = 0;
        state_start_time = millis();   
        lv_timer_create(train_dispense_tick, 50, NULL); // Call tick every 50ms
    }
    // called when the “Train Start” button is pressed
    void action_train_dispense_stop(lv_event_t * e) {
        // TODO: STOP all your training‐mode dispense sequence

        Serial.println("=== Train Dispense STOPPED ===\n");
        full_stop();
        Serial.println("=== Train Dispense STOP Complete ===\n");

    }
    //************************************************** */
    // called when the “Next” button in Schedule #1 is pressed
    void action_schedule_add_treat_num(lv_event_t * e) {

        // Get the roller object from your objects struct


        // Get the selected index (0-based)
        int idx = lv_roller_get_selected(objects.schedule_1_treatsnumber);

        // If your roller options are "1\n2\n3\n..." then add 1 to idx to get the actual value
        selected_treats_number = idx + 1;

        Serial.print("Treats to dispense selected: ");
        Serial.println(selected_treats_number);
        
        // Force immediate UI update for schedule screen 3
        update_schedule_3_ui();

    }
    //************************************************** */
    // called when the “Hours to Dispense” control in Schedule #2 changes
    void action_schedule_add_hours(lv_event_t * e) {

        // Get the selected index (0-based)
        int idx = lv_roller_get_selected(objects.schedule_2_hours_to_dispense);

        // If your roller options are "1\n2\n3\n..." then add 1 to idx to get the actual value
        selected_hours_to_dispense = idx + 1;

        Serial.print("Hours to dispense selected: ");
        Serial.println(selected_hours_to_dispense);
        
        // Force immediate UI update for schedule screen 3
        update_schedule_3_ui();

        // Now selected_hours_to_dispense can be used on following screens
    }
    
    //************************************************** */
    // called when the "Next" button in Schedule #2 is pressed (transition to Schedule #3)
    void action_schedule_2_next(lv_event_t * e) {
        // Force update the schedule screen 3 values immediately
        // This ensures the UI shows current user selections
        
        Serial.println("Transitioning to Schedule 3 screen");
        Serial.printf("Current values: treats=%d, hours=%d\n", selected_treats_number, selected_hours_to_dispense);
        
        // The screen transition will happen automatically via the EEZ flow
        // The values will be updated by the tick function
    }
    
    //************************************************** */
    // Schedule #3 — Stop, Pause, Start
    void action_scheduletreatdispensestart(lv_event_t * e) {
        Serial.println("=== Schedule Dispense START ===");

        // Stop motor and IR
            full_stop();
        if (!schedule_is_running) {
            // Reset schedule stop flag when starting new schedule
            schedule_stop_requested = false;
            
            // Initialize new schedule
            schedule_treats_dispensed = 0;
            schedule_remaining_minutes = selected_hours_to_dispense * 60;
            current_treat_index = 0;
            
            Serial.printf("Initializing schedule: %d hours = %d minutes\n", 
                         selected_hours_to_dispense, schedule_remaining_minutes);
            
            // Generate the schedule times
            generate_schedule_times();
            
            // Start the schedule timer - do NOT dispense immediately
            schedule_is_running = true;
            schedule_is_paused = false;
            schedule_start_time = millis();
            
            Serial.printf("Schedule start time: %lu\n", schedule_start_time);
            
            // Delete existing timer if it exists
            if (schedule_timer != NULL) {
                lv_timer_del((lv_timer_t*)schedule_timer);
                schedule_timer = NULL;
            }
            
            // Create timer that checks every 100ms for more responsive UI updates
            schedule_timer = lv_timer_create(schedule_timer_tick, 100, NULL);
            
            // Immediately dispense the first treat
            Serial.println("Dispensing first treat immediately");
            schedule_dispense_treat();
            // Note: schedule_dispense_treat() will handle incrementing current_treat_index
            
            Serial.printf("Schedule started: %d treats over %d hours\n", 
                         selected_treats_number, selected_hours_to_dispense);
        } else if (schedule_is_paused) {
            // Resume from pause
            schedule_is_paused = false;
            
            // Adjust start time to account for pause duration
            unsigned long pause_duration = millis() - schedule_pause_time;
            schedule_start_time += pause_duration;
            
            Serial.println("Schedule RESUMED");
        }
    }
    
    void action_scheduletreatdispensepause(lv_event_t * e) {
        if (schedule_is_running && !schedule_is_paused) {
            schedule_is_paused = true;
            schedule_pause_time = millis();
            full_stop(); // Stop any current motor activity
            Serial.println("=== Schedule Dispense PAUSED ===");
        } else if (schedule_is_paused) {
            // Resume if already paused (toggle behavior)
            action_scheduletreatdispensestart(e);
        }
    }
    
    void action_scheduletreatdispensestop(lv_event_t * e) {
        Serial.println("=== Schedule Dispense STOPPED ===");
        
        // Set stop flag to signal the timer to stop
        schedule_stop_requested = true;
        
        schedule_is_running = false;
        schedule_is_paused = false;
        schedule_treats_dispensed = 0;
        schedule_remaining_minutes = 0;
        current_treat_index = 0;
        
        // Reset training state machine
        schedule_waiting_for_button = false;
        schedule_train_state = 0;
        led_blink_mode = false;
        led_set_solid(false);
        
        // Clean up timer
        if (schedule_timer != NULL) {
            lv_timer_del((lv_timer_t*)schedule_timer);
            schedule_timer = NULL;
        }
        
        full_stop();
        
        // Update UI to show initial state (not running)
        update_schedule_3_ui();
        
        Serial.println("=== Schedule Dispense STOP Complete ===");
    }

}  // end extern "C"


