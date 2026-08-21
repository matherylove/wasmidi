#include "buildconfig.h"
#include "Voice/voice.h"
#include "Parser/sf2_parser.h"
#include "Parser/sfz_parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

sfz_instrument* instrument = NULL;

static AudioConfig g_cfg = {44100, 2, 32, 512, 8, 0};
static float* g_out = NULL;
static int g_out_capacity_frames = 0;
static int64_t g_render_cursor = 1;
static int g_ready = 0;

int ssw_init(int sample_rate, int channels, int block_frames, int max_voices)
{
    if (g_ready)
        voice_shutdown();

    g_cfg.sample_rate =
        sample_rate > 0 ? sample_rate : 44100;
    g_cfg.num_channels =
        channels > 0 ? channels : 2;
    g_cfg.bits_per_sample = 32;
    g_cfg.buffer_size =
        block_frames > 0 ? block_frames : 512;
    g_cfg.num_buffers = 8;
    g_cfg.realtime_priority = 0;

    voice_init_with_count(
        max_voices > 0 ? max_voices : 2048,
        &g_cfg);

    voice_set_output_soft_clip_enabled(0);

    g_render_cursor = 1;
    g_ready = 1;
    return 1;
}

int ssw_load_sf2(const char* path)
{
    if (!g_ready || !path || !*path)
        return 0;

    sfz_instrument* next =
        sf2_load_as_instrument(path);

    if (!next)
        return 0;

    if (instrument)
        sfz_free(instrument);

    instrument = next;
    sfz_apply_presampling(
        instrument,
        g_cfg.sample_rate);

    return instrument->num_regions;
}

void ssw_set_volume(float linear)
{
    if (linear < 0.0f)
        linear = 0.0f;
    if (linear > 1.0f)
        linear = 1.0f;

    voice_set_master_volume_14bit(
        (int)(linear * 16383.0f + 0.5f));
}

void ssw_set_vor_mode(int mode)
{
    voice_set_vor_volume_mode(mode ? 1 : 0);
}

void ssw_reset(void)
{
    if (!g_ready)
        return;

    // Reset controller/program state and kill every sounding voice.
    for (int ch = 0; ch < 16; ++ch) {
        voice_control_change(ch, 120, 0); // all sound off
        voice_control_change(ch, 121, 0); // reset controllers
        voice_control_change(ch, 123, 0); // all notes off
        voice_control_change(ch, 0, 0);
        voice_control_change(ch, 32, 0);
        voice_program_change(ch, 0);
        voice_pitch_bend(ch, 0, 64);
    }

    g_render_cursor = 1;
}

uintptr_t ssw_render(
    const uint32_t* messages,
    const uint32_t* offsets,
    int count,
    int frames)
{
    if (!g_ready || frames <= 0)
        return 0;

    if (frames > g_out_capacity_frames) {
        float* next =
            (float*)realloc(
                g_out,
                sizeof(float) *
                (size_t)frames *
                (size_t)g_cfg.num_channels);

        if (!next)
            return 0;

        g_out = next;
        g_out_capacity_frames = frames;
    }

    voice_set_render_timing(
        g_render_cursor,
        g_cfg.sample_rate);

    for (int i = 0; i < count; ++i) {
        uint32_t offset =
            offsets ? offsets[i] : 0u;

        if (offset >= (uint32_t)frames)
            offset = (uint32_t)(frames - 1);

        voice_send_short_at(
            messages[i],
            g_render_cursor +
            (int64_t)offset);
    }

    memset(
        g_out,
        0,
        sizeof(float) *
        (size_t)frames *
        (size_t)g_cfg.num_channels);

    voice_render_float(
        g_out,
        frames);

    g_render_cursor += frames;

    return (uintptr_t)g_out;
}

int ssw_active_voices(void)
{
    return GetVoiceStats().active_voices;
}

int ssw_channels(void)
{
    return g_cfg.num_channels;
}

int ssw_sample_rate(void)
{
    return g_cfg.sample_rate;
}

void ssw_shutdown(void)
{
    if (g_ready)
        voice_shutdown();

    g_ready = 0;

    if (instrument) {
        sfz_free(instrument);
        instrument = NULL;
    }

    free(g_out);
    g_out = NULL;
    g_out_capacity_frames = 0;
}
