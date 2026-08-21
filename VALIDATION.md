# WASMIDI Pass 10 — Final Validation

Validated before final packaging.

## Passed

- `src/mainwindow.cpp`
  - C++17 host/stub syntax: `-Wall -Wextra -Werror`
  - C++17 Emscripten-stub syntax: `-D__EMSCRIPTEN__ -Wall -Wextra -Werror`
- `midi_parser.cpp`: C++17 `-Wall -Wextra -Werror`
- `scheduler.cpp`: C++17 `-Wall -Wextra -Werror`
- `gl_renderer.cpp`: C++17 `-Wall -Wextra -Werror` with GLES3 validation stubs.
- JavaScript `node --check`:
  - `web/snappysynth_bridge.js`
  - `web/snappysynth-worker.js`
  - `web/snappysynth-audio-worklet.js`
  - `web/coi-serviceworker.js`
- GitHub Actions YAML parses and contains both build and Pages deployment jobs.
- Pages workflow explicitly publishes every SnappySynthV2 runtime dependency:
  - `snappysynth-core.js`
  - `snappysynth-core.wasm`
  - `snappysynth-core.worker.js`
  - `snappysynth-worker.js`
  - `snappysynth-audio-worklet.js`
  - `snappysynth_bridge.js`
  - `coi-serviceworker.js`
- Native SnappySynthV2 core/config smoke test passed:
  - sample rate 44100
  - stereo
  - 32-bit metadata
  - 16 buffers
  - explicit 2-worker request
  - float render
- Complete adapted C source set passes native C11 syntax validation with POSIX/pthread compatibility enabled.
- Worker -> C core ABI was checked: every `_ssw_*` function called by the Worker is defined and exported by CMake.
- QML synth properties were checked against `MainWindow` properties/setters.
- Bridge -> Worker configuration fields were checked end-to-end.
- Pass 9.4 pthread startup protections remain present:
  - `mainScriptUrlOrBlob`
  - `workerReadyPromise`

## Source configuration coverage

Browser-applicable source configuration/API controls are exposed:

- MaxVoices
- Minimum Voices
- BufferSize
- NumBuffers
- SampleRate request
- NumChannels
- BitsPerSample
- RealtimePriority metadata
- VOR / stack-gain mode
- output soft clip
- SS_WORKERS
- SS_NOTE_SHARDING
- SS_STEAL_SCORE_CACHE
- SS_FAST_NOTE_OFF
- SS_VALIDATE_STATE
- multi-SF2 SoundFont layers

Runtime statistics expose:

- active voices
- free voices
- steals
- effective worker count
- layer count
- region count
- underruns

Desktop-only source paths are explicitly identified rather than emulated:
WinMM/DirectSound `AudioAPI` and the DirectCompute GPU mixer are not available
inside browser WASM; browser output uses AudioWorklet.

## Fidelity checks

- The supplied SnappySynthV2 `voice.c` remains the voice/steal/VOR engine.
- Worker policy is restored to the source's automatic logical-core behavior.
- Multi-SF2 stacking uses the source's preset/bank override semantics.
- MIDI short-message dispatch preserves GS part remapping.
- SysEx path covers GM/GM2 reset, GS reset, XG reset, Universal Master Volume,
  GS receive-channel mapping, drum-part assignment and scale tuning.
- UI volume is post-synth AudioWorklet gain, so it does not overwrite MIDI
  Universal Master Volume state.
- Original float rendering path and soft-clip path are retained.
- Max/Minimum voices UI state now stays consistent with the source behavior:
  Minimum Voices is a floor for the effective voice cap.

## Environment limitation

The container does not include the exact Qt 6.8.3 WebAssembly SDK and
Emscripten 3.1.56, so the authoritative final Qt/Emscripten link is performed
by the included GitHub Actions workflow.

The supplied SnappySynthV2 archive does not include a SoundFont fixture, so
automated validation covers engine initialization/rendering, protocol,
configuration, ABI, layering implementation and source-path fidelity rather
than an offline A/B waveform comparison against a bundled SF2.
