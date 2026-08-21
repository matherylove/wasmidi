# WASMIDI Pass 10 — SnappySynthV2 source-fidelity engine

This pass is based directly on the complete SnappySynthV2 archive supplied by
the WASMIDI owner. Windows/KDMAPI/device plumbing remains excluded, but synth
behavior is taken from the original source rather than approximated.

## Performance restored
- Voice engine worker policy restored from the original: `VOICE_WORKER_COUNT=0`
  (all logical cores / source's own low-cap reductions).
- Emscripten pthread pool uses `navigator.hardwareConcurrency`.
- Browser `GetSystemInfo()` returns Emscripten logical core count instead of 2.
- The original O(1)/sampled steal system remains untouched.
- UI now exposes real `VoiceStats.steals` and free voices.

## Max Voices
- User can type an exact voice count instead of cycling presets.
- Accepted source-compatible range: 1..5,000,000.
- Browser memory limits still apply at extreme settings.

## SoundFont layers / stack
The original `SnappySynth_LoadSoundfont()` behavior is restored:
- first SF2 becomes the base instrument;
- additional SF2 files are appended;
- later matching bank/program presets override earlier matching presets;
- non-conflicting presets remain available;
- sample caches and region cache invalidation follow the original functions.

Use `Add Layer` repeatedly; `Clear` unloads the complete stack.

## Audio fidelity
- Realtime float output keeps SnappySynthV2's original soft clip/limiter path.
- UI volume is now post-synth AudioWorklet gain; it no longer overwrites MIDI
  Universal Master Volume inside SnappySynth.
- Default UI output volume is 100%.
- Original VOR stack mode remains default (`VOR=1`); `Stack gain` toggles mode 0.
- GM/GM2, GS reset, XG reset, Universal Master Volume, GS receive-channel map,
  GS drum-part assignment and GS scale tuning SysEx are routed through logic
  adapted directly from the original `snappysynth.c`.
- Short MIDI events honor the original GS part remapping path instead of calling
  `voice_send_short_at()` blindly.

## Browser-only adaptations retained
- Windows audio/KDMAPI/configurator are not compiled.
- Audio device output is AudioWorklet float32.
- WORKERFS supplies selected SF2 files.
- x86 intrinsics are guarded on wasm32; the previously-required scalar VOR
  helper remains for non-AVX2 builds.
