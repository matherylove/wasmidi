#include "Debug/errorhandler.h"
#include "GameAudio/game_audio.h"

#include <stdio.h>

void ShowError(const char* error)
{
    if (error)
        fprintf(stderr, "SnappySynthV2: %s\n", error);
}

void ShowErrorWithLocation(const char* error, int line, const char* file)
{
    if (error)
        fprintf(stderr, "SnappySynthV2: %s (%s:%d)\n",
                error, file ? file : "?", line);
}

void ShowErrorWithMessage(const char* error, const char* message)
{
    fprintf(stderr, "SnappySynthV2: %s %s\n",
            error ? error : "", message ? message : "");
}

int SS_Game_HasActiveSources(void)
{
    return 0;
}

void ss_game_mix_float(float* out, int frames, int channels, int sample_rate)
{
    (void)out;
    (void)frames;
    (void)channels;
    (void)sample_rate;
}

void TerminateSynth(void)
{
    // Windows version tears down the realtime DLL backend here.
    // WASMIDI owns the worker lifetime explicitly.
}
