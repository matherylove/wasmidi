# WASMIDI Pass 11 — SnappySynthV2 WASM Hot Path

Apply on top of Pass 10 Full Fidelity.

This pass is performance-only. It does not replace the supplied SnappySynthV2
voice engine and does not change MIDI dispatch, voice stealing, VOR, filters,
envelopes, interpolation rules, SoundFont layering, controller state, SysEx
behavior, or the configured SnappySynth render-block cadence.

## Main bottlenecks found

The native source contains AVX2/AVX512 fast paths. Although the WASM target was
compiled with `-msimd128`, those x86-only feature guards left WebAssembly on
scalar paths.

The browser integration also added costs absent in the native driver:
- pthread mutex+condition-variable event emulation for worker barriers;
- a full PCM copy from WASM heap to a temporary transferable ArrayBuffer;
- one Worker -> AudioWorklet PCM transfer per rendered block;
- temporary JS event/offset TypedArrays followed by copies into WASM;
- front removal/compaction overhead in dense event batches.

## Optimizations

### WASM SIMD128
`Voice/voice.c` now includes WebAssembly SIMD equivalents for:
- multi-voice stereo sustain batching;
- mono -> mono no-interpolation sustain;
- mono -> stereo no-interpolation sustain;
- stereo -> stereo no-interpolation sustain;
- final worker-buffer copy/add accumulation.

They retain the source eligibility conditions and per-sample voice addition
order. Pitched/interpolated, filtered and envelope-transition paths continue to
use the original code.

No `-ffast-math` or relaxed DSP option was added.

### Futex-backed Emscripten events
Only on Emscripten, Win32-compatible events use an atomic 32-bit state plus
`emscripten_futex_wait/wake` instead of mutex+cond locking on every synth worker
start/done signal. Native/POSIX behavior outside Emscripten is unchanged.

### Shared-memory PCM ring
`ssw_render_into()` enters the exact same `ssw_render_to_buffer()` implementation
as `ssw_render()`, but writes directly into a ring in the pthread Shared
WebAssembly.Memory.

The AudioWorklet reads that same SharedArrayBuffer. The hot path no longer
performs:
- HEAPF32 -> temporary PCM Float32Array copy;
- transferable PCM ArrayBuffer allocation/pool;
- PCM postMessage transfer;
- buffer-recycle messages.

A generation counter rejects stale in-progress blocks after seek/flush.

The user-configured SnappySynth BufferSize is still the actual synth render
block size. It was deliberately not enlarged for benchmarks because that could
change block-boundary behavior.

### Event path
Final MIDI messages and sample offsets are written directly into Emscripten
HEAPU32. The two temporary JS scratch TypedArrays and their per-block heap copies
are removed.

Consumed event batches use a cursor plus occasional compaction instead of
repeated front removal.

### Output semantics
Pass 10's post-synth AudioWorklet `outputGain` multiply is retained exactly.
It was not moved to a GainNode, specifically to avoid an unnecessary audible
behavior change while optimizing unrelated transport work.


## Final hot-path cleanup

- Added WASM SIMD128 to subsequent worker mix-buffer accumulation.
- Removed the dense-event count pass; events are written directly into
  persistent WASM scratch arrays in one traversal.
- Removed normal-case AudioWorklet full-buffer clearing and bypassed only the
  exact `* 1.0` UI gain operation at unity.
- No interpolation, filters, envelopes, VOR, steals, SoundFont behavior,
  limiter/soft clip, MIDI timing formula, render block size, or worker ordering
  was changed.
