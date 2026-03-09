// actions.cpp
//
// IMPORTANT INTEGRATION NOTE:
// - Call actions_init(); ONCE after LVGL + I2C/PCF are initialized.
// - main.cpp should define the calibrated zero-current variable, e.g.:
//       float ZERO_CURRENT_VOLTAGE = 2.50f;
//   and calibrate it at startup before motor use.
// - After that, the P7 remote trigger works even if scheduled mode is not running.
//
// Behaviors included:
// 1) Manual treat mode sequence (NON-BLOCKING):
//      - LED ON (P4 active-low)
//      - Beep 1s (assumes audio_play_tone_1s() is non-blocking)
//      - Dispense treat asynchronously
//      - LED turns OFF after 5 seconds total-on-time (minimum)
//
// 2) Foot-switch training (standalone / “anytime”) (NON-BLOCKING):
//      - 20s window
//      - LED solid ON (P4)
//      - 1s tone every 5s
//      - Wait for foot switch on P3 (active-low)
//      - If pressed within window -> dispense ONE treat asynchronously
//      - If timeout -> no treat
//
// 3) Scheduled treat mode behavior (NON-BLOCKING):
//      - Treat #1 follows the manual treat sequence
//      - Treat #2..N require foot-switch activation (20s window). If not pressed -> skip treat.
//
// 4) IR remote trigger on PCF P7 (active-low):
//      - Debounced edge on P7 starts the standalone foot-switch training window at ANY time
//      - Implemented via LVGL timer polling P7
//
// 5) Jam detection uses TWO paths:
//      A) PRIMARY: rotary no-motion jam detection
//      B) SECONDARY: elevated filtered current jam detection
//      - Hard jam threshold path removed
//
// 6) Serial summary includes motion + current info to help tuning.
//

#include <Arduino.h>
#include <stdlib.h>
#include <math.h>
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

// -----------------------------
// Fallback pin defines (safe)
// -----------------------------
#ifndef PIN_LED
#define PIN_LED 4  // LED control on PCF (active-low)
#endif

#ifndef PIN_IR_TX
#define PIN_IR_TX 5 // IR transmitter enable on PCF (active-low)
#endif

#ifndef PIN_FOOTSWITCH
#define PIN_FOOTSWITCH 3 // Foot switch input on PCF (active-low)
#endif

#ifndef PIN_REMOTE
#define PIN_REMOTE 7 // Remote trigger input on PCF (active-low)
#endif

#ifndef PIN_MOTOR_IN1
#define PIN_MOTOR_IN1 0
#endif

#ifndef PIN_MOTOR_IN2
#define PIN_MOTOR_IN2 1
#endif

#ifndef PIN_ROTARY
#define PIN_ROTARY 2 // Rotary switch on PCF
#endif

#ifndef PIN_IR_RX
#define PIN_IR_RX 6 // Beam receiver on PCF (HIGH=intact, LOW=broken)
#endif

#ifndef PIN_BUTTON
#define PIN_BUTTON 2
#endif

// ---- Restore / fallback definitions ----
#ifndef AUDIO_DAC_CHANNEL
#define AUDIO_DAC_CHANNEL DAC_CHANNEL_1
#endif

#ifndef WAV_FILE
#define WAV_FILE "/treat.wav"
#endif

// -----------------------------
// Motor Current Sense
// -----------------------------
#ifndef CURRENT_SENSOR_PIN
#define CURRENT_SENSOR_PIN 35
#endif

#ifndef CURRENT_SENSOR_ADC_FS_VOLTS
#define CURRENT_SENSOR_ADC_FS_VOLTS 3.3f
#endif

// ACS712-30A = 66 mV/A
#ifndef SENSITIVITY
#define SENSITIVITY 0.066f
#endif

#ifndef CURRENT_SENSOR_AVG_SAMPLES
#define CURRENT_SENSOR_AVG_SAMPLES 4
#endif

#ifndef MOTOR_UNJAM_REVERSE_MS
#define MOTOR_UNJAM_REVERSE_MS 1000UL
#endif

#ifndef MOTOR_MAX_UNJAM_RETRIES
#define MOTOR_MAX_UNJAM_RETRIES 3
#endif

#ifndef MOTOR_JAM_REARM_MS
#define MOTOR_JAM_REARM_MS 150UL
#endif

#ifndef CURRENT_FILTER_ALPHA
#define CURRENT_FILTER_ALPHA 0.20f
#endif

// -----------------------------
// Primary jam detection: rotary no-motion
// -----------------------------
#ifndef JAM_NO_MOTION_STARTUP_MS
#define JAM_NO_MOTION_STARTUP_MS 400UL
#endif

#ifndef JAM_NO_MOTION_TIMEOUT_MS
#define JAM_NO_MOTION_TIMEOUT_MS 1500UL
#endif

// -----------------------------
// Secondary jam detection: filtered current
// Tuned conservatively to avoid normal-start false positives.
// -----------------------------
#ifndef JAM_FILTERED_THRESHOLD_AMPS
#define JAM_FILTERED_THRESHOLD_AMPS 0.85f
#endif

#ifndef JAM_FILTERED_CONFIRM_MS
#define JAM_FILTERED_CONFIRM_MS 250UL
#endif

// Must be defined in main.cpp and calibrated there.
extern float ZERO_CURRENT_VOLTAGE;

#ifndef TRAIN_MOTOR_RUN_MS
#define TRAIN_MOTOR_RUN_MS 8000UL
#endif

#ifndef MOTOR_JOB_TICK_MS
#define MOTOR_JOB_TICK_MS 5
#endif

// -----------------------------
// Remote Control Settings
// -----------------------------
#ifndef IR_SETTLE_MS
#define IR_SETTLE_MS 150UL
#endif

#ifndef TRAINING_ARM_MS
#define TRAINING_ARM_MS 50UL
#endif

#ifndef REMOTE_POLL_INTERVAL_MS
#define REMOTE_POLL_INTERVAL_MS 5UL
#endif

#ifndef REMOTE_DEBOUNCE_MS
#define REMOTE_DEBOUNCE_MS 10UL
#endif

// ---------------------------
// Serial rate limiting helpers
// ---------------------------
static inline bool every_30s(unsigned long now_ms) {
    static unsigned long last = 0;
    if (now_ms - last >= 30000UL) { last = now_ms; return true; }
    return false;
}

// ---------------------------
// Current sensor helpers
// ---------------------------
static inline int read_current_sensor_adc_avg() {
    int sample_count = CURRENT_SENSOR_AVG_SAMPLES;
    if (sample_count < 1) sample_count = 1;

    long sum = 0;
    for (int i = 0; i < sample_count; i++) {
        sum += analogRead(CURRENT_SENSOR_PIN);
    }

    return (int)(sum / sample_count);
}

static inline float adc_to_voltage(int adcValue) {
    return (adcValue / 4095.0f) * CURRENT_SENSOR_ADC_FS_VOLTS;
}

static inline float voltage_to_current_amps(float voltage) {
    float current = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY;
    if (current < 0.0f) current = -current;
    return current;
}

// ---------------------------
// State variables (legacy button training state machine)
// ---------------------------
volatile bool train_dispense_stop_requested = false;
static int  train_dispense_state = 0;
static unsigned long state_start_time = 0;

// LED blink state
static bool led_blink_state = false;
static unsigned long last_blink = 0;

// ---------------------------
// Foot-switch training (standalone action)
// ---------------------------
static lv_timer_t* foot_train_timer = NULL;
static bool foot_train_active = false;
static unsigned long foot_train_start_ms = 0;
static unsigned long foot_train_last_tone_ms = 0;
static unsigned long foot_train_armed_after_ms = 0;

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
static int scheduled_times[96];
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
// Shared motor run logic
// ---------------------------------------------
enum MotorStopReason {
    STOP_TIMEOUT = 0,
    STOP_TREAT_NEXT_HIGH,
    STOP_NO_TREAT_3_TRANSITIONS,
    STOP_JAM,
    STOP_EXTERNAL_REQUEST
};

// ---------------------------
// LED helpers
// ---------------------------
static inline void led_apply(bool on) {
    setPCF8574Pin(PIN_LED, !on); // active-low
}

static inline void led_set_solid(bool on) {
    led_blink_mode = false;
    led_is_on_solid = on;
    led_apply(on);
}

static inline void led_blink_tick(unsigned long now) {
    if (!led_blink_mode) return;
    if (now - last_blink >= 250UL) {
        last_blink = now;
        led_blink_state = !led_blink_state;
        led_apply(led_blink_state);
    }
}

// ---------------------------
// Debounced inputs
// ---------------------------
static inline bool footswitch_pressed_debounced(unsigned long now_ms) {
    bool raw = !readPCF8574Pin(PIN_FOOTSWITCH); // active-low
    static bool prev = false;
    static unsigned long t = 0;
    if (raw != prev) { prev = raw; t = now_ms; }
    return raw && (now_ms - t > 30UL);
}

// ---------------------------
// Forward declarations
// ---------------------------
extern "C" {
void init_audio();
void full_stop();
void update_schedule_3_ui();
void Motor_Start();
void IR_Start();
void IR_Stop();
}

// ---------------------------------------------
// Non-blocking async motor job
// ---------------------------------------------
typedef void (*MotorJobDoneCb)(MotorStopReason reason);

struct AsyncMotorJob {
    bool active = false;
    bool ir_started = false;

    unsigned long start_ms = 0;
    unsigned long timeout_ms = 0;
    unsigned long led_on_start_ms = 0;
    unsigned long led_min_on_ms = 0;
    unsigned long ir_valid_after_ms = 0;

    volatile bool* external_stop_flag = nullptr;
    MotorJobDoneCb done_cb = nullptr;

    lv_timer_t* timer = NULL;

    // Treat logic state
    bool treatDispensed = false;
    bool lastRotary = false;
    int  lhTransitions = 0;
    bool stopRequested_NoTreat = false;
    bool waitForNextHigh_AfterTreat = false;
    bool seenLowAfterTreat = false;

    // Current monitoring state
    float inst_current_amps = 0.0f;
    float filtered_current_amps = 0.0f;
    float peak_current_amps = 0.0f;
    float last_voltage = 0.0f;
    float last_delta_v = 0.0f;
    int   last_adc = 0;

    // Motion / no-motion jam detection
    unsigned long last_motion_ms = 0;
    unsigned long motion_arm_after_ms = 0;
    bool saw_motion_this_run = false;

    // Filtered current jam detection
    unsigned long filtered_jam_start_ms = 0;

    // Unjam / reverse state
    bool reverse_active = false;
    unsigned long reverse_start_ms = 0;
    int jam_retries = 0;
    unsigned long jam_rearm_after_ms = 0;

    MotorStopReason final_reason = STOP_TIMEOUT;
};

static AsyncMotorJob g_motor_job;

// ---------------------------
// Helper prototypes
// ---------------------------
static void motor_job_tick(lv_timer_t* timer);
static void motor_job_finish(MotorStopReason reason);
static void start_async_motor_job(unsigned long timeout_ms,
                                  unsigned long led_on_start_ms,
                                  unsigned long led_min_on_ms,
                                  volatile bool* external_stop_flag,
                                  MotorJobDoneCb done_cb);
static void led_off_timer_cb(lv_timer_t* t);
static void footswitch_train_tick(lv_timer_t* timer);
static void remote_poll_tick(lv_timer_t* t);
static void ensure_remote_poll_timer_running();
static void cancel_footswitch_training_window();
static void start_footswitch_training_window();
static void start_unjam_reverse(unsigned long now, const char* cause);

// ---------------------------
// Button helpers
// ---------------------------
static inline bool button_pressed_debounced(unsigned long now_ms) {
    bool raw = !readPCF8574Pin(PIN_BUTTON);
    static bool prev = false;
    static unsigned long t = 0;
    if (raw != prev) { prev = raw; t = now_ms; }
    return raw && (now_ms - t > 30UL);
}

static inline bool button_edge_pressed() {
    bool raw = !readPCF8574Pin(PIN_BUTTON);
    static bool last = false;
    bool edge = raw && !last;
    last = raw;
    return edge;
}

// ---------------------------
// One-shot LED off helper
// ---------------------------
static void led_off_timer_cb(lv_timer_t* t) {
    led_set_solid(false);
    lv_timer_del(t);
}

static inline void motor_ir_stop_only() {
    setPCF8574Pin(PIN_MOTOR_IN1, false);
    setPCF8574Pin(PIN_MOTOR_IN2, false);
    setPCF8574Pin(PIN_IR_TX, true); // active-low off
}

static inline void finish_with_led_min_on_time(unsigned long led_on_start_ms,
                                               unsigned long min_on_ms = 5000UL) {
    unsigned long elapsed = millis() - led_on_start_ms;
    if (elapsed >= min_on_ms) {
        led_set_solid(false);
    } else {
        lv_timer_create(led_off_timer_cb, min_on_ms - elapsed, NULL);
    }
}

static inline void stop_motor_ir_and_hold_led_if_needed(unsigned long led_on_start_ms,
                                                        unsigned long min_on_ms) {
    motor_ir_stop_only();
    finish_with_led_min_on_time(led_on_start_ms, min_on_ms);
}

// ---------------------------
// Async motor job internals
// ---------------------------
static void motor_job_reset_treat_logic() {
    g_motor_job.treatDispensed = false;
    g_motor_job.lhTransitions = 0;
    g_motor_job.stopRequested_NoTreat = false;
    g_motor_job.waitForNextHigh_AfterTreat = false;
    g_motor_job.seenLowAfterTreat = false;
    g_motor_job.lastRotary = readPCF8574Pin(PIN_ROTARY);

    g_motor_job.inst_current_amps = 0.0f;
    g_motor_job.filtered_current_amps = 0.0f;
    g_motor_job.peak_current_amps = 0.0f;
    g_motor_job.last_voltage = 0.0f;
    g_motor_job.last_delta_v = 0.0f;
    g_motor_job.last_adc = 0;

    g_motor_job.last_motion_ms = millis();
    g_motor_job.motion_arm_after_ms = g_motor_job.last_motion_ms + JAM_NO_MOTION_STARTUP_MS;
    g_motor_job.saw_motion_this_run = false;

    g_motor_job.filtered_jam_start_ms = 0;

    // legacy mirrors for compatibility/debug
    treatDispensed = false;
    lhTransitions = 0;
    stopRequested_NoTreat = false;
    waitForNextHigh_AfterTreat = false;
    seenLowAfterTreat = false;
    lastRotary = g_motor_job.lastRotary;
}

static void start_async_motor_job(unsigned long timeout_ms,
                                  unsigned long led_on_start_ms,
                                  unsigned long led_min_on_ms,
                                  volatile bool* external_stop_flag,
                                  MotorJobDoneCb done_cb) {
    if (g_motor_job.active) {
        Serial.println("Async motor job already active; ignoring new request.");
        return;
    }

    const unsigned long now = millis();

    g_motor_job.active = true;
    g_motor_job.ir_started = false;
    g_motor_job.start_ms = now;
    g_motor_job.timeout_ms = timeout_ms;
    g_motor_job.led_on_start_ms = led_on_start_ms;
    g_motor_job.led_min_on_ms = led_min_on_ms;
    g_motor_job.ir_valid_after_ms = now + IR_SETTLE_MS;
    g_motor_job.external_stop_flag = external_stop_flag;
    g_motor_job.done_cb = done_cb;
    g_motor_job.final_reason = STOP_TIMEOUT;

    g_motor_job.reverse_active = false;
    g_motor_job.reverse_start_ms = 0;
    g_motor_job.jam_retries = 0;
    g_motor_job.jam_rearm_after_ms = 0;

    motor_job_reset_treat_logic();

    led_set_solid(true);
    Motor_Start();
    setPCF8574Pin(PIN_IR_TX, false); // IR ON, non-blocking
    g_motor_job.ir_started = true;

    if (g_motor_job.timer) {
        lv_timer_del(g_motor_job.timer);
        g_motor_job.timer = NULL;
    }

    g_motor_job.timer = lv_timer_create(motor_job_tick, MOTOR_JOB_TICK_MS, NULL);
    Serial.println("Async motor job started.");
}

static void motor_job_finish(MotorStopReason reason) {
    g_motor_job.final_reason = reason;

    if (g_motor_job.timer) {
        lv_timer_del(g_motor_job.timer);
        g_motor_job.timer = NULL;
    }

    g_motor_job.active = false;
    g_motor_job.reverse_active = false;

    Serial.printf(
        "RUN SUMMARY: peak=%.2fA filtered=%.2fA inst=%.2fA zero=%.3fV retries=%d reason=%d transitions=%d treat=%d sawMotion=%d noMotionMs=%lu\r\n",
        g_motor_job.peak_current_amps,
        g_motor_job.filtered_current_amps,
        g_motor_job.inst_current_amps,
        ZERO_CURRENT_VOLTAGE,
        g_motor_job.jam_retries,
        (int)reason,
        g_motor_job.lhTransitions,
        g_motor_job.treatDispensed ? 1 : 0,
        g_motor_job.saw_motion_this_run ? 1 : 0,
        (unsigned long)(millis() - g_motor_job.last_motion_ms)
    );

    if (g_motor_job.done_cb) {
        MotorJobDoneCb cb = g_motor_job.done_cb;
        g_motor_job.done_cb = nullptr;
        cb(reason);
    }
}

static void start_unjam_reverse(unsigned long now, const char* cause) {
    g_motor_job.jam_retries++;
    g_motor_job.filtered_jam_start_ms = 0;

    Serial.printf("JAM detected by %s -> retry %d/%d\r\n",
                  cause,
                  g_motor_job.jam_retries,
                  MOTOR_MAX_UNJAM_RETRIES);

    if (g_motor_job.jam_retries > MOTOR_MAX_UNJAM_RETRIES) {
        Serial.println("Max unjam retries exceeded. Stopping motor job as JAM.");
        motor_job_finish(STOP_JAM);
        return;
    }

    setPCF8574Pin(PIN_MOTOR_IN1, false);
    setPCF8574Pin(PIN_MOTOR_IN2, true);
    Serial.println("Motor ON (CCW / unjam)");

    g_motor_job.reverse_active = true;
    g_motor_job.reverse_start_ms = now;
    g_motor_job.last_motion_ms = now;
}

static void motor_job_tick(lv_timer_t* timer) {
    (void)timer;
    const unsigned long now = millis();

    if (!g_motor_job.active) {
        if (g_motor_job.timer) {
            lv_timer_del(g_motor_job.timer);
            g_motor_job.timer = NULL;
        }
        return;
    }

    if (g_motor_job.external_stop_flag && *g_motor_job.external_stop_flag) {
        Serial.println("External stop flag set -> abort motor run.");
        motor_job_finish(STOP_EXTERNAL_REQUEST);
        return;
    }

    if ((now - g_motor_job.start_ms) >= g_motor_job.timeout_ms) {
        Serial.println("Motor timeout reached.");
        motor_job_finish(STOP_TIMEOUT);
        return;
    }

    // If reversing to unjam, hold reverse for a fixed interval, then resume forward.
    if (g_motor_job.reverse_active) {
        if ((now - g_motor_job.reverse_start_ms) >= MOTOR_UNJAM_REVERSE_MS) {
            g_motor_job.reverse_active = false;
            g_motor_job.jam_rearm_after_ms = now + MOTOR_JAM_REARM_MS;
            g_motor_job.motion_arm_after_ms = now + JAM_NO_MOTION_STARTUP_MS;
            g_motor_job.filtered_jam_start_ms = 0;
            g_motor_job.last_motion_ms = now;
            g_motor_job.saw_motion_this_run = false;

            Motor_Start();
            g_motor_job.ir_valid_after_ms = now + IR_SETTLE_MS;

            Serial.printf("Unjam reverse complete. Resuming forward. Retry %d/%d\r\n",
                          g_motor_job.jam_retries, MOTOR_MAX_UNJAM_RETRIES);
        }
        return;
    }

    // Current monitoring while motor runs (averaged ADC)
    const int adcValue = read_current_sensor_adc_avg();
    const float voltage = adc_to_voltage(adcValue);
    const float delta_v = voltage - ZERO_CURRENT_VOLTAGE;
    const float inst_current = voltage_to_current_amps(voltage);

    if (g_motor_job.filtered_current_amps <= 0.0001f) {
        g_motor_job.filtered_current_amps = inst_current;
    } else {
        g_motor_job.filtered_current_amps =
            (CURRENT_FILTER_ALPHA * inst_current) +
            ((1.0f - CURRENT_FILTER_ALPHA) * g_motor_job.filtered_current_amps);
    }

    if (inst_current > g_motor_job.peak_current_amps) {
        g_motor_job.peak_current_amps = inst_current;
    }

    g_motor_job.inst_current_amps = inst_current;
    g_motor_job.last_voltage = voltage;
    g_motor_job.last_delta_v = delta_v;
    g_motor_job.last_adc = adcValue;

    // Wait for IR settle before evaluating beam or rotary-based logic
    if (now < g_motor_job.ir_valid_after_ms) {
        return;
    }

    bool rawValue     = readPCF8574Pin(PIN_IR_RX);   // HIGH=intact, LOW=broken
    bool rotarySwitch = readPCF8574Pin(PIN_ROTARY);  // HIGH=safe-to-stop
    bool beamBroken   = !rawValue;

    // Rotary motion detection
    if (rotarySwitch != g_motor_job.lastRotary) {
        g_motor_job.last_motion_ms = now;
        g_motor_job.saw_motion_this_run = true;
    }

    // ---------------------------------------------
    // JAM PATH A: PRIMARY - no rotary movement
    // Does NOT require current threshold.
    // ---------------------------------------------
    if (now >= g_motor_job.jam_rearm_after_ms &&
        now >= g_motor_job.motion_arm_after_ms &&
        !g_motor_job.treatDispensed) {

        const bool no_motion_too_long =
            ((now - g_motor_job.last_motion_ms) >= JAM_NO_MOTION_TIMEOUT_MS);

        if (no_motion_too_long) {
            Serial.printf("NO-MOTION JAM detected! noMotion=%lums I=%.2fA Ifilt=%.2fA Ipeak=%.2fA\r\n",
                          (unsigned long)(now - g_motor_job.last_motion_ms),
                          g_motor_job.inst_current_amps,
                          g_motor_job.filtered_current_amps,
                          g_motor_job.peak_current_amps);
            start_unjam_reverse(now, "NO_MOTION");
            return;
        }
    }

    // ---------------------------------------------
    // JAM PATH B: SECONDARY - elevated filtered current
    // Runs after rotary no-motion logic and uses startup blanking.
    // ---------------------------------------------
    if (now >= g_motor_job.jam_rearm_after_ms &&
        now >= g_motor_job.motion_arm_after_ms) {

        const bool filtered_over =
            (g_motor_job.filtered_current_amps >= JAM_FILTERED_THRESHOLD_AMPS);

        if (filtered_over) {
            if (g_motor_job.filtered_jam_start_ms == 0) {
                g_motor_job.filtered_jam_start_ms = now;
            }
        } else {
            g_motor_job.filtered_jam_start_ms = 0;
        }

        const bool filtered_jam_confirmed =
            (g_motor_job.filtered_jam_start_ms != 0) &&
            ((now - g_motor_job.filtered_jam_start_ms) >= JAM_FILTERED_CONFIRM_MS);

        if (filtered_jam_confirmed) {
            Serial.printf("FILTERED-CURRENT JAM detected! Ifilt=%.2fA I=%.2fA Ipeak=%.2fA overFor=%lums\r\n",
                          g_motor_job.filtered_current_amps,
                          g_motor_job.inst_current_amps,
                          g_motor_job.peak_current_amps,
                          (unsigned long)(now - g_motor_job.filtered_jam_start_ms));
            start_unjam_reverse(now, "FILTERED_CURRENT");
            return;
        }
    }

    if (!g_motor_job.treatDispensed && beamBroken) {
        g_motor_job.treatDispensed = true;
        g_motor_job.lhTransitions = 0;
        g_motor_job.stopRequested_NoTreat = false;
        g_motor_job.waitForNextHigh_AfterTreat = true;
        g_motor_job.seenLowAfterTreat = !rotarySwitch;

        treatDispensed = true;
        lhTransitions = 0;
        stopRequested_NoTreat = false;
        waitForNextHigh_AfterTreat = true;
        seenLowAfterTreat = g_motor_job.seenLowAfterTreat;

        Serial.println("Beam broken! Treat dispensed.");
    }

    if (!g_motor_job.treatDispensed) {
        if (!g_motor_job.lastRotary && rotarySwitch) {
            g_motor_job.lhTransitions++;
            lhTransitions = g_motor_job.lhTransitions;

            Serial.print("Rotary LOW->HIGH transitions: ");
            Serial.println(g_motor_job.lhTransitions);

            if (g_motor_job.lhTransitions >= 3) {
                g_motor_job.stopRequested_NoTreat = true;
                stopRequested_NoTreat = true;
            }
        }
    }

    if (g_motor_job.waitForNextHigh_AfterTreat) {
        if (!g_motor_job.seenLowAfterTreat) {
            if (!rotarySwitch) {
                g_motor_job.seenLowAfterTreat = true;
                seenLowAfterTreat = true;
            }
        } else {
            if (rotarySwitch) {
                Serial.println("Stopping at NEXT HIGH after treat dispense.");
                motor_job_finish(STOP_TREAT_NEXT_HIGH);
                return;
            }
        }
    }

    if (g_motor_job.stopRequested_NoTreat && rotarySwitch) {
        Serial.println("No treat after 3 LOW->HIGH transitions. Stopping at HIGH.");
        motor_job_finish(STOP_NO_TREAT_3_TRANSITIONS);
        return;
    }

    g_motor_job.lastRotary = rotarySwitch;
    lastRotary = rotarySwitch;
}

// =====================================================
// IR Remote (P7 active-low) -> start foot-switch training ANYTIME
// =====================================================
static lv_timer_t* remote_poll_timer = NULL;

static inline bool remote_p7_edge_pressed(unsigned long now_ms) {
    bool raw = !readPCF8574Pin(PIN_REMOTE); // active-low, pressed = true

    static bool last_raw = false;
    static bool stable_state = false;
    static unsigned long last_change_ms = 0;

    if (raw != last_raw) {
        last_raw = raw;
        last_change_ms = now_ms;
    }

    if ((now_ms - last_change_ms) >= REMOTE_DEBOUNCE_MS) {
        if (stable_state != raw) {
            bool previous_stable = stable_state;
            stable_state = raw;
            if (stable_state && !previous_stable) {
                return true;
            }
        }
    }

    return false;
}

static void cancel_footswitch_training_window() {
    if (!foot_train_active && !foot_train_timer) return;

    Serial.println("Foot-switch training window CANCELLED");

    foot_train_active = false;

    if (foot_train_timer) {
        lv_timer_del(foot_train_timer);
        foot_train_timer = NULL;
    }

    full_stop();
    led_set_solid(false);
}

static void start_footswitch_training_window() {
    if (foot_train_active || foot_train_timer || g_motor_job.active) return;

    Serial.println("Foot-switch training window STARTED (remote/UI)");

    initPCF8574Pins();
    full_stop();

    // Release quasi-bidirectional pins for input reads
    setPCF8574Pin(PIN_FOOTSWITCH, false);
    setPCF8574Pin(PIN_REMOTE, false);

    foot_train_active = true;
    foot_train_start_ms = millis();
    foot_train_last_tone_ms = foot_train_start_ms - 5000UL;
    foot_train_armed_after_ms = millis() + TRAINING_ARM_MS;

    led_set_solid(true);

    foot_train_timer = lv_timer_create(footswitch_train_tick, 50, NULL);

    Serial.printf("Training LED: PIN_LED=%d read=%d (LOW=ON if active-low)\n",
                  (int)PIN_LED, (int)readPCF8574Pin(PIN_LED));
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
        setPCF8574Pin(PIN_REMOTE, false); // release pin for input
        remote_poll_timer = lv_timer_create(remote_poll_tick, REMOTE_POLL_INTERVAL_MS, NULL);
        Serial.printf("Remote poll timer started (P7 training trigger), poll=%lu ms, debounce=%lu ms\r\n",
                      (unsigned long)REMOTE_POLL_INTERVAL_MS,
                      (unsigned long)REMOTE_DEBOUNCE_MS);
    }
}

// ---------------------------
// UI updater
// ---------------------------
extern "C" void update_schedule_3_ui() {
    if (objects.treats_per_hour) {
        char treats_str[10];
        snprintf(treats_str, sizeof(treats_str), "%d", selected_treats_number);
        lv_label_set_text(objects.treats_per_hour, treats_str);
    }

    if (objects.treats_dispensed) {
        char dispensed_str[10];
        snprintf(dispensed_str, sizeof(dispensed_str), "%d", schedule_treats_dispensed);
        lv_label_set_text(objects.treats_dispensed, dispensed_str);
    }

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
static void generate_schedule_times() {
    total_scheduled_treats = 0;
    current_treat_index = 0;

    int treats_per_hour = selected_treats_number;
    int total_hours     = selected_hours_to_dispense;

    if (treats_per_hour < 2) treats_per_hour = 2;

    int reliable_per_hour = (treats_per_hour + 1) / 2;
    int random_per_hour   = treats_per_hour / 2;
    int spacing = 60 / reliable_per_hour;

    for (int hour = 0; hour < total_hours; hour++) {
        int hour_offset = hour * 60;

        for (int k = 0; k < reliable_per_hour; k++) {
            int t = k * spacing;
            if (t >= 60) t = 59;
            scheduled_times[total_scheduled_treats++] = hour_offset + t;
        }

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

// ---------------------------
// Async completion callbacks
// ---------------------------
static bool legacy_train_job_done = false;
static MotorStopReason legacy_train_reason = STOP_TIMEOUT;

static void training_motor_done_cb(MotorStopReason reason) {
    full_stop();
    Serial.print("Foot-switch dispense stop reason: ");
    Serial.println((int)reason);
}

static void manual_dispense_done_cb(MotorStopReason reason) {
    stop_motor_ir_and_hold_led_if_needed(g_motor_job.led_on_start_ms, 5000UL);
    Serial.print("Manual stop reason: ");
    Serial.println((int)reason);
    Serial.println("=== Manual Treat Dispense Complete ===\n");
}

static void schedule_treat1_done_cb(MotorStopReason reason) {
    stop_motor_ir_and_hold_led_if_needed(g_motor_job.led_on_start_ms, 5000UL);

    Serial.print("Schedule #1 stop reason: ");
    Serial.println((int)reason);

    if (reason == STOP_EXTERNAL_REQUEST) {
        Serial.println("Schedule stopped by user during treat #1; not incrementing counters.");
        return;
    }

    schedule_treats_dispensed++;
    update_schedule_3_ui();
    current_treat_index++;
}

static void schedule_footswitch_done_cb(MotorStopReason reason) {
    full_stop();

    Serial.print("Schedule foot-switch stop reason: ");
    Serial.println((int)reason);

    if (reason == STOP_EXTERNAL_REQUEST) {
        Serial.println("Schedule stopped by user during foot-switch dispense; not incrementing counters.");
        return;
    }

    schedule_treats_dispensed++;
    update_schedule_3_ui();
    current_treat_index++;
    led_set_solid(false);
}

static void legacy_train_done_cb(MotorStopReason reason) {
    full_stop();
    legacy_train_reason = reason;
    legacy_train_job_done = true;
}

// ---------------------------
// Scheduled treat helpers
// ---------------------------
static bool schedule_dispense_manual_sequence_now(volatile bool* stop_flag) {
    if (g_motor_job.active) {
        Serial.println("Schedule treat #1 ignored: motor job already active.");
        return false;
    }

    Serial.println("Schedule treat #1: manual-sequence dispense");

    const unsigned long led_on_start = millis();
    led_set_solid(true);
    audio_play_tone_1s();

    start_async_motor_job(TRAIN_MOTOR_RUN_MS,
                          led_on_start,
                          5000UL,
                          stop_flag,
                          schedule_treat1_done_cb);
    return true;
}

static bool schedule_dispense_now_on_footswitch(volatile bool* stop_flag) {
    if (g_motor_job.active) {
        Serial.println("Schedule foot-switch dispense ignored: motor job already active.");
        return false;
    }

    Serial.println("Schedule: foot-switch dispense NOW");

    full_stop();
    led_set_solid(true);

    start_async_motor_job(TRAIN_MOTOR_RUN_MS,
                          millis(),
                          0UL,
                          stop_flag,
                          schedule_footswitch_done_cb);
    return true;
}

static void schedule_dispense_treat() {
    Serial.println("=== Schedule Treat Trigger ===");

    if (current_treat_index == 0) {
        (void)schedule_dispense_manual_sequence_now(&schedule_stop_requested);
    } else {
        schedule_waiting_for_footswitch = true;
        schedule_wait_start_ms = millis();
        schedule_last_tone_ms  = schedule_wait_start_ms - 5000UL;
        Serial.printf("Schedule treat %d: waiting for FOOT SWITCH (20s)\n", current_treat_index + 1);
    }
}

// ---------------------------
// Schedule timer tick
// ---------------------------
static void schedule_timer_tick(lv_timer_t * timer) {
    (void)timer;

    if (!schedule_is_running || schedule_is_paused) return;

    unsigned long now = millis();

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

    if (schedule_waiting_for_footswitch) {
        setPCF8574Pin(PIN_MOTOR_IN1, false);
        setPCF8574Pin(PIN_MOTOR_IN2, false);

        led_set_solid(true);

        if (now - schedule_last_tone_ms >= 5000UL) {
            schedule_last_tone_ms = now;
            audio_play_tone_1s();
        }

        if (now - schedule_wait_start_ms >= 20000UL) {
            Serial.printf("Schedule treat %d: foot-switch TIMEOUT -> skipping\n", current_treat_index + 1);
            schedule_waiting_for_footswitch = false;
            led_set_solid(false);
            current_treat_index++;
            return;
        }

        if (footswitch_pressed_debounced(now) && !g_motor_job.active) {
            Serial.printf("Schedule treat %d: foot-switch PRESSED -> dispensing\n", current_treat_index + 1);
            schedule_waiting_for_footswitch = false;
            (void)schedule_dispense_now_on_footswitch(&schedule_stop_requested);
            return;
        }

        return;
    }

    unsigned long elapsed_total = now - schedule_start_time;
    int elapsed_minutes = (int)(elapsed_total / 60000UL);

    int total_minutes = selected_hours_to_dispense * 60;
    schedule_remaining_minutes = total_minutes - elapsed_minutes;
    if (schedule_remaining_minutes < 0) schedule_remaining_minutes = 0;

    if (schedule_remaining_minutes != schedule_last_displayed_minutes) {
        schedule_last_displayed_minutes = schedule_remaining_minutes;
        update_schedule_3_ui();
    }

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

    static unsigned long last_dispense_time = 0;
    static bool dispense_in_progress = false;

    if (!g_motor_job.active && current_treat_index < total_scheduled_treats) {
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
extern "C" void init_audio() {
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_voltage(DAC_CHANNEL_2, 0);
    Serial.println("DAC audio initialized on GPIO 26 (DAC_CHANNEL_2)");
}

// ---------------------------
// Motor/IR control
// ---------------------------
extern "C" void full_stop() {
    setPCF8574Pin(PIN_MOTOR_IN1, false);
    setPCF8574Pin(PIN_MOTOR_IN2, false);

    led_set_solid(false);
    setPCF8574Pin(PIN_IR_TX, true);

    Serial.println("Full stop: Motor and LED OFF");
}

extern "C" void Motor_Start() {
    setPCF8574Pin(PIN_MOTOR_IN1, true);
    setPCF8574Pin(PIN_MOTOR_IN2, false);
    Serial.println("Motor ON (CW)");
}

static bool beam_initial_state = true;
static inline bool read_beam() { return readPCF8574Pin(PIN_IR_RX); }

extern "C" void IR_Start() {
    led_set_solid(true);
    setPCF8574Pin(PIN_IR_TX, false);
    beam_initial_state = read_beam();
    Serial.print("IR Start requested - initial beam: ");
    Serial.println(beam_initial_state ? "HIGH" : "LOW");
}

extern "C" void IR_Stop() {
    led_set_solid(false);
    setPCF8574Pin(PIN_IR_TX, true);
    Serial.println("IR transmitter OFF");
}

// ---------------------------
// Standalone foot-switch training tick
// ---------------------------
static void footswitch_train_tick(lv_timer_t* timer) {
    (void)timer;
    const unsigned long now = millis();

    if (!foot_train_active) {
        led_set_solid(false);
        if (foot_train_timer) {
            lv_timer_del(foot_train_timer);
            foot_train_timer = NULL;
        }
        return;
    }

    setPCF8574Pin(PIN_MOTOR_IN1, false);
    setPCF8574Pin(PIN_MOTOR_IN2, false);

    led_set_solid(true);

    if (now - foot_train_last_tone_ms >= 5000UL) {
        foot_train_last_tone_ms = now;
        audio_play_tone_1s();
    }

    if (now < foot_train_armed_after_ms) {
        return;
    }

    if (now - foot_train_start_ms >= 20000UL) {
        Serial.println("Foot-switch training: TIMEOUT (no treat dispensed).");
        foot_train_active = false;
        cancel_footswitch_training_window();
        return;
    }

    if (footswitch_pressed_debounced(now) && !g_motor_job.active) {
        Serial.println("Foot-switch training: PRESSED -> dispensing 1 treat.");

        foot_train_active = false;
        if (foot_train_timer) {
            lv_timer_del(foot_train_timer);
            foot_train_timer = NULL;
        }

        led_set_solid(true);

        start_async_motor_job(TRAIN_MOTOR_RUN_MS,
                              millis(),
                              0UL,
                              nullptr,
                              training_motor_done_cb);
    }
}

// ---------------------------
// Legacy button training state machine
// ---------------------------
static void train_dispense_tick(lv_timer_t * timer) {
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
        case 0: {
            led_set_solid(true);
            static bool audio0 = false;
            if (!audio0) { audio_play_tone_1s(); audio0 = true; }

            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
                audio0 = false;
            } else if (now - state_start_time > 5000UL) {
                train_dispense_state = 1;
                state_start_time = now;
                audio0 = false;
            }
            break;
        }

        case 1: {
            led_set_solid(false);
            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
            } else if (now - state_start_time > 2000UL) {
                train_dispense_state = 2;
                state_start_time = now;
            }
            break;
        }

        case 2: {
            led_set_solid(true);
            static bool audio2 = false;
            if (!audio2) { audio_play_tone_1s(); audio2 = true; }

            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
                audio2 = false;
            } else if (now - state_start_time > 5000UL) {
                train_dispense_state = 3;
                state_start_time = now;
                last_blink = now;
                led_blink_mode = true;
                led_blink_state = false;
                audio2 = false;
            }
            break;
        }

        case 3: {
            static bool audiob = false;
            if (!audiob) { audio_play_tone_1s(); audiob = true; }

            if (edge_pressed) {
                train_dispense_state = 10;
                state_start_time = now;
                led_blink_mode = false;
                audiob = false;
            } else if (now - state_start_time > 5000UL) {
                train_dispense_state = 99;
                led_blink_mode = false;
                led_set_solid(false);
                audiob = false;
            } else {
                led_blink_tick(now);
            }
            break;
        }

        case 10: {
            if (g_motor_job.active) break;

            led_blink_mode = false;
            led_set_solid(true);

            legacy_train_job_done = false;

            start_async_motor_job(TRAIN_MOTOR_RUN_MS,
                                  millis(),
                                  0UL,
                                  &train_dispense_stop_requested,
                                  legacy_train_done_cb);

            train_dispense_state = 11;
            break;
        }

        case 11: {
            if (legacy_train_job_done) {
                Serial.print("Train stop reason: ");
                Serial.println((int)legacy_train_reason);
                train_dispense_state = 99;
            }
            break;
        }

        case 99: {
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
extern "C" void actions_init() {
    Wire.setClock(400000);
    ensure_remote_poll_timer_running();
    Serial.printf("Current config: ZERO=%.3fV SENS=%.3fV/A AVG=%d FILTER_ALPHA=%.2f NO_MOTION_START=%lu NO_MOTION_TIMEOUT=%lu IFILT_JAM=%.2f IFILT_CONFIRM=%lu\r\n",
                  ZERO_CURRENT_VOLTAGE,
                  SENSITIVITY,
                  (int)CURRENT_SENSOR_AVG_SAMPLES,
                  (float)CURRENT_FILTER_ALPHA,
                  (unsigned long)JAM_NO_MOTION_STARTUP_MS,
                  (unsigned long)JAM_NO_MOTION_TIMEOUT_MS,
                  (float)JAM_FILTERED_THRESHOLD_AMPS,
                  (unsigned long)JAM_FILTERED_CONFIRM_MS);
}

// Manual treat behavior
extern "C" void action_manual_dispense_treat(lv_event_t * e) {
    (void)e;

    if (g_motor_job.active) {
        Serial.println("Manual dispense ignored: motor job already active.");
        return;
    }

    Serial.println("\n=== Manual Treat Dispense Started ===");

    const unsigned long led_on_start = millis();
    led_set_solid(true);
    audio_play_tone_1s();

    start_async_motor_job(TRAIN_MOTOR_RUN_MS,
                          led_on_start,
                          5000UL,
                          nullptr,
                          manual_dispense_done_cb);
}

extern "C" void action_train_dispense_treat(lv_event_t * e) {
    (void)e;
    Serial.println("=== Training Mode START (footswitch window) ===");
    ensure_remote_poll_timer_running();
    start_footswitch_training_window();
}

extern "C" void action_train_dispense_stop(lv_event_t * e) {
    (void)e;
    Serial.println("=== Training Mode STOP requested ===");
    cancel_footswitch_training_window();
    train_dispense_stop_requested = true;
    full_stop();
}

extern "C" void action_train_footswitch_dispense_start(lv_event_t* e) {
    (void)e;
    start_footswitch_training_window();
}

extern "C" void action_schedule_add_treat_num(lv_event_t * e) {
    (void)e;
    int idx = lv_roller_get_selected(objects.schedule_1_treatsnumber);
    selected_treats_number = idx + 1;
    Serial.print("Treats to dispense selected: ");
    Serial.println(selected_treats_number);
    update_schedule_3_ui();
}

extern "C" void action_schedule_add_hours(lv_event_t * e) {
    (void)e;
    int idx = lv_roller_get_selected(objects.schedule_2_hours_to_dispense);
    selected_hours_to_dispense = idx + 1;
    Serial.print("Hours to dispense selected: ");
    Serial.println(selected_hours_to_dispense);
    update_schedule_3_ui();
}

extern "C" void action_schedule_2_next(lv_event_t * e) {
    (void)e;
    Serial.println("Transitioning to Schedule 3 screen");
    Serial.printf("Current values: treats=%d, hours=%d\n", selected_treats_number, selected_hours_to_dispense);
}

extern "C" void action_scheduletreatdispensestart(lv_event_t * e) {
    (void)e;
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
        schedule_last_displayed_minutes = -9999;
        update_schedule_3_ui();

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

extern "C" void action_scheduletreatdispensepause(lv_event_t * e) {
    (void)e;
    if (schedule_is_running && !schedule_is_paused) {
        schedule_is_paused = true;
        schedule_pause_time = millis();
        full_stop();
        Serial.println("=== Schedule Dispense PAUSED ===");
    } else if (schedule_is_paused) {
        action_scheduletreatdispensestart(e);
    }
}

extern "C" void action_scheduletreatdispensestop(lv_event_t * e) {
    (void)e;
    Serial.println("=== Schedule Dispense STOPPED ===");

    if (g_motor_job.active) {
        schedule_stop_requested = true;
    }

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