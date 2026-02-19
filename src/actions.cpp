// actions.cpp (clean, compile-safe version)
//
// Updates included:
// 1) Manual treat mode sequence:
//      - Beep 1s + LED ON (P4 active-low)
//      - LED stays ON
//      - Wait 5s
//      - Beep 1s
//      - Dispense treat (motor + IR + stop logic)
//      - full_stop() at end
//
// 2) Foot-switch training (standalone / “anytime”):
//      - 20s window
//      - LED solid ON (P4)
//      - 1s tone every 5s
//      - Wait for foot switch on P3 (active-low)
//      - If pressed within window -> dispense ONE treat
//      - If timeout -> no treat
//      - Hardened against motor auto-start by forcing full_stop AFTER init + motor-off guard
//
// 3) Scheduled treat mode behavior:
//      - Treat #1 follows the manual treat sequence (beep+LED, wait 5s, beep, dispense)
//      - Treat #2..N require foot-switch activation (20s window). If not pressed -> skip treat.
//
// 4) NEW: IR remote trigger on PCF P7 (active-low):
//      - Debounced edge on P7 starts the standalone foot-switch training window at ANY time
//      - Implemented via a small LVGL timer polling P7.
//

#include <Arduino.h>
#include <stdlib.h>
#include "ui.h"
#include "vars.h"
#include "actions.h"
#include <SD.h>
#include <FS.h>
#include "driver/dac.h"
#include "pcf8574_control.h"
#include <Wire.h>
#include <IRremoteESP8266.h>
#include "audio_utils.h"

// ---- Restore / fallback definitions (only used if not already provided elsewhere) ----
#ifndef AUDIO_DAC_CHANNEL
#define AUDIO_DAC_CHANNEL DAC_CHANNEL_1
#endif

#ifndef WAV_FILE
#define WAV_FILE "/treat.wav"
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
#define TRAIN_MOTOR_RUN_MS 5000UL
#endif
// ---- End fallback definitions ----

// ---------------------------
// Serial rate limiting helpers
// ---------------------------
static inline bool every_30s(unsigned long now_ms) {
    static unsigned long last = 0;
    if (now_ms - last >= 30000UL) { last = now_ms; return true; }
    return false;
}

// ---------------------------
// State variables (button training)
// ---------------------------
volatile bool train_dispense_stop_requested = false;
static int  train_dispense_state = 0;
static unsigned long state_start_time = 0;

// LED blink state (shared)
static bool led_blink_state = false;
static unsigned long last_blink = 0;

// ---------------------------
// Foot-switch training (standalone action)
// ---------------------------
static lv_timer_t* foot_train_timer = NULL;
static bool foot_train_active = false;
static unsigned long foot_train_start_ms = 0;
static unsigned long foot_train_last_tone_ms = 0;

// Foot switch is on PCF P3 (pressed = LOW)
static inline bool footswitch_pressed_debounced(unsigned long now_ms) {
    bool raw = !readPCF8574Pin(3); // active-low => pressed when LOW
    static bool prev = false;
    static unsigned long t = 0;
    if (raw != prev) { prev = raw; t = now_ms; }
    return raw && (now_ms - t > 30);
}

// ---------------------------
// Schedule state variables
// ---------------------------
static bool schedule_waiting_for_footswitch = false;
static unsigned long schedule_wait_start_ms = 0;
static unsigned long schedule_last_tone_ms = 0;

volatile bool schedule_stop_requested = false;

int selected_treats_number = 1;
int selected_hours_to_dispense = 2;

int schedule_treats_dispensed = 0;
int schedule_remaining_minutes = 0;
bool schedule_is_running = false;
bool schedule_is_paused = false;

void* schedule_timer = NULL;
static unsigned long schedule_start_time = 0;
static unsigned long schedule_pause_time = 0;
static int schedule_last_displayed_minutes = -9999;

// Schedule timing arrays
static int scheduled_times[96]; // Max 8 hours * 12 treats = 96 treats
static int total_scheduled_treats = 0;
static int current_treat_index = 0;

// LED control
static bool led_blink_mode = false;
static bool led_is_on_solid = false;

// ---------------------------
// Treat detection / rotary logic globals
// ---------------------------
bool treatDispensed = false;
bool lastRotary = false;
int  lhTransitions = 0;

bool stopRequested_NoTreat = false;
bool waitForNextHigh_AfterTreat = false;
bool seenLowAfterTreat = false;

// ---------------------------------------------
// Shared motor run logic (IR + Rotary Switch)
// ---------------------------------------------
enum MotorStopReason {
    STOP_TIMEOUT = 0,
    STOP_TREAT_NEXT_HIGH,
    STOP_NO_TREAT_3_TRANSITIONS,
    STOP_JAM,
    STOP_EXTERNAL_REQUEST
};

static inline void reset_treat_logic_state_for_run() {
    treatDispensed = false;
    lhTransitions = 0;
    stopRequested_NoTreat = false;
    waitForNextHigh_AfterTreat = false;
    seenLowAfterTreat = false;

    // Seed edge detector to current rotary state to avoid a fake edge
    lastRotary = readPCF8574Pin(2);
}

static MotorStopReason run_motor_with_treat_logic(unsigned long timeout_ms,
                                                  volatile bool* external_stop_flag = nullptr) {
    const unsigned long startTime = millis();

    reset_treat_logic_state_for_run();

    while ((millis() - startTime) < timeout_ms) {

        if (external_stop_flag && *external_stop_flag) {
            Serial.println("External stop flag set -> abort motor run.");
            return STOP_EXTERNAL_REQUEST;
        }

        bool rawValue     = readPCF8574Pin(6); // P6 IR Receiver (HIGH=intact, LOW=broken)
        bool rotarySwitch = readPCF8574Pin(2); // P2 Rotary Switch (HIGH=safe-to-stop / blocking position)
        bool beamBroken   = !rawValue;

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
            if (!lastRotary && rotarySwitch) { // LOW -> HIGH edge
                lhTransitions++;
                Serial.print("Rotary LOW->HIGH transitions: ");
                Serial.println(lhTransitions);

                if (lhTransitions >= 3) {
                    stopRequested_NoTreat = true;
                }
            }
        }

        // --- 3) Stop conditions ---
        // A) Treat dispensed: stop at the NEXT high position
        if (waitForNextHigh_AfterTreat) {
            if (!seenLowAfterTreat) {
                if (!rotarySwitch) seenLowAfterTreat = true;
            } else {
                if (rotarySwitch) {
                    Serial.println("Stopping at NEXT HIGH after treat dispense.");
                    return STOP_TREAT_NEXT_HIGH;
                }
            }
        }

        // B) No treat after 3 transitions: stop when rotary is HIGH
        if (stopRequested_NoTreat && rotarySwitch) {
            Serial.println("No treat after 3 LOW->HIGH transitions. Stopping at HIGH.");
            return STOP_NO_TREAT_3_TRANSITIONS;
        }

        // --- 4) Optional jam detection (disabled by default) ---
        int adcValue = analogRead(CURRENT_SENSOR_PIN);
        float voltage = (adcValue / 4095.0f) * 4.0f;
        float current = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY;
        (void)current;

        // if (current > JAM_THRESHOLD_AMPS) {
        //     Serial.printf("JAM detected! current=%.2fA\n", current);
        //     return STOP_JAM;
        // }

        lastRotary = rotarySwitch;
        delay(5);
    }

    Serial.println("Motor timeout reached.");
    return STOP_TIMEOUT;
}

// ---------------------------
// LED helpers (active-low)
// ---------------------------
static inline void led_apply(bool on) {
    // on=true => drive low (active-low LED ON)
    setPCF8574Pin(PIN_LED, !on);
}

static inline void led_set_solid(bool on) {
    led_blink_mode = false;
    led_is_on_solid = on;
    led_apply(on);
}

static inline void led_blink_tick(unsigned long now) {
    if (!led_blink_mode) return;
    if (now - last_blink >= 250) {
        last_blink = now;
        led_blink_state = !led_blink_state;
        led_apply(led_blink_state);
    }
}

extern "C" {

// Forward declarations
void init_audio();
void full_stop();
void update_schedule_3_ui();

void Motor_Start();
void IR_Start();
void IR_Stop();

// ---------------------------
// Button (PIN_BUTTON) helpers
// ---------------------------
static inline bool button_pressed_debounced(unsigned long now_ms) {
    bool raw = !readPCF8574Pin(PIN_BUTTON); // pressed = LOW
    static bool prev = false;
    static unsigned long t = 0;
    if (raw != prev) { prev = raw; t = now_ms; }
    return raw && (now_ms - t > 30);
}

static inline bool button_edge_pressed() {
    bool raw = !readPCF8574Pin(PIN_BUTTON);
    static bool last = false;
    bool edge = raw && !last;
    last = raw;
    return edge;
}

// =====================================================
// IR Remote (P7 active-low) -> start foot-switch training ANYTIME
// =====================================================
static lv_timer_t* remote_poll_timer = NULL;

static inline bool remote_p7_edge_pressed(unsigned long now_ms) {
    bool raw = !readPCF8574Pin(7); // active-low => pressed when LOW
    static bool last_raw = false;
    static bool last_stable = false;
    static unsigned long t_change = 0;

    if (raw != last_raw) {
        last_raw = raw;
        t_change = now_ms;
    }

    if ((now_ms - t_change) > 30) {
        bool prev = last_stable;
        last_stable = raw;
        return (last_stable && !prev); // edge into pressed state
    }
    return false;
}

static void footswitch_train_tick(lv_timer_t* timer); // forward

static void start_footswitch_training_window() {
    // Don’t restart if already running
    if (foot_train_active || foot_train_timer) return;

    Serial.println("Foot-switch training window STARTED (remote/UI)");

    // IMPORTANT: init can glitch outputs -> init FIRST, then full_stop AFTER
    initPCF8574Pins();
    delay(50);

    // Force known-safe outputs AFTER init (prevents motor auto-start)
    full_stop();

    // Ensure P3 + P7 behave as inputs (PCF quasi-bidirectional): write HIGH to read them
    setPCF8574Pin(3, false);
    setPCF8574Pin(7, false);

    foot_train_active = true;
    foot_train_start_ms = millis();
    foot_train_last_tone_ms = foot_train_start_ms - 5000UL; // immediate tone

    // LED ON (P4 active-low)
    setPCF8574Pin(4, false);

    // Tick fast enough to catch switch presses + timing
    foot_train_timer = lv_timer_create(footswitch_train_tick, 50, NULL);
}

static void remote_poll_tick(lv_timer_t* t) {
    (void)t;
    unsigned long now = millis();
    if (remote_p7_edge_pressed(now)) {
        Serial.println("IR Remote (P7) pressed -> start training");
        start_footswitch_training_window();
    }
}

static void ensure_remote_poll_timer_running() {
    if (!remote_poll_timer) {
        // Make sure P7 is released high so reads work reliably
        setPCF8574Pin(7, false);
        remote_poll_timer = lv_timer_create(remote_poll_tick, 25, NULL);
        Serial.println("Remote poll timer started (P7 training trigger)");
    }
}

// ---------------------------
// UI updater
// ---------------------------
void update_schedule_3_ui() {
    // Keep remote trigger alive (safe to call repeatedly)
    ensure_remote_poll_timer_running();

    // Treats per hour
    if (objects.treats_per_hour) {
        char treats_str[10];
        snprintf(treats_str, sizeof(treats_str), "%d", selected_treats_number);
        lv_label_set_text(objects.treats_per_hour, treats_str);
    }

    // Treats dispensed
    if (objects.treats_dispensed) {
        char dispensed_str[10];
        snprintf(dispensed_str, sizeof(dispensed_str), "%d", schedule_treats_dispensed);
        lv_label_set_text(objects.treats_dispensed, dispensed_str);
    }

    // Time left HH:MM
    if (objects.schedule_time_left) {
        char time_str[10];
        if (schedule_is_running) {
            int hours = schedule_remaining_minutes / 60;
            int minutes = schedule_remaining_minutes % 60;
            snprintf(time_str, sizeof(time_str), "%d:%02d", hours, minutes);
        } else {
            snprintf(time_str, sizeof(time_str), "%d:00", selected_hours_to_dispense);
        }
        lv_label_set_text(objects.schedule_time_left, time_str);
    }

    unsigned long now = millis();
    if (every_30s(now)) {
        Serial.printf("Updated Schedule UI: treats/hr=%d, dispensed=%d, hours=%d, remaining_min=%d\n",
                      selected_treats_number, schedule_treats_dispensed, selected_hours_to_dispense, schedule_remaining_minutes);
    }
}

// ---------------------------
// Schedule generation
// ---------------------------
void generate_schedule_times() {
    total_scheduled_treats = 0;
    current_treat_index = 0;

    int treats_per_hour = selected_treats_number;
    int total_hours     = selected_hours_to_dispense;

    // Enforce minimum 2
    if (treats_per_hour < 2) treats_per_hour = 2;

    int reliable_per_hour = (treats_per_hour + 1) / 2; // ceil(N/2)
    int random_per_hour   = treats_per_hour / 2;       // floor(N/2)

    int spacing = 60 / reliable_per_hour;

    for (int hour = 0; hour < total_hours; hour++) {
        int hour_offset = hour * 60;

        // Reliable: 0, spacing, 2*spacing...
        for (int k = 0; k < reliable_per_hour; k++) {
            int t = k * spacing;
            if (t >= 60) t = 59;
            scheduled_times[total_scheduled_treats++] = hour_offset + t;
        }

        // Random: anywhere 0..59, avoid exact duplicates inside the hour
        for (int r = 0; r < random_per_hour; r++) {
            bool valid = false;
            int attempts = 0;
            int t = 0;

            while (!valid && attempts < 100) {
                t = random() % 60;

                valid = true;
                for (int j = 0; j < total_scheduled_treats; j++) {
                    int existing = scheduled_times[j];
                    if (existing >= hour_offset && existing < hour_offset + 60) {
                        int existing_min = existing - hour_offset;
                        if (abs(existing_min - t) < 1) {
                            valid = false;
                            break;
                        }
                    }
                }
                attempts++;
            }

            if (valid) {
                scheduled_times[total_scheduled_treats++] = hour_offset + t;
            } else {
                scheduled_times[total_scheduled_treats++] = hour_offset + (59 - r);
            }
        }
    }

    // Sort
    for (int i = 0; i < total_scheduled_treats - 1; i++) {
        for (int j = i + 1; j < total_scheduled_treats; j++) {
            if (scheduled_times[i] > scheduled_times[j]) {
                int tmp = scheduled_times[i];
                scheduled_times[i] = scheduled_times[j];
                scheduled_times[j] = tmp;
            }
        }
    }

    Serial.println("=== Generated Schedule ===");
    for (int i = 0; i < total_scheduled_treats; i++) {
        int h = scheduled_times[i] / 60;
        int m = scheduled_times[i] % 60;
        Serial.printf("Treat %d: %d:%02d\n", i + 1, h, m);
    }
}

// =====================================================
// Scheduled treat helpers
// =====================================================

// Treat #1: manual sequence, immediate
static bool schedule_dispense_manual_sequence_now(volatile bool* stop_flag) {
    Serial.println("Schedule treat #1: manual-sequence dispense");

    // Beep + LED ON
    audio_play_tone_1s();
    setPCF8574Pin(4, false); // LED ON

    // Wait 5 seconds (LED stays ON)
    delay(5000);

    // Beep again
    audio_play_tone_1s();

    // Dispense
    Motor_Start();
    IR_Start();

    const unsigned long MOTOR_TIMEOUT = TRAIN_MOTOR_RUN_MS;
    MotorStopReason reason = run_motor_with_treat_logic(MOTOR_TIMEOUT, stop_flag);

    full_stop();

    Serial.print("Schedule #1 stop reason: ");
    Serial.println((int)reason);

    if (reason == STOP_EXTERNAL_REQUEST) {
        Serial.println("Schedule stopped by user during treat #1; not incrementing counters.");
        return false;
    }

    schedule_treats_dispensed++;
    update_schedule_3_ui();
    current_treat_index++;
    return true;
}

// Treat #2..N: foot-switch pressed -> dispense NOW (no 5s delay)
static bool schedule_dispense_now_on_footswitch(volatile bool* stop_flag) {
    Serial.println("Schedule: foot-switch dispense NOW");

    full_stop();

    setPCF8574Pin(4, false); // LED ON
    Motor_Start();
    IR_Start();

    const unsigned long MOTOR_TIMEOUT = TRAIN_MOTOR_RUN_MS;
    MotorStopReason reason = run_motor_with_treat_logic(MOTOR_TIMEOUT, stop_flag);

    full_stop();

    Serial.print("Schedule foot-switch stop reason: ");
    Serial.println((int)reason);

    if (reason == STOP_EXTERNAL_REQUEST) {
        Serial.println("Schedule stopped by user during foot-switch dispense; not incrementing counters.");
        return false;
    }

    schedule_treats_dispensed++;
    update_schedule_3_ui();
    current_treat_index++;
    return true;
}

// Entry point called when schedule time is reached
void schedule_dispense_treat() {
    Serial.println("=== Schedule Treat Trigger ===");

    if (current_treat_index == 0) {
        // Treat #1: manual sequence immediately
        (void)schedule_dispense_manual_sequence_now(&schedule_stop_requested);
    } else {
        // Treat #2..N: open 20s foot-switch window
        schedule_waiting_for_footswitch = true;
        schedule_wait_start_ms = millis();
        schedule_last_tone_ms  = schedule_wait_start_ms - 5000UL; // immediate tone
        Serial.printf("Schedule treat %d: waiting for FOOT SWITCH (20s)\n", current_treat_index + 1);
    }
}

// ---------------------------
// Schedule timer tick
// ---------------------------
void schedule_timer_tick(lv_timer_t * timer) {
    (void)timer;

    // Keep remote trigger alive (safe to call repeatedly)
    ensure_remote_poll_timer_running();

    if (!schedule_is_running || schedule_is_paused) return;

    unsigned long now = millis();

    // Stop request
    if (schedule_stop_requested) {
        schedule_is_running = false;
        schedule_waiting_for_footswitch = false;
        schedule_stop_requested = false;

        full_stop();

        if (schedule_timer != NULL) {
            lv_timer_del((lv_timer_t*)schedule_timer);
            schedule_timer = NULL;
        }

        Serial.println("=== Schedule STOPPED by user ===");
        return;
    }

    // If we are in the schedule foot-switch wait window (#2..N)
    if (schedule_waiting_for_footswitch) {
        // GUARD: keep motor OFF while waiting
        setPCF8574Pin(0, false);
        setPCF8574Pin(1, false);

        // LED solid ON during wait
        setPCF8574Pin(4, false);

        // Tone every 5 seconds (1 second tone)
        if (now - schedule_last_tone_ms >= 5000UL) {
            schedule_last_tone_ms = now;
            audio_play_tone_1s();
        }

        // Timeout window: 20 seconds -> skip treat
        if (now - schedule_wait_start_ms >= 20000UL) {
            Serial.printf("Schedule treat %d: foot-switch TIMEOUT -> skipping\n", current_treat_index + 1);
            schedule_waiting_for_footswitch = false;
            setPCF8574Pin(4, true); // LED OFF
            current_treat_index++;  // skip this treat
            return;
        }

        // Foot switch pressed -> dispense ONE treat
        if (footswitch_pressed_debounced(now)) {
            Serial.printf("Schedule treat %d: foot-switch PRESSED -> dispensing\n", current_treat_index + 1);

            schedule_waiting_for_footswitch = false;
            (void)schedule_dispense_now_on_footswitch(&schedule_stop_requested);

            setPCF8574Pin(4, true); // LED OFF after dispense
            return;
        }

        // Still waiting
        return;
    }

    // Countdown minutes
    unsigned long elapsed_total = now - schedule_start_time;
    int elapsed_minutes = (int)(elapsed_total / 60000UL);

    int total_minutes = selected_hours_to_dispense * 60;
    schedule_remaining_minutes = total_minutes - elapsed_minutes;
    if (schedule_remaining_minutes < 0) schedule_remaining_minutes = 0;

    // Update UI only when minute changes
    if (schedule_remaining_minutes != schedule_last_displayed_minutes) {
        schedule_last_displayed_minutes = schedule_remaining_minutes;
        update_schedule_3_ui();
    }

    // Completion check
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

    // Dispense when time reached (re-entry safe)
    static unsigned long last_dispense_time = 0;
    static bool dispense_in_progress = false;

    if (current_treat_index < total_scheduled_treats) {
        int next_treat_time = scheduled_times[current_treat_index];

        if (schedule_is_running && current_treat_index == 1 && every_30s(now)) {
            Serial.printf("Checking treat 2: current_time=%d min, scheduled=%d min\n",
                          elapsed_minutes, next_treat_time);
        }

        if (elapsed_minutes >= next_treat_time) {
            if (!dispense_in_progress && (now - last_dispense_time >= 30000UL)) {
                dispense_in_progress = true;
                last_dispense_time = now;

                Serial.printf("TRIGGER schedule treat: idx=%d (treat=%d), now=%d min, scheduled=%d min\n",
                              current_treat_index, current_treat_index + 1, elapsed_minutes, next_treat_time);

                schedule_dispense_treat();

                dispense_in_progress = false;
            }
        }
    }
}

// ---------------------------
// Audio (DAC bell)
// ---------------------------
void init_audio() {
    dac_output_enable(DAC_CHANNEL_2); // GPIO 26
    dac_output_voltage(DAC_CHANNEL_2, 0);
    Serial.println("DAC audio initialized on GPIO 26 (DAC_CHANNEL_2)");
}

// ---------------------------
// Motor/IR control
// ---------------------------
void full_stop() {
    // Motor stop
    setPCF8574Pin(0, false);
    setPCF8574Pin(1, false);

    // LED + IR off (active-low)
    setPCF8574Pin(4, true);
    setPCF8574Pin(5, true);

    Serial.println("Full stop: Motor and LED OFF");
}

void Motor_Start() {
    // CW: IN1 LOW, IN2 HIGH (matches your previous code)
    setPCF8574Pin(0, true);
    setPCF8574Pin(1, false);
    Serial.println("Motor ON (CW)");
}

static bool beam_initial_state = true;
static inline bool read_beam_P6() { return readPCF8574Pin(6); }

void IR_Start() {
    setPCF8574Pin(4, false); // LED ON
    setPCF8574Pin(5, false); // IR TX ON

    delay(150);
    beam_initial_state = read_beam_P6();
    Serial.print("IR Start - initial P6: ");
    Serial.println(beam_initial_state ? "HIGH" : "LOW");
}

void IR_Stop() {
    setPCF8574Pin(4, true);
    setPCF8574Pin(5, true);
    Serial.println("IR transmitter and receiver OFF");
}

// ---------------------------
// Standalone foot-switch training tick (20s window)
// ---------------------------
static void footswitch_train_tick(lv_timer_t* timer) {
    (void)timer;

    // Keep remote trigger alive
    ensure_remote_poll_timer_running();

    const unsigned long now = millis();

    if (!foot_train_active) {
        setPCF8574Pin(4, true);
        if (foot_train_timer) { lv_timer_del(foot_train_timer); foot_train_timer = NULL; }
        return;
    }

    // GUARD: keep motor outputs OFF until a valid footswitch press
    setPCF8574Pin(0, false);
    setPCF8574Pin(1, false);

    // LED ON solid for the whole 20s window (P4 active-low)
    setPCF8574Pin(4, false);

    // Tone every 5 seconds (1 second tone)
    if (now - foot_train_last_tone_ms >= 5000UL) {
        foot_train_last_tone_ms = now;
        audio_play_tone_1s();
    }

    // Timeout: 20 seconds -> no treat dispensed
    if (now - foot_train_start_ms >= 20000UL) {
        Serial.println("Foot-switch training: TIMEOUT (no treat dispensed).");

        foot_train_active = false;
        full_stop();
        setPCF8574Pin(4, true);

        if (foot_train_timer) {
            lv_timer_del(foot_train_timer);
            foot_train_timer = NULL;
        }
        return;
    }

    // If foot switch pressed within 20 seconds -> dispense exactly one treat
    if (footswitch_pressed_debounced(now)) {
        Serial.println("Foot-switch training: PRESSED -> dispensing 1 treat.");

        // Stop the training timer first so we cannot re-enter
        foot_train_active = false;
        if (foot_train_timer) {
            lv_timer_del(foot_train_timer);
            foot_train_timer = NULL;
        }

        full_stop();

        setPCF8574Pin(4, false); // LED ON during dispense
        Motor_Start();
        IR_Start();

        const unsigned long MOTOR_TIMEOUT = TRAIN_MOTOR_RUN_MS;
        MotorStopReason reason = run_motor_with_treat_logic(MOTOR_TIMEOUT, nullptr);

        full_stop();

        Serial.print("Foot-switch dispense stop reason: ");
        Serial.println((int)reason);

        setPCF8574Pin(4, true); // LED OFF
    }
}

// ---------------------------
// Button training state machine (PIN_BUTTON) (unchanged behavior)
// ---------------------------
void train_dispense_tick(lv_timer_t * timer) {
    ensure_remote_poll_timer_running();

    if (train_dispense_stop_requested) {
        full_stop();
        led_set_solid(false);
        lv_timer_del(timer);
        train_dispense_state = 0;
        Serial.println("=== Train Dispense STOPPED ===");
        return;
    }

    unsigned long now = millis();
    bool edge_pressed = button_edge_pressed();

    (void)button_pressed_debounced(now);

    switch (train_dispense_state) {
        case 0: { // LED ON 5s
            led_set_solid(true);
            static bool audio0 = false;
            if (!audio0) { audio_play_tone_1s(); audio0 = true; }

            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
                audio0 = false;
                Serial.println("EDGE -> start dispense (from case 0)");
            } else if (now - state_start_time > 5000) {
                train_dispense_state = 1;
                state_start_time = now;
                audio0 = false;
            }
            break;
        }

        case 1: { // LED OFF 2s
            led_set_solid(false);
            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
            } else if (now - state_start_time > 2000) {
                train_dispense_state = 2;
                state_start_time = now;
            }
            break;
        }

        case 2: { // LED ON 5s
            led_set_solid(true);
            static bool audio2 = false;
            if (!audio2) { audio_play_tone_1s(); audio2 = true; }

            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
                audio2 = false;
            } else if (now - state_start_time > 5000) {
                train_dispense_state = 3;
                state_start_time = now;
                last_blink = now;
                led_blink_mode = true;
                led_blink_state = false;
                audio2 = false;
            }
            break;
        }

        case 3: { // Blink 5s
            static bool audiob = false;
            if (!audiob) { audio_play_tone_1s(); audiob = true; }

            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
                led_blink_mode = false;
                audiob = false;
            } else if (now - state_start_time > 5000) {
                train_dispense_state = 99;
                led_blink_mode = false;
                led_set_solid(false);
                audiob = false;
            } else {
                led_blink_tick(now);
            }
            break;
        }

        case 10: { // Dispense: motor + IR + shared stop logic
            led_blink_mode = false;
            led_set_solid(false);
            setPCF8574Pin(4, false); // LED ON

            Motor_Start();
            IR_Start();

            const unsigned long MOTOR_TIMEOUT = TRAIN_MOTOR_RUN_MS;
            MotorStopReason reason = run_motor_with_treat_logic(MOTOR_TIMEOUT, &train_dispense_stop_requested);

            full_stop();

            Serial.print("Train stop reason: ");
            Serial.println((int)reason);

            train_dispense_state = 99;
            break;
        }

        case 99: { // Done
            full_stop();
            led_set_solid(false);
            lv_timer_del(timer);
            train_dispense_state = 0;
            Serial.println("=== Train Dispense Complete ===");
            break;
        }
    }
}

// ---------------------------
// Actions (LVGL events)
// ---------------------------

// Manual treat behavior:
// Press button -> beep+LED ON -> wait 5s (LED stays on) -> beep -> dispense
void action_manual_dispense_treat(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    Serial.println("\n=== Manual Treat Dispense (timed) Started ===");

    // Beep + LED ON
    audio_play_tone_1s();
    setPCF8574Pin(4, false); // LED ON

    // Wait 5 seconds (LED stays ON)
    delay(5000);

    // Beep
    audio_play_tone_1s();

    // Dispense treat
    Motor_Start();
    IR_Start();

    const unsigned long MOTOR_TIMEOUT = TRAIN_MOTOR_RUN_MS;
    MotorStopReason reason = run_motor_with_treat_logic(MOTOR_TIMEOUT, nullptr);

    full_stop(); // turns motor + LED off

    Serial.print("Manual stop reason: ");
    Serial.println((int)reason);

    Serial.println("=== Manual Treat Dispense (timed) Complete ===\n");
}

void action_train_dispense_treat(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    full_stop();
    initPCF8574Pins();
    delay(100);

    Serial.println("=== Train Dispense STARTED ===");

    train_dispense_stop_requested = false;
    train_dispense_state = 0;
    state_start_time = millis();

    lv_timer_create(train_dispense_tick, 50, NULL);
}

// Standalone foot-switch training action (20s window) - can also be triggered by P7 remote
void action_train_footswitch_dispense_start(lv_event_t* e) {
    (void)e;
    ensure_remote_poll_timer_running();
    start_footswitch_training_window();
}

void action_train_dispense_stop(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    Serial.println("=== Train Dispense STOP requested ===");
    train_dispense_stop_requested = true;
    full_stop();
}

void action_schedule_add_treat_num(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    int idx = lv_roller_get_selected(objects.schedule_1_treatsnumber);
    selected_treats_number = idx + 1;
    Serial.print("Treats to dispense selected: ");
    Serial.println(selected_treats_number);
    update_schedule_3_ui();
}

void action_schedule_add_hours(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    int idx = lv_roller_get_selected(objects.schedule_2_hours_to_dispense);
    selected_hours_to_dispense = idx + 1;
    Serial.print("Hours to dispense selected: ");
    Serial.println(selected_hours_to_dispense);
    update_schedule_3_ui();
}

void action_schedule_2_next(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    Serial.println("Transitioning to Schedule 3 screen");
    Serial.printf("Current values: treats=%d, hours=%d\n", selected_treats_number, selected_hours_to_dispense);
}

void action_scheduletreatdispensestart(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    Serial.println("=== Schedule Dispense START ===");
    full_stop();

    if (!schedule_is_running) {
        schedule_stop_requested = false;

        schedule_treats_dispensed = 0;
        schedule_remaining_minutes = selected_hours_to_dispense * 60;
        current_treat_index = 0;
        schedule_waiting_for_footswitch = false;

        Serial.printf("Initializing schedule: %d hours = %d minutes\n",
                      selected_hours_to_dispense, schedule_remaining_minutes);

        generate_schedule_times();

        schedule_is_running = true;
        schedule_is_paused = false;
        schedule_start_time = millis();

        schedule_remaining_minutes = selected_hours_to_dispense * 60;
        schedule_last_displayed_minutes = -9999; // force first refresh
        update_schedule_3_ui();

        Serial.printf("Schedule start time: %lu\n", schedule_start_time);

        if (schedule_timer != NULL) {
            lv_timer_del((lv_timer_t*)schedule_timer);
            schedule_timer = NULL;
        }

        schedule_timer = lv_timer_create(schedule_timer_tick, 100, NULL);

        Serial.println("Triggering treat #1 immediately (manual sequence)");
        schedule_dispense_treat();

        Serial.printf("Schedule started: %d treats/hr over %d hours\n",
                      selected_treats_number, selected_hours_to_dispense);
    } else if (schedule_is_paused) {
        schedule_is_paused = false;
        unsigned long pause_duration = millis() - schedule_pause_time;
        schedule_start_time += pause_duration;
        Serial.println("Schedule RESUMED");
    }
}

void action_scheduletreatdispensepause(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    if (schedule_is_running && !schedule_is_paused) {
        schedule_is_paused = true;
        schedule_pause_time = millis();
        full_stop();
        Serial.println("=== Schedule Dispense PAUSED ===");
    } else if (schedule_is_paused) {
        action_scheduletreatdispensestart(e);
    }
}

void action_scheduletreatdispensestop(lv_event_t * e) {
    (void)e;
    ensure_remote_poll_timer_running();

    Serial.println("=== Schedule Dispense STOPPED ===");

    schedule_stop_requested = true;

    schedule_is_running = false;
    schedule_is_paused = false;
    schedule_treats_dispensed = 0;
    schedule_remaining_minutes = 0;
    current_treat_index = 0;

    schedule_waiting_for_footswitch = false;

    if (schedule_timer != NULL) {
        lv_timer_del((lv_timer_t*)schedule_timer);
        schedule_timer = NULL;
    }

    full_stop();
    update_schedule_3_ui();

    Serial.println("=== Schedule Dispense STOP Complete ===");
}

} // end extern "C"
