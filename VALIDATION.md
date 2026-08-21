# WASMIDI Pass 11 — Validation

## Passed

- `node --check`:
  - web/snappysynth-worker.js
  - web/snappysynth-audio-worklet.js
  - web/snappysynth_bridge.js
- Native compatibility-event smoke test:
  - timeout while unsignaled
  - SetEvent
  - successful auto-reset wait
  - timeout after automatic reset
- WASM SIMD intrinsic compile test:
  - `clang --target=wasm32 -msimd128`
  - i16->i32->f32 conversion
  - f32x4 multiply/add
  - vector shuffle
  - v128 load/store
- Shared-ring AudioWorklet functional test:
  - 128 known interleaved stereo Float32 frames
  - exact L/R values delivered from SharedArrayBuffer
  - correct read/available atomic counters
- CMake exports `_ssw_render_into` and retains `-msimd128`.
- No `-ffast-math`.
- `ssw_render()` and `ssw_render_into()` both use the same
  `ssw_render_to_buffer()` path containing:
  - voice_set_render_timing
  - original MIDI dispatch
  - memset
  - voice_render_float
  - identical render cursor advancement
- No Pass 10 transferable PCM pool remains in the Worker.
- No `type: "pcm"` Worker -> AudioWorklet block transfer remains.
- MIDI event messages/offsets are written directly into WASM HEAPU32.
- Pass 10 `mainScriptUrlOrBlob`, pthread startup, layers, source voice stealing,
  VOR, SysEx and full configuration paths remain in place.

## Fidelity boundary

The optimization changes execution/transport, not SoundFont parsing or DSP
algorithms.

At unity UI gain, the shared-memory transport test is exact Float32 value for
value. Pass 10's outputGain multiplication is retained for non-unity UI volume.

WASM SIMD is used only in source-equivalent sustain/no-interpolation fast-path
cases. Other cases continue through the existing code.

## Environment limitation

The exact Qt 6.8.3 + Emscripten 3.1.56 final link is not available in this
container. The included repository GitHub Actions workflow remains the
authoritative WebAssembly build/runtime check.

Actual speedup depends on workload. Dense sustained voices with unfiltered
no-interpolation samples benefit most; heavily pitched/interpolated or filtered
voices still spend more time in scalar code.


## Final hot-path validation

- Worker, AudioWorklet and bridge: `node --check` PASS.
- wasm32 SIMD128 A/B:
  - scalar float accumulation vs SIMD accumulation: bit-exact PASS.
  - scalar int16->float scale/add vs SIMD scale/add: bit-exact PASS.
- Shared-memory AudioWorklet: 128 stereo Float32 frames delivered
  value-for-value at unity gain: PASS.
- No `-ffast-math`; existing source DSP and render cadence are unchanged.
