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
- a grid-aligned, load-governed scheduling cell for controller automation
  (see `ssw_render_queued_into`). The native engine never splits a render on an
  event: `winmm_output.c` sizes its chunk from elapsed device time and `voice.c`
  applies CC/program/pitch state when the worker drains its channel queue at the
  start of that chunk. Only note-on/note-off carry a sample offset. The browser
  build may therefore render controller automation on a finer grid than the
  block when there is CPU headroom, but collapses to exactly one
  `voice_render_float()` per block under load, which is the native behaviour.
  Selector-only events (bank select, RPN/NRPN select, program change) never open
  a boundary, because the channel event queue is drained in submission order and
  a later note-on already observes the correct state
- a scalar VOR helper needed when AVX2 is unavailable
- minimal Windows compatibility types/stubs
- `snappy_wasm_core.c`, which exposes only init/SF2/render/reset/settings.

Known-bad approaches for this subset, do not reintroduce:

- Splitting the render at the exact sample of every state event. Dense bank/RPN
  or program-change material turned one 512-frame block into ~100
  `voice_render_float()` calls. The cost of that call is dominated by fixed
  per-call work, not by frames: two futex-backed worker barriers, an
  `InterlockedAdd` + `memcpy` rebuild of the global render queue, the per-voice
  setup chain paid in full for every active voice, and loss of the SIMD sustain
  paths (which need frames to be a multiple of 8 or 4). This produced stalls at
  a few hundred voices in passages that were not dense.
- Any boundary policy that is not hard-capped. The cap must hold regardless of
  event density, and it must be driven by measured load rather than by event
  count.
