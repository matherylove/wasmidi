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
