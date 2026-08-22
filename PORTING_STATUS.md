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

## Pass 12.2 — parser startup hardening

The dedicated MIDI parser no longer deploys separate generated `.js` and
`.wasm` files. It is a single-file modularized Emscripten module with a
cache-versioned Worker bootstrap, inert entry point, persistent runtime, and
parser-only exception/assertion diagnostics. GitHub Actions now loads the exact
generated module and parses a minimal MIDI before Pages deployment.

## Pass 12.4 — large black-MIDI parser memory hotfix

The background parser no longer creates one temporary 64-bit end-sort key for
every visual note while building the keyboard timeline. NoteOff counts are now
aggregated while `buildVisualNotes()` pairs the source event stream, and only
the final compressed `VisualKeyEvent` records are sorted/coalesced. This removes
an O(noteCount) 8-byte temporary that could push dense files past wasm32 memory
near the end of parsing.

Parser packing is now a second explicit phase (`wmp_pack`). The Worker frees the
raw MIDI allocation after `wmp_parse` succeeds and before the serialized wire
image is created. The serialized result is also explicitly released immediately
after JS copies it, so a previously loaded giant MIDI cannot retain its old
capacity into the next load.

The isolated MIDI parser Worker keeps a 64 MiB initial memory but can grow to the
4 GiB wasm32 ceiling on desktop browsers. Qt and SnappySynth memory limits are
unchanged.

## Pass 12.5 — adaptive Memory64 parser budget

The dedicated background MIDI parser is now built as full WebAssembly Memory64
while the Qt renderer/player remains wasm32. The parser starts at 64 MiB and has
no WASMIDI-specific runtime RAM budget below the browser engine's Memory64
ceiling. `ALLOW_MEMORY_GROWTH` commits pages only as allocations need them and
geometric overgrowth is disabled, so an allocation fails when the browser/OS can
no longer supply memory rather than because WASMIDI chose an arbitrary 2/4 GiB
limit.

Current Chromium/V8 implements a 16 GiB hard ceiling for one Memory64 linear
memory. That browser-engine ceiling cannot be raised by application JavaScript,
and browsers do not expose exact free system RAM. For Chrome 133+ this still
raises the isolated parser from wasm32's 4 GiB maximum to 16 GiB and lets real
machine/process pressure decide any lower effective limit.

The file picker no longer builds a second full-size JavaScript copy of the MIDI.
The selected `File` is streamed directly into the already-grown parser heap.
Likewise, the packed document is transferred to the Qt module in 16 MiB chunks,
removing the previous full result-sized JS staging allocation.

Qt remains wasm32 but is explicitly allowed to grow to its 4 GiB address-space
ceiling. A parsed wire image larger than that is reported as requiring segmented
player residency rather than being silently truncated through a 32-bit ABI.

## Pass 12.6 — Memory64 JS ABI facade

Memory64 itself was working, but CI exposed an Emscripten ABI mismatch: the raw
`wmp_parse(const uint8_t*, size_t)` export required i64/BigInt arguments while
the address returned by the allocation path was observed as a JavaScript Number.
The generated module therefore rejected a valid pointer before parsing began.

The parser now exports a deliberately Number-only JavaScript facade. Allocation,
free, parse pointer/length, result pointer/size and error pointer cross the wasm
boundary as f64 values. C++ validates that every value is a finite exact
non-negative integer, then converts it to `uintptr_t`/`size_t`. This is exact for
the parser's <=16 GiB address space (well below 2^53), removes all JS BigInt
coercion ambiguity, and leaves the internal parser fully Memory64. Raw pointer
exports and `_malloc`/`_free` are no longer part of the parser's public JS ABI.

## Pass 12.7 — Memory64 progress callback ABI hotfix

The Memory64 parser's public API was already Number-only, but parser progress
callbacks still passed a raw `const char*` through `EM_JS`. Emscripten 3.1.56
represents that wasm64 pointer as JavaScript `BigInt`, while `UTF8ToString`
requires a `Number`. Pass 12.7 legalizes progress-stage pointers to `double` in
C++ before entering JavaScript, so progress reporting uses the same JS-safe ABI
as the parser's exported data/result/error paths. The browser Worker/core URLs
are cache-busted to 12.7, and CI requires at least one decoded string progress
stage during the generated Memory64 parser smoke test.

## Pass 12.8 — bounded random-access MIDI source

The browser parser no longer allocates `file.size` bytes in Memory64 before
parsing. `MidiParser::parseReadAt()` reads a 64-bit source through a callback,
and its source cursor caches at most 4 MiB. The dedicated Worker backs that
callback with `FileReaderSync(file.slice(...))`, so both parser passes can seek
through a huge browser `File` without copying the full raw MIDI into WASM.

Production uses `_wmp_parse_file_js(size)`; the previous Number-safe contiguous
API remains available for diagnostics but is no longer used by the browser file
picker. A generated-module smoke source emulates a ~480 MiB MIDI without
allocating it and verifies that no read exceeds the 4 MiB window.

The current GitHub `main` also had an inconsistent parser/manifest combination:
`MANIFEST.sha256` described the low-memory keyboard-end implementation while
`src/midi/midi_parser.cpp` still contained the older `endKeys.reserve(notes.size())`
path. Pass 12.8 ships the cumulative low-memory parser implementation again so
that regression is removed together with paged input.

## Pass 12.9 — deployment freshness / paged-source enforcement

The paged `parseReadAt()` implementation from Pass 12.8 was present in source,
but a browser could still execute an older deployed Qt bootstrap/Worker and show
the obsolete `Allocating parser memory on demand` path. Pass 12.9 removes that
ambiguity. Qt fetches `midi-parser-worker.js` with `cache: no-store`, verifies an
exact `12.9` source marker, launches the fetched source from a Blob with an
explicit deployment base URL, and the Worker reports a `worker-ready` handshake
stating that paged-source mode is active. The COI service worker now checks for
updates even while the current page is already cross-origin isolated, registers
with `updateViaCache: none`, and bypasses HTTP cache for executable same-origin
assets. Pages fingerprints the Qt/COI bootstrap URLs with the commit SHA and
ships `build-id.txt`.

The production MIDI path still uses `_wmp_parse_file_js()` with bounded 4 MiB
random-access windows; it does not allocate `file.size` in the parser heap.

## Pass 13.0 — SharpMIDI-style mapped residency

The large-MIDI path now follows the memory model used by SharpMIDI-raylib instead
of building a permanent `MidiDocument` proportional to every channel event and
NoteOn. The browser `File` remains the authoritative backing store in the
persistent parser Worker. The Memory64 core keeps a 32 MiB LRU source-page cache,
track/source checkpoints spaced by 65,536 channel events or 4 MiB, tempo/SysEx
metadata, and 128 fixed visual-state checkpoints. Renderer pages, keyboard state,
and exact synth event batches are decoded from the mapped source on demand.

Qt receives metadata only (`MidiDocument::remoteIndexed=true`) and never receives
the full event/note stream. The horizontal renderer requests bounded visual pages,
the keyboard requests fixed 384-word snapshots, and SnappySynth receives bounded
exact channel-event batches from a persistent 64-bit cursor. This removes the
previous `events + visualNotes + keyboard-index + serialized-copy + Qt-copy`
resident-memory multiplication and removes the Qt wasm32 heap as the storage
location for the complete MIDI.

The design intentionally mirrors SharpMIDI's important properties: source-backed
parsing, two-pass indexing, long event counters/offsets, bounded playback buffers,
and reconstruction of visible notes during a sweep instead of permanent storage
of one render note per NoteOn.

## Pass 13.1 — performance/stability follow-up

The SharpMIDI-style mapped source now becomes playable after one indexing pass.
Visual checkpoints are lazy and sparse. Horizontal rendering retains the last
complete GPU page composite during cache misses and only rebuilds that composite
when a page used by the viewport changes. The 64-page prefetch protocol sends
missing masks instead of rebuilding overlapping pages. SnappySynth catch-up
velocity shedding is debounced/rate-limited and the previous 100 ms hard-resync
loop was replaced by immediate recovery resync plus a much larger healthy-drift
safety threshold.

### Pass 13.1 current mapped-store residency details

The historical Pass 13.0 description above is superseded for the current build:
track source checkpoints are now byte-spaced (4 MiB) rather than event-count
spaced, and the initial load performs one full source scan. Visual-state
checkpoints are generated lazily on demand, so a billion-note crashpoint does
not create one checkpoint for every 65,536 channel events during loading.
