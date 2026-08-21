# WASMIDI Pass 12 — Buffered Playback and Visual Pipeline

This pass keeps the supplied SnappySynthV2 DSP/voice engine and changes the
browser scheduling, transport, loading and visual-cache architecture around it.

## SnappySynthV2

A source comparison against the supplied native SnappySynthV2 tree found the
WASM `Voice/voice.c` to be overwhelmingly source-equivalent; the largest browser
penalty was the execution model around it rather than a replacement DSP engine.

The browser backend now:

- writes PCM directly into a Shared WebAssembly.Memory ring read by the
  AudioWorklet;
- uses that ring as a configurable rolling pre-render cache (default 8 s,
  `0` = up to the MIDI duration, with a browser-memory cap);
- renders several configured source blocks in one `ssw_render_into()` call
  (normally about 4096 frames) while retaining sample offsets for events inside
  the larger call, amortizing pthread worker barriers;
- proactively fills the ring instead of waiting for a shallow realtime queue;
- invalidates pre-rendered PCM on seek and audio/MIDI-stream settings changes;
- keeps the user velocity floor as the minimum adaptive floor and can raise the
  effective floor toward 127 under audio catch-up pressure.

The AudioWorklet is transport-following, not transport-leading. The horizontal
visualizer is the master clock. Audio consumption is bounded by the most recent
visual clock sample, and recovered/starved audio is rebuilt at the current
visual position instead of resuming from an old lagged position.

## Master visual clock

Playback time advances only after the horizontal renderer reports another
completed frame. Each accepted step is bounded, so a renderer slowdown becomes
a transport slowdown instead of allowing the keyboard/audio to run ahead while
visual frames are dropped.

The keyboard consumes the same MainWindow timeline. SnappySynth may prepare PCM
ahead in the background, but the AudioWorklet cannot consume indefinitely past
the visual clock.

## Non-blocking MIDI loading

Browser MIDI loading uses a dedicated non-pthread Emscripten parser module in a
Web Worker:

- file reading reports progress;
- parsing, visual-index construction and density statistics are performed in the
  Worker;
- a compact `MidiDocument` wire representation is transferred to Qt;
- QML presents a modal program-styled percentage/progress overlay throughout the
  browser loading path.

The normal native/direct `loadMidiRaw()` path remains available outside the
browser Worker picker path.

## Horizontal visualizer

The renderer keeps two complementary caches:

1. the persistent start-ordered GPU ring for immediate/recovery rendering;
2. a background 64-page rolling render cache built by `visual-cache-worker.js`.

A note is no longer discarded merely because its start left the rolling window.
Long notes move into a carry cache and remain drawable until their end leaves the
visible history. Reverse seeks and discontinuous jumps explicitly rebuild the
ring and carry set using the parser's 4096-note max-end seek index.

The page Worker scores upcoming screen pages from note density, same-tick
crashpoints, pitch concentration and duration pressure. The current page is
prepared first, then the hardest pages. Difficulty results are cached while the
page span remains unchanged.

Missing/unfinished pages never block playback: the renderer falls back to the
persistent source GPU ring.

## Keyboard visual cache

The parser builds counted key-start/key-end streams and a newest-owner stream.
The same visual-cache Worker prepares exact keyboard snapshots at the rolling
screen boundaries, in the same difficult-page priority order as the roll.

A seek restores the nearest cached snapshot and replays only the residual event
range. If the snapshot is unavailable, the existing exact source/block-index
recovery path is used.

## Dense-note reduction

For dense views, note intervals are projected to the current horizontal pixel
columns and processed in reverse source/draw order. An earlier same-pitch note is
removed only when every pixel column it could contribute is already covered by a
later opaque note. Zero-width sub-pixel notes are also removed.

Partial overlaps are retained whole. Source order is restored before drawing, so
stacking, clipping and visible density are unchanged. Coverage uses a 64-bit
per-pitch bitset to keep full-occlusion queries cheap at crashpoints.

## Resize and live colors

During live resize, the old FBO texture is reused/stretched and final-resolution
FBO recreation is delayed until geometry has been stable for 140 ms. The
keyboard instance buffer is only rebuilt when key state or palette data changes.

Custom channel color changes are forwarded while the color dialog is changing,
not only when it is accepted; presets remain immediate.

## Build note

The repository workflow has been extended to build/deploy the standalone MIDI
parser WASM/JS and both hand-written browser Workers. A full Qt 6.8/Emscripten
link still requires the repository CI/toolchain; the local execution environment
used for this pass does not contain Qt or `emcc`.
