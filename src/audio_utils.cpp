#include <AudioFileSourceSD.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include "audio_utils.h"

static AudioGeneratorWAV *wav = nullptr;
static AudioFileSourceSD *file = nullptr;
static AudioOutputI2S *out = nullptr;

extern "C" void play_treat_wav() {
    if (wav) {
        delete wav;
        wav = nullptr;
    }
    if (file) {
        delete file;
        file = nullptr;
    }
    if (out) {
        delete out;
        out = nullptr;
    }

    file = new AudioFileSourceSD("/treat.wav");
    out = new AudioOutputI2S();
    out->SetPinout(26, -1, -1);  // Use only DAC1 (GPIO 26), disable other pins

    wav = new AudioGeneratorWAV();
    if (!wav->begin(file, out)) {
        Serial.println("Error: Failed to start WAV playback.");
    } else {
        Serial.println("WAV playback started on GPIO 26.");
    }
}