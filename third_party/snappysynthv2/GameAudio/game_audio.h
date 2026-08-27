#ifndef SS_GAME_AUDIO_H
#define SS_GAME_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

// SnappySynthV2's voice mixer optionally mixes the unrelated GameAudio layer.
// WASMIDI does not expose that API, so only these two symbols are retained.
int SS_Game_HasActiveSources(void);
void ss_game_mix_float(float* out, int frames, int channels, int sample_rate);

#ifdef __cplusplus
}
#endif

#endif
