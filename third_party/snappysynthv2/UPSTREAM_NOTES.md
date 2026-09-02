# SnappySynthV2 browser subset

This directory is adapted from the SnappySynthV2 source archive supplied by the
WASMIDI project owner for this port.

Only code required for SF2 playback is kept:
- SF2/SFZ/WAV parsing
- the SnappySynthV2 voice engine
- a small browser/WASM C ABI

The following upstream components are intentionally not integrated:
- KDMAPI / WinMM / MIDI Out
- DirectSound / Windows audio backends
- SnappySynth's MIDI-file parser and command-line renderer
- Configurator
- GameAudio API
- DLL/config-file/sflist plumbing

Browser-specific compatibility changes are limited to:
- portable include paths
- an Emscripten pthread pool with a bounded, voice-count-aware default worker set
- WASM SIMD128 equivalents for the eligible native AVX2 sustain paths
- a generation-safe aggregate pthread completion barrier
- bounded 64-frame scheduling cells for continuous CC/pitch automation, while
  discrete controllers and program changes remain sample-exact
- a scalar VOR helper needed when AVX2 is unavailable
- minimal Windows compatibility types/stubs
- `snappy_wasm_core.c`, which exposes only init/SF2/render/reset/settings.
