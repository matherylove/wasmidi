#include "buildconfig.h"
#include "Voice/voice.h"
#include "Parser/sf2_parser.h"
#include "Parser/sfz_parser.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sfz_instrument* instrument = NULL;

static AudioConfig g_cfg = {44100, 2, 32, 512, 16, 1};
static float* g_out = NULL;
static int g_out_capacity_frames = 0;
static int64_t g_render_cursor = 1;
static int g_ready = 0;
static int g_max_voices = 16384;
static int g_min_voices = 0;
static int g_soundfont_layers = 0;

/* Source-exposed runtime/performance settings. */
static int g_requested_workers = 0;       /* 0 = original auto policy */
static int g_note_sharding = 0;           /* 0 auto, 1 channel, 2 hash */
static int g_steal_score_cache = 1;       /* SS_STEAL_SCORE_CACHE default */
static int g_fast_note_off = 1;           /* SS_FAST_NOTE_OFF default */
static int g_validate_state = 0;           /* SS_VALIDATE_STATE default off */
static int g_soft_clip = 1;                /* realtime source default on */

static void ssw_set_env_int(const char* name, int enabled, int value) {
    char buffer[32];
    if (!name || !*name) return;
    if (!enabled) {
        unsetenv(name);
        return;
    }
    snprintf(buffer, sizeof(buffer), "%d", value);
    setenv(name, buffer, 1);
}

static void ssw_apply_source_runtime_options(void) {
    ssw_set_env_int("SS_WORKERS", g_requested_workers > 0, g_requested_workers);

    if (g_note_sharding == 1) setenv("SS_NOTE_SHARDING", "channel", 1);
    else if (g_note_sharding == 2) setenv("SS_NOTE_SHARDING", "hash", 1);
    else unsetenv("SS_NOTE_SHARDING");

    setenv("SS_STEAL_SCORE_CACHE", g_steal_score_cache ? "1" : "0", 1);
    setenv("SS_FAST_NOTE_OFF", g_fast_note_off ? "1" : "0", 1);

    if (g_validate_state) setenv("SS_VALIDATE_STATE", "1", 1);
    else unsetenv("SS_VALIDATE_STATE");
}

/* ---- Exact SnappySynthV2 soundfont stacking policy ---- */
static int region_selector_is_wildcard(const sfz_region* region) {
    return region && region->midi_bank < 0 && region->midi_program < 0;
}

static int region_selector_matches(const sfz_region* a, const sfz_region* b) {
    if (!a || !b) return 0;
    if (region_selector_is_wildcard(a) || region_selector_is_wildcard(b))
        return region_selector_is_wildcard(a) && region_selector_is_wildcard(b);
    return a->midi_bank == b->midi_bank && a->midi_program == b->midi_program;
}

static int region_is_melodic_program(const sfz_region* region) {
    if (!region || region->midi_is_drum > 0) return 0;
    return region->midi_program >= 0;
}

static int region_is_default_melodic_program(const sfz_region* region) {
    if (!region_is_melodic_program(region)) return 0;
    if (region->midi_program != 0) return 0;
    return region->midi_bank < 0 || region->midi_bank == 0;
}

static int region_keyvel_overlaps(const sfz_region* a, const sfz_region* b) {
    if (!a || !b) return 0;
    if (a->hikey < b->lokey || b->hikey < a->lokey) return 0;
    if (a->hivel < b->lovel || b->hivel < a->lovel) return 0;
    return 1;
}

static int region_is_overridden_by_incoming(const sfz_region* existing,
                                             const sfz_instrument* incoming) {
    if (!existing || !incoming) return 0;
    for (int i = 0; i < incoming->num_regions; ++i) {
        const sfz_region* r = &incoming->regions[i];
        if (region_selector_matches(existing, r)) return 1;
        if (incoming->format == SOUND_FONT_FORMAT_SFZ &&
            region_selector_is_wildcard(r) &&
            region_is_default_melodic_program(existing) &&
            region_keyvel_overlaps(existing, r)) return 1;
    }
    return 0;
}

static void remove_overridden_regions(sfz_instrument* existing,
                                      const sfz_instrument* incoming) {
    int write_index = 0;
    if (!existing || !incoming || existing->num_regions <= 0 || incoming->num_regions <= 0)
        return;
    for (int i = 0; i < existing->num_regions; ++i) {
        sfz_region* region = &existing->regions[i];
        if (region_is_overridden_by_incoming(region, incoming)) {
            free(region->opcodes);
            region->opcodes = NULL;
            region->opcode_count = 0;
            region->opcode_cap = 0;
            continue;
        }
        if (write_index != i) existing->regions[write_index] = existing->regions[i];
        ++write_index;
    }
    existing->num_regions = write_index;
    sfz_invalidate_region_cache(existing);
}

static int append_instrument_regions(sfz_instrument* existing, sfz_instrument* incoming) {
    if (!existing || !incoming || incoming->num_regions <= 0) return 1;
    int old_count = existing->num_regions;
    int total = old_count + incoming->num_regions;
    sfz_region* regions = (sfz_region*)realloc(existing->regions,
                                                (size_t)total * sizeof(sfz_region));
    if (!regions) return 0;
    existing->regions = regions;
    memcpy(existing->regions + old_count, incoming->regions,
           (size_t)incoming->num_regions * sizeof(sfz_region));
    existing->num_regions = total;
    sfz_invalidate_region_cache(existing);
    return 1;
}

/* ---- Exact SnappySynthV2 GS/XG/GM short-message routing and SysEx state ---- */
static volatile LONG g_gs_part_map_active = 0;
static int g_gs_part_to_channel[16] = { 9,0,1,2,3,4,5,6,7,8,10,11,12,13,14,15 };
static int g_gs_channel_part_count[16] = {0};
static int g_gs_channel_parts[16][16] = {{0}};

static void gs_rebuild_channel_parts(void) {
    for (int ch = 0; ch < 16; ++ch) g_gs_channel_part_count[ch] = 0;
    for (int part = 0; part < 16; ++part) {
        int ch = g_gs_part_to_channel[part];
        if (ch < 0 || ch >= 16) continue;
        int n = g_gs_channel_part_count[ch];
        if (n < 16) g_gs_channel_parts[ch][n] = part;
        if (n < 16) g_gs_channel_part_count[ch] = n + 1;
    }
}

static void gs_reset_part_map(int active) {
    static const int defaults[16] = { 9,0,1,2,3,4,5,6,7,8,10,11,12,13,14,15 };
    for (int i = 0; i < 16; ++i) g_gs_part_to_channel[i] = defaults[i];
    gs_rebuild_channel_parts();
    voice_set_gs_part_mode(active);
    InterlockedExchange(&g_gs_part_map_active, active ? 1 : 0);
}

static int gs_part_addr_to_part_index(int part_addr) { return part_addr & 0x0F; }

static void dispatch_midi_part_event_at_qpc(int part, int cmd, int b1, int b2,
                                             int stack_count, int64_t timestamp_qpc) {
    if ((unsigned int)part >= 16) return;
    switch (cmd) {
        case 0x90:
            if (b2 > 0) voice_send_note_event_at(part, b1, b2, 1, stack_count, timestamp_qpc);
            else voice_send_note_event_at(part, b1, 0, 0, 1, timestamp_qpc);
            break;
        case 0x80: voice_send_note_event_at(part, b1, 0, 0, 1, timestamp_qpc); break;
        case 0xB0: voice_control_change_at(part, b1, b2, timestamp_qpc); break;
        case 0xC0: voice_program_change_at(part, b1, timestamp_qpc); break;
        case 0xE0: voice_pitch_bend_at(part, b1, b2, timestamp_qpc); break;
        default: break;
    }
}

static void sysex_normalize_payload(const unsigned char** data, int* length) {
    if (!data || !length || !*data || *length <= 0) return;
    if ((*data)[0] == 0xF0) { ++(*data); --(*length); }
    if (*length > 0 && (*data)[*length - 1] == 0xF7) --(*length);
}

static int sysex_matches_gm_reset(const unsigned char* data, int length) {
    if (!data || length < 4) return 0;
    sysex_normalize_payload(&data, &length);
    return length >= 4 && data[0] == 0x7E && data[2] == 0x09 &&
           (data[3] == 0x01 || data[3] == 0x03);
}

static int sysex_matches_gs_reset(const unsigned char* data, int length) {
    if (!data || length < 9) return 0;
    sysex_normalize_payload(&data, &length);
    return length >= 9 && data[0] == 0x41 && data[2] == 0x42 && data[3] == 0x12 &&
           data[4] == 0x40 && data[5] == 0x00 && data[6] == 0x7F && data[7] == 0x00;
}

static int sysex_matches_xg_reset(const unsigned char* data, int length) {
    if (!data || length < 7) return 0;
    sysex_normalize_payload(&data, &length);
    return length >= 7 && data[0] == 0x43 && data[2] == 0x4C && data[3] == 0x00 &&
           data[4] == 0x00 && data[5] == 0x7E && data[6] == 0x00;
}

static int sysex_get_universal_master_volume_14bit(const unsigned char* data, int length,
                                                    int* out_value14) {
    if (!data || !out_value14 || length < 6) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 6 || data[0] != 0x7F || data[2] != 0x04 || data[3] != 0x01) return 0;
    *out_value14 = (((int)data[5] & 0x7F) << 7) | ((int)data[4] & 0x7F);
    return 1;
}

static int sysex_get_gs_drum_part(const unsigned char* data, int length,
                                  int* out_part, int* out_mode) {
    int addr;
    if (!data || !out_part || !out_mode || length < 9) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 9 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12 ||
        data[4] != 0x40 || data[6] != 0x15) return 0;
    addr = (int)data[5] & 0x7F;
    if ((addr & 0x70) != 0x10) return 0;
    *out_part = gs_part_addr_to_part_index(addr);
    if (*out_part < 0 || *out_part >= 16) return 0;
    *out_mode = (int)data[7] & 0x7F;
    return 1;
}

static int sysex_get_gs_receive_channel(const unsigned char* data, int length,
                                        int* out_part, int* out_channel) {
    int addr;
    if (!data || !out_part || !out_channel || length < 9) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 9 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12 ||
        data[4] != 0x40 || data[6] != 0x02) return 0;
    addr = (int)data[5] & 0x7F;
    if ((addr & 0x70) != 0x10) return 0;
    *out_part = gs_part_addr_to_part_index(addr);
    if (data[7] <= 0x0F) *out_channel = (int)data[7];
    else if (data[7] == 0x10) *out_channel = -1;
    else return 0;
    return *out_part >= 0 && *out_part < 16;
}

static int sysex_apply_gs_scale_tuning(const unsigned char* data, int length,
                                       int64_t timestamp_qpc) {
    int addr, part, count;
    if (!data || length < 10) return 0;
    sysex_normalize_payload(&data, &length);
    if (length < 10 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12 ||
        data[4] != 0x40 || data[6] != 0x40) return 0;
    addr = (int)data[5] & 0x7F;
    if ((addr & 0x70) != 0x10) return 0;
    part = gs_part_addr_to_part_index(addr);
    if (part < 0 || part >= 16) return 0;
    count = length - 8;
    if (count > 12) count = 12;
    for (int i = 0; i < count; ++i)
        voice_set_scale_tune_at(part, i, ((int)data[7 + i] & 0x7F) - 64, timestamp_qpc);
    return 1;
}

static void apply_gm_reset_at_qpc(int64_t timestamp_qpc) {
    for (int ch = 0; ch < 16; ++ch) {
        voice_control_change_at(ch, 120, 0, timestamp_qpc);
        voice_control_change_at(ch, 121, 0, timestamp_qpc);
        voice_control_change_at(ch, 123, 0, timestamp_qpc);
        voice_control_change_at(ch, 0, 0, timestamp_qpc);
        voice_control_change_at(ch, 32, 0, timestamp_qpc);
        voice_program_change_at(ch, 0, timestamp_qpc);
        voice_pitch_bend_at(ch, 0, 64, timestamp_qpc);
        voice_set_drum_part_at(ch, 0, timestamp_qpc);
    }
    voice_set_master_volume_14bit_at(0x3FFF, timestamp_qpc);
}

static void dispatch_sysex_data_at_qpc(const unsigned char* data, int length,
                                       int64_t timestamp_qpc) {
    int master, part, mode, recv_part, recv_channel;
    if (!data || length <= 0) return;
    if (sysex_matches_gm_reset(data, length)) {
        gs_reset_part_map(0); apply_gm_reset_at_qpc(timestamp_qpc); return;
    }
    if (sysex_matches_gs_reset(data, length)) {
        gs_reset_part_map(1); apply_gm_reset_at_qpc(timestamp_qpc);
        voice_set_drum_part_at(0, 1, timestamp_qpc); return;
    }
    if (sysex_matches_xg_reset(data, length)) {
        gs_reset_part_map(0); apply_gm_reset_at_qpc(timestamp_qpc); return;
    }
    if (sysex_get_universal_master_volume_14bit(data, length, &master)) {
        voice_set_master_volume_14bit_at(master, timestamp_qpc); return;
    }
    if (sysex_apply_gs_scale_tuning(data, length, timestamp_qpc)) return;
    if (sysex_get_gs_receive_channel(data, length, &recv_part, &recv_channel)) {
        g_gs_part_to_channel[recv_part] = recv_channel;
        gs_rebuild_channel_parts();
        InterlockedExchange(&g_gs_part_map_active, 1);
        return;
    }
    if (sysex_get_gs_drum_part(data, length, &part, &mode)) {
        voice_set_drum_part_at(part, mode, timestamp_qpc);
        return;
    }
}

static void dispatch_short_at_qpc(uint32_t msg, int64_t timestamp_qpc) {
    unsigned status = msg & 0xFFu;
    int cmd = (int)(status & 0xF0u);
    int ch = (int)(status & 0x0Fu);
    int b1 = (int)((msg >> 8) & 0x7Fu);
    int b2 = (int)((msg >> 16) & 0x7Fu);
    int stack_count = (int)((msg >> 24) & 0xFFu) + 1;

    if (g_gs_part_map_active) {
        int count = g_gs_channel_part_count[ch];
        for (int i = 0; i < count; ++i)
            dispatch_midi_part_event_at_qpc(g_gs_channel_parts[ch][i], cmd, b1, b2,
                                             stack_count, timestamp_qpc);
        return;
    }

    switch (cmd) {
        case 0x90:
            if (b2 > 0) voice_send_note_event_at(ch, b1, b2, 1, stack_count, timestamp_qpc);
            else voice_send_note_event_at(ch, b1, 0, 0, 1, timestamp_qpc);
            break;
        case 0x80: voice_send_note_event_at(ch, b1, 0, 0, 1, timestamp_qpc); break;
        case 0xB0: voice_control_change_at(ch, b1, b2, timestamp_qpc); break;
        case 0xC0: voice_program_change_at(ch, b1, timestamp_qpc); break;
        case 0xE0: voice_pitch_bend_at(ch, b1, b2, timestamp_qpc); break;
        default: break;
    }
}

int ssw_init_ex(int sample_rate,
                int channels,
                int bits_per_sample,
                int block_frames,
                int num_buffers,
                int realtime_priority,
                int max_voices,
                int min_voices,
                int workers,
                int note_sharding,
                int steal_score_cache,
                int fast_note_off,
                int validate_state,
                int soft_clip) {
    if (g_ready) voice_shutdown();

    g_cfg.sample_rate = sample_rate > 0 ? sample_rate : 44100;
    g_cfg.num_channels = channels > 0 ? channels : 2;
    if (g_cfg.num_channels > 2) g_cfg.num_channels = 2;

    g_cfg.bits_per_sample =
        bits_per_sample > 0 ? bits_per_sample : 32;

    g_cfg.buffer_size =
        block_frames > 0 ? block_frames : 480;

    g_cfg.num_buffers =
        num_buffers > 0 ? num_buffers : 16;

    g_cfg.realtime_priority =
        realtime_priority ? 1 : 0;

    g_max_voices =
        max_voices > 0 ? max_voices : 16384;

    g_min_voices =
        min_voices > 0 ? min_voices : 0;

    if (g_min_voices > g_max_voices)
        g_max_voices = g_min_voices;

    g_requested_workers =
        workers > 0 ? workers : 0;

    g_note_sharding =
        note_sharding >= 1 && note_sharding <= 2
            ? note_sharding
            : 0;

    g_steal_score_cache =
        steal_score_cache ? 1 : 0;

    g_fast_note_off =
        fast_note_off ? 1 : 0;

    g_validate_state =
        validate_state ? 1 : 0;

    g_soft_clip =
        soft_clip ? 1 : 0;

    ssw_apply_source_runtime_options();

    /*
     * This is the supplied SnappySynthV2 voice_init_with_count() unchanged.
     * It owns its freelists, sampled/O(1) steal paths, VOR state, pressure
     * thresholds, channel/hash sharding and internal pthread workers.
     */
    voice_init_with_count(
        g_max_voices,
        &g_cfg);

    voice_set_output_soft_clip_enabled(
        g_soft_clip);

    gs_reset_part_map(0);

    if (instrument)
        sfz_apply_presampling(
            instrument,
            g_cfg.sample_rate);

    g_render_cursor = 1;
    g_ready = 1;
    return 1;
}

int ssw_init(int sample_rate, int channels, int block_frames, int max_voices) {
    return ssw_init_ex(
        sample_rate,
        channels,
        32,
        block_frames,
        16,
        1,
        max_voices,
        0,
        0,
        0,
        1,
        1,
        0,
        1);
}

int ssw_load_sf2(const char* path) {
    if (!g_ready || !path || !*path) return 0;
    sfz_instrument* next = sf2_load_as_instrument(path);
    if (!next) return 0;

    if (!instrument) {
        instrument = next;
    } else {
        remove_overridden_regions(instrument, next);
        if (!append_instrument_regions(instrument, next)) {
            sfz_free(next);
            return 0;
        }
        sfz_transfer_sample_cache(instrument, next);
        if (next->format == SOUND_FONT_FORMAT_SF2 ||
            instrument->format == SOUND_FONT_FORMAT_UNKNOWN ||
            instrument->format == SOUND_FONT_FORMAT_SFZ)
            instrument->format = next->format;
        strncpy(instrument->source_path, next->source_path,
                sizeof(instrument->source_path) - 1);
        instrument->source_path[sizeof(instrument->source_path) - 1] = '\0';
        free(next->regions);
        next->regions = NULL;
        next->num_regions = 0;
        free(next);
    }

    ++g_soundfont_layers;
    sfz_apply_presampling(instrument, g_cfg.sample_rate);
    return instrument->num_regions;
}

void ssw_clear_soundfonts(void) {
    if (g_ready) {
        voice_shutdown();
        g_ready = 0;
    }
    if (instrument) { sfz_free(instrument); instrument = NULL; }
    g_soundfont_layers = 0;
    ssw_init_ex(
        g_cfg.sample_rate,
        g_cfg.num_channels,
        g_cfg.bits_per_sample,
        g_cfg.buffer_size,
        g_cfg.num_buffers,
        g_cfg.realtime_priority,
        g_max_voices,
        g_min_voices,
        g_requested_workers,
        g_note_sharding,
        g_steal_score_cache,
        g_fast_note_off,
        g_validate_state,
        g_soft_clip);
}

void ssw_set_volume(float linear) {
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 1.0f) linear = 1.0f;
    voice_set_master_volume_14bit((int)(linear * 16383.0f + 0.5f));
}

void ssw_set_vor_mode(int mode) { voice_set_vor_volume_mode(mode ? 1 : 0); }

void ssw_set_soft_clip(int enabled) {
    g_soft_clip = enabled ? 1 : 0;
    if (g_ready)
        voice_set_output_soft_clip_enabled(g_soft_clip);
}

void ssw_reset(void) {
    if (!g_ready) return;
    gs_reset_part_map(0);
    apply_gm_reset_at_qpc(g_render_cursor);
    g_render_cursor = 1;
}

void ssw_send_sysex(const uint8_t* data, int length, uint32_t offset) {
    if (!g_ready || !data || length <= 0) return;
    dispatch_sysex_data_at_qpc(data, length, g_render_cursor + (int64_t)offset);
}

static int ssw_render_to_buffer(float* out_buffer,
                                const uint32_t* messages,
                                const uint32_t* offsets,
                                int count,
                                int frames) {
    if (!g_ready || !out_buffer || frames <= 0)
        return 0;

    voice_set_render_timing(g_render_cursor, g_cfg.sample_rate);
    for (int i = 0; i < count; ++i) {
        uint32_t offset = offsets ? offsets[i] : 0u;
        if (offset >= (uint32_t)frames)
            offset = (uint32_t)(frames - 1);
        dispatch_short_at_qpc(
            messages[i],
            g_render_cursor + (int64_t)offset);
    }

    memset(out_buffer,
           0,
           sizeof(float) * (size_t)frames * (size_t)g_cfg.num_channels);
    voice_render_float(out_buffer, frames);
    g_render_cursor += frames;
    return 1;
}

uintptr_t ssw_render(const uint32_t* messages, const uint32_t* offsets,
                     int count, int frames) {
    if (!g_ready || frames <= 0)
        return 0;

    if (frames > g_out_capacity_frames) {
        float* next = (float*)realloc(
            g_out,
            sizeof(float) * (size_t)frames * (size_t)g_cfg.num_channels);
        if (!next)
            return 0;
        g_out = next;
        g_out_capacity_frames = frames;
    }

    if (!ssw_render_to_buffer(g_out, messages, offsets, count, frames))
        return 0;

    return (uintptr_t)g_out;
}

/*
 * Browser hot path: render directly into a caller-owned region inside the
 * module's Shared WebAssembly.Memory.  The AudioWorklet can read that same
 * SharedArrayBuffer, eliminating the old Worker -> transferable ArrayBuffer
 * PCM copy without changing one DSP operation in voice_render_float().
 */
int ssw_render_into(uintptr_t out_ptr,
                    const uint32_t* messages,
                    const uint32_t* offsets,
                    int count,
                    int frames) {
    return ssw_render_to_buffer(
        (float*)out_ptr,
        messages,
        offsets,
        count,
        frames);
}

int ssw_active_voices(void) { return GetVoiceStats().active_voices; }
int ssw_free_voices(void) { return GetVoiceStats().free_voices; }
int ssw_steals(void) {
    long value = GetVoiceStats().steals;
    if (value < 0) return 0;
    return value > 2147483647L ? 2147483647 : (int)value;
}
int ssw_layer_count(void) { return g_soundfont_layers; }
int ssw_region_count(void) { return instrument ? instrument->num_regions : 0; }
int ssw_channels(void) { return g_cfg.num_channels; }
int ssw_sample_rate(void) { return g_cfg.sample_rate; }
int ssw_bits_per_sample(void) { return g_cfg.bits_per_sample; }
int ssw_num_buffers(void) { return g_cfg.num_buffers; }
int ssw_worker_count(void) { return voice_get_worker_count(); }

void ssw_shutdown(void) {
    if (g_ready) voice_shutdown();
    g_ready = 0;
    if (instrument) { sfz_free(instrument); instrument = NULL; }
    g_soundfont_layers = 0;
    free(g_out); g_out = NULL; g_out_capacity_frames = 0;
}
