#ifndef WAV_LOADER_H
#define WAV_LOADER_H

typedef struct {
    short* data;
    int num_samples;
    int sample_rate;
    int num_channels;
    int root_key;
    float root_key_fraction;
    int has_loop;
    int loop_start;
    int loop_end;
} wav_data;

wav_data* wav_load(const char* path);
void wav_free(wav_data* data);

#endif // WAV_LOADER_H
