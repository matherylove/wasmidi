# WASMIDI Pass 12 — Validation

## Local checks passed

- `sha256sum -c MANIFEST.sha256` for all manifest-tracked critical files.
- `git diff --check`.
- `node --check`:
  - `web/midi-parser-worker.js`
  - `web/visual-cache-worker.js`
  - `web/snappysynth_bridge.js`
  - `web/snappysynth-worker.js`
  - `web/snappysynth-audio-worklet.js`
- Native C++17 compilation of:
  - `src/midi/midi_parser.cpp`
  - `src/midi/midi_document_codec.cpp`
  - `src/midi/midi_worker_core.cpp`
- `MidiDocument` wire-codec round trip, including corruption rejection.
- Synthetic 50,000-note same-tick MIDI parser stress test:
  - 50,000 source/visual notes retained;
  - key start/end streams compressed to 88 counted entries each;
  - peak polyphony and peak NPS both detected as 50,000;
  - serialized/deserialized document retained counts and visual data.
- Visual-cache Worker functional test:
  - 4 requested roll pages emitted;
  - 4 matching keyboard snapshots emitted;
  - current page prioritized first;
  - keyboard state at page boundaries matched inclusive note-end semantics.
- AudioWorklet visual-clock gate functional test:
  - PCM consumption stopped at the visual lead ceiling;
  - intentional visual gating did not increment underrun count;
  - advancing the visual clock released PCM consumption again.

## Fidelity rules exercised by the implementation

- SnappySynth synthesis still enters the existing `ssw_render_to_buffer()` /
  `voice_render_float()` path.
- Larger browser render calls retain sample-accurate MIDI offsets inside each
  call.
- No `-ffast-math` or relaxed DSP mode is added.
- Dense-note removal is full-pixel-occlusion-only; partially visible notes are
  not split or discarded.
- Horizontal source/draw order is retained after dense reduction.
- Long-note lifetime uses `endTick`, not only `startTick` cache residency.
- Missing background visual pages fall back to the authoritative source index.
- Keyboard snapshot recovery has an exact parser-index fallback.
- SnappySynth audio clock is not allowed to advance the visual timeline.

## Environment limitation

This container does not provide Qt 6.8 or Emscripten (`emcc`), so the final Qt
WebAssembly compile/link cannot be executed locally. The GitHub Actions workflow
is configured for that authoritative build.

The connected GitHub integration in this session can read the repository but
returned HTTP 403 `Resource not accessible by integration` for branch/blob write
operations, so this pass could not be pushed to a CI branch from the session.

## Pass 12.2 parser bootstrap validation

The background MIDI parser is packaged as one modularized JavaScript file with
its WebAssembly payload embedded. This prevents a newly deployed glue file from
being paired with a stale cached `.wasm` file. The browser worker and its core
URL are versioned (`12.2`) to invalidate older worker/bootstrap cache entries.

The Pages workflow now executes the *generated* parser module under Node before
uploading the Pages artifact. The smoke test checks every required export and
parses a valid format-0 MIDI containing an EndOfTrack event. A parser module
that aborts during startup or cannot call `_wmp_parse()` therefore fails CI and
cannot reach deployment.

The parser target also keeps an inert real `main()`, uses `NO_EXIT_RUNTIME=1`,
and catches C++ allocation/standard exceptions around the complete parse and
serialization operation. Parser-only Emscripten assertions remain enabled so a
future fatal runtime error reports its actual reason and stage instead of only
`Aborted()`.

## Pass 12.4 memory regression validation

- Compared the old and new keyboard-index algorithms on randomized multi-track
  MIDIs, zero-length notes, orphan NoteOns, overlapping FIFO notes, and a
  200,000-note crashpoint. `visualNotes`, `visualKeyStarts`, `visualKeyEnds`, and
  `visualKeyOwners` were byte-identical.
- A native 2,000,000-note identical crashpoint produced the same document hashes
  while peak RSS fell from about 82,008 KiB to 73,084 KiB. The removed temporary
  saves approximately 8 bytes per raw visual note, so the benefit scales
  linearly on the giant files that triggered the 2 GiB failure.
- The generated-module CI smoke test now requires the phased `_wmp_parse` / raw
  input free / `_wmp_pack` API and parses an additional 250,000-note dense
  zero-length crashpoint before deployment.
- Parser result capacity is explicitly released after transfer and before the
  next load.

## Pass 12.5 Memory64 / adaptive-RAM validation

- Parser compile+link flags select `MEMORY64=1`, keep a 64 MiB initial heap, set
  the current Chromium/V8 Memory64 ceiling (16 GiB), and disable geometric heap
  over-allocation.
- `_wmp_pointer_bits()` is part of the generated ABI; the Worker and CI smoke
  test require 64-bit pointers so an accidental wasm32 build cannot deploy.
- The Worker uses BigInt for wasm64 pointer/`size_t` calls and converts addresses
  to JavaScript numbers only after verifying they are exact safe integers.
- Raw MIDI bytes are streamed directly from `File.stream()` into the parser heap;
  no file-sized JavaScript `Uint8Array` is retained.
- Packed documents are transferred in 16 MiB transferable chunks instead of one
  result-sized JavaScript buffer.
- GitHub Actions selects Node 24 for the generated-module smoke test because that
  runtime supports WebAssembly Memory64.
- Local syntax/native checks do not substitute for the authoritative Emscripten
  Memory64 link; CI must compile and execute the generated parser module before
  deployment.

## Pass 12.6 Memory64 ABI validation

- Parser Worker and generated-module smoke test now use only the Number-safe
  facade exports: `_wmp_alloc_js`, `_wmp_free_js`, `_wmp_parse_js`,
  `_wmp_result_ptr_js`, `_wmp_result_size_js`, and `_wmp_error_ptr_js`.
- Native C++17 facade round-trip validated allocation -> parse -> free raw MIDI
  -> pack -> result pointer/size -> release.
- `node --check` passes for the Worker and smoke test.
- The Worker and Qt picker cache-busters were advanced to `12.6`.
- The remaining mandatory CI check is execution of the *generated* Memory64
  Emscripten module under Node 24; it must complete both the minimal MIDI and
  dense 250k-note smoke parse without any BigInt conversion error.

## Pass 12.7 — Memory64 progress callback ABI hotfix

The Memory64 parser's public API was already Number-only, but parser progress
callbacks still passed a raw `const char*` through `EM_JS`. Emscripten 3.1.56
represents that wasm64 pointer as JavaScript `BigInt`, while `UTF8ToString`
requires a `Number`. Pass 12.7 legalizes progress-stage pointers to `double` in
C++ before entering JavaScript, so progress reporting uses the same JS-safe ABI
as the parser's exported data/result/error paths. The browser Worker/core URLs
are cache-busted to 12.7, and CI requires at least one decoded string progress
stage during the generated Memory64 parser smoke test.

## Pass 12.8 — paged source validation

- Native `parse()` and `parseReadAt()` outputs were serialized and compared
  byte-for-byte on randomized MIDIs plus 200k and 2M-note crashpoints.
- A 16,000,026-byte 2M-note MIDI required 9 read-at calls; the largest was
  exactly 4,194,304 bytes.
- A synthetic random-access MIDI of 503,316,520 bytes (~480 MiB) was parsed
  without materializing the file. It required 7 reads, each <=4 MiB, produced a
  124-byte wire document, and the native test peaked at roughly 12 MiB RSS.
- `midi_parser.cpp`, `midi_worker_core.cpp`, and `midi_document_codec.cpp`
  compile as C++17 in the local validation environment.
- `node --check` passes for `web/midi-parser-worker.js` and
  `tools/midi_parser_bootstrap_smoke.cjs`.
- GitHub Actions must still compile/run the generated Emscripten Memory64 module;
  the smoke test now exercises `_wmp_parse_file_js()` and includes the virtual
  ~480 MiB source regression.

## Pass 12.9 — stale deployment regression validation

- `web/midi-parser-worker.js` advertises bootstrap `12.9`, reports a
  `worker-ready` handshake, and uses `_wmp_parse_file_js(total)` for production
  browser files.
- `src/mainwindow.cpp` fetches the Worker source with `cache: no-store`, verifies
  the exact `12.9` marker before execution, and launches it with an explicit base
  URL so the generated single-file Memory64 parser can still be imported from a
  Blob Worker.
- The obsolete `Allocating parser memory on demand` string and production
  `_wmp_alloc_js(file.size)` pattern are absent from the current source tree.
- The COI service worker no longer returns early when a page is already isolated;
  it checks for an updated worker and uses `updateViaCache: none`. JS/WASM/HTML/
  data requests are fetched with `cache: no-store` while retaining COOP/COEP.
- The Pages artifact fingerprints `wasmidi.js`, `coi-serviceworker.js`, and
  `snappysynth_bridge.js` with `${GITHUB_SHA}` and writes `build-id.txt`.
- `node --check` passes for both browser workers and the generated-module smoke
  harness; workflow YAML parses successfully; the artifact post-processing
  Python was executed against a representative Qt HTML bootstrap.

## Pass 13.0 — SharpMIDI-style mapped-store validation

- `src/midi/midi_mapped_store.cpp` compiles under C++17 with
  `-Wall -Wextra -Werror` together with the existing parser/codec/worker core.
- A native parity smoke MIDI containing tempo, explicit status, running status,
  CC, NoteOff and velocity-zero NoteOn validated mapped metadata, visual-page
  reconstruction, inclusive keyboard state and bounded playback batches.
- A 1,000,000-note same-pitch crashpoint used an ~8 MiB source, retained only
  the sparse mapped index, reconstructed one merged visible note, and returned a
  bounded 65,536-event playback batch. Native test peak RSS was ~31 MiB.
- A virtual single-track MIDI larger than 2 GiB (2,500,000,022 bytes) indexed
  successfully without materializing its source. The largest source read was
  exactly 1 MiB and only 11 source-page reads were required for the synthetic
  sparse file.
- `node --check` passes for the parser Worker, visual worker, SnappySynth bridge,
  SnappySynth worker, AudioWorklet and generated-module smoke harness.
- The generated-module smoke harness now requires the mapped visual-page,
  keyboard-snapshot and bounded-event APIs and rejects a dense source if the Qt
  metadata wire image grows proportionally with note count.

The final Qt 6.8 + Emscripten/Memory64 link and browser execution still require
GitHub Actions because this container does not provide the project Qt WASM SDK.
