#include "audio_utils.h"
#include <Arduino.h>
#include "driver/dac.h"

// --------- Command/state shared with audio task ----------
static volatile bool     g_playing = false;
static volatile uint16_t g_freq_hz  = 400;
static volatile uint8_t  g_amp      = 200;
static volatile uint32_t g_end_ms   = 0;

static TaskHandle_t g_audioTaskHandle = nullptr;

// Audio task: runs on background core, toggles DAC without blocking your UI/main
static void audio_task(void* /*param*/) {
    // Enable DAC once
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_voltage(DAC_CHANNEL_2, 0);

    bool high = false;

    for (;;) {
        // Sleep until we have something to do
        if (!g_playing) {
            dac_output_voltage(DAC_CHANNEL_2, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Tone parameters (copy volatile -> local)
        uint16_t freq = g_freq_hz;
        uint8_t  amp  = g_amp;

        if (freq == 0) {
            g_playing = false;
            dac_output_voltage(DAC_CHANNEL_2, 0);
            continue;
        }

        uint32_t half_period_us = (1000000UL / (uint32_t)freq) / 2UL;
        if (half_period_us == 0) {
            // Too high for this method; stop safely
            g_playing = false;
            dac_output_voltage(DAC_CHANNEL_2, 0);
            continue;
        }

        // Stop check (millis wrap safe)
        if ((int32_t)(millis() - g_end_ms) >= 0) {
            g_playing = false;
            dac_output_voltage(DAC_CHANNEL_2, 0);
            continue;
        }

        // Toggle output (square wave)
        high = !high;
        dac_output_voltage(DAC_CHANNEL_2, high ? amp : 0);

        // Precise-ish delay in task context (won't freeze UI)
        ets_delay_us(half_period_us);

        // Yield occasionally so we don't hog CPU
        // (every ~5ms worth of toggles)
        static uint32_t lastYield = 0;
        uint32_t now = millis();
        if ((uint32_t)(now - lastYield) >= 5) {
            lastYield = now;
            taskYIELD();
        }
    }
}

void audio_init(void) {
    if (g_audioTaskHandle) return;

    // Create task on the OTHER core than Arduino loop typically runs on.
    // Arduino loop is usually core 1; pin audio to core 0 to keep UI responsive.
    xTaskCreatePinnedToCore(
        audio_task,
        "audio_task",
        2048,
        nullptr,
        1,              // low priority
        &g_audioTaskHandle,
        0               // core 0
    );
}

void audio_play_tone(uint16_t frequency_hz, uint8_t amplitude, uint32_t duration_ms) {
    if (!g_audioTaskHandle) audio_init();

    if (duration_ms == 0 || frequency_hz == 0 || amplitude == 0) {
        audio_stop();
        return;
    }

    g_freq_hz  = frequency_hz;
    g_amp      = amplitude;
    g_end_ms   = millis() + duration_ms;
    g_playing  = true;
}

void audio_play_tone_1s(void) {
    audio_play_tone(g_freq_hz, g_amp, 1000);
}

bool audio_is_playing(void) {
    return g_playing;
}

void audio_stop(void) {
    g_playing = false;
    // Task will force DAC to 0 on its next loop
}
