#ifndef BUILDCONFIG_H
#define BUILDCONFIG_H
#include "compat/win_compat.h"
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#endif

// Build configuration options--------------------------------------------------------------------------------

// General config

// Voice configuration - if FORCED_VOICE_CAP is defined, it overrides config file
// If not defined, voice count will be loaded from cfg file (MaxVoices=)
// #define FORCED_VOICE_CAP 65535

// Default voice count if not specified in config
#define DEFAULT_VOICE_COUNT 2048
// Enables general logging
/* DEBUG disabled in browser core */

// Enables extensive voice debug logging (SLOW AND GENERATES LOTS OF LOGS)
#ifdef DEBUG 
//#define VOICEDEBUG
#endif

// Voice Overlap Remover modes:
// 0 = merge exact overlaps into one louder voice, then peel gain down on note-offs.
// 1 = merge exact overlaps without loudness stacking; the surviving voice stays one-voice loud.
#define VOR 1

// Optional runtime-loaded GPU voice mixer. CPU-only systems keep the normal fast path.
#if defined(_WIN32) && !defined(LEGACY_XP) && !defined(LEGACY_NT4) && !defined(LEGACY_WIN95)
#define GPU_MIX
#endif

#if defined(GPU_MIX) && !defined(GPUMIX)
#define GPUMIX
#endif

// Audio output config
// Windows Audio API selection values:
// 0 = WinMM
// 1 = WASAPI (Not implemented)
// 2 = ASIO (Not implemented)
// 3 = DirectSound
// If defined, overrides config file AudioAPI= setting
// #define FORCE_AUDIO_API 3

// Universal Audio Configuration
typedef struct {
    int sample_rate;
    int num_channels;
    int bits_per_sample;
    int buffer_size;      // frames per buffer
    int num_buffers;      // number of buffers in pool
    int realtime_priority; // 1 = use high priority
} AudioConfig;

// Default to very low latency settings
#define DEFAULT_AUDIO_CONFIG { \
    .sample_rate = 44100,      \
    .num_channels = 2,         \
    .bits_per_sample = 32,     \
    .buffer_size = 480,         \
    .num_buffers = 12,          \
    .realtime_priority = 1     \
}

// Add reload notification support
typedef void (*config_change_callback)(void);

// Build configuration options--------------------------------------------------------------------------------
#endif // BUILDCONFIG_H
