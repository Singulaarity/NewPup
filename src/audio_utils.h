#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);

// Non-blocking: returns immediately; audio runs in background task
void audio_play_tone(uint16_t frequency_hz, uint8_t amplitude, uint32_t duration_ms);
void audio_play_tone_1s(void);

bool audio_is_playing(void);
void audio_stop(void);

#ifdef __cplusplus
}
#endif
