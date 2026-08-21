# Pass 9 final validation

Final validation performed before packaging.

## Passed

- JavaScript syntax (`node --check`):
  - web/snappysynth_bridge.js
  - web/snappysynth-worker.js
  - web/snappysynth-audio-worklet.js
  - web/coi-serviceworker.js
- GitHub Actions YAML parses successfully and contains both build/deploy jobs.
- `midi_parser.cpp`: C++17 `-Wall -Wextra -Werror` syntax check passed.
- `scheduler.cpp`: C++17 `-Wall -Wextra -Werror` syntax check passed against the
  merged Pass 8.3 + Pass 9 tree.
- `gl_renderer.cpp`: C++17 `-Wall -Wextra -Werror` syntax check passed using the
  GLES3 API validation stub.
- Adapted SnappySynthV2 C sources compile and link together natively.
- SnappySynth core runtime smoke test passed at 48,000 Hz: init -> render float
  stereo PCM -> reset -> shutdown.
- Parser functional test passed for overlapping same-pitch notes across tracks,
  FIFO NoteOff pairing, original MIDI channel metadata, long sustained notes and
  the VisualSeekBlock max-end accelerator.
- Static bridge/API check confirms every C++ bridge method is exported by
  `WasmidiSnappyBridge`.
- Static worker/core check confirms SnappySynth functions used by JS exist in the
  C adapter and are listed in Emscripten `EXPORTED_FUNCTIONS`.
- AudioContext sample rate propagation is present bridge -> worker -> `_ssw_init`.
- Runtime-facing QML/controller contain no old outputMode, Request Access,
  MIDI I/O or Web MIDI bridge controls.
- Large-number formatters explicitly generate ordinary decimal strings and use
  HorizontalFit; scientific notation is not used for the affected UI counters.
- New Pages workflow checks/copies the synth `.js`, `.wasm`, pthread worker,
  bridge, synth worker, AudioWorklet and COI service worker.

## Environment limitation

This container does not include the Qt 6.8.3 WebAssembly SDK or Emscripten
3.1.56, so the exact final Qt/Emscripten link cannot be executed locally.
The included GitHub Actions workflow performs that authoritative toolchain build
when the overlay is committed to the repository. The pre-package native,
syntax, protocol and integration checks above all pass.
