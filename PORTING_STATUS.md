# WASMIDI Pass 9 — SnappySynthV2 Worker Audio

Apply this overlay on top of the current WASMIDI main / Pass 8.3 state.

## Audio architecture

The old browser MIDI-output path is removed from the UI/controller. WASMIDI now
uses the SnappySynthV2 source archive supplied for this port as its synth engine:

Qt/WASM CompactEvent scheduler
  -> batched timestamped MIDI messages
  -> dedicated snappysynth-worker.js
  -> SnappySynthV2 pthread WASM core
  -> interleaved float PCM
  -> direct MessageChannel
  -> AudioWorklet
  -> browser/system audio output

The Qt WebAssembly application remains single-threaded. SnappySynthV2 is built
as a separate Emscripten pthread module, so its voice workers do not run on the
QML/render hot path.

## SnappySynthV2 subset

Only the required upstream pieces are included under third_party/snappysynthv2:
- voice engine
- SF2/SFZ/WAV parser support required by SF2 instruments
- minimal headers/stubs
- browser-specific C ABI in snappy_wasm_core.c

Not integrated:
- KDMAPI / WinMM / browser MIDI Out
- DirectSound / Windows output backends
- SnappySynth MIDI-file parser
- Configurator
- DLL/config plumbing
- GPU/D3D backends

## Browser audio

- SF2 files are passed to the synth Worker as File objects and mounted with
  WORKERFS; the main Qt thread does not copy the full SoundFont into its WASM heap.
- AudioContext sample rate is propagated to the worker/core (44.1/48 kHz etc.).
- SnappySynth renders float stereo blocks with voice_render_float().
- PCM moves Worker -> AudioWorklet directly through MessageChannel.
- Transferable ArrayBuffers are recycled to reduce GC allocation pressure.
- Worklet queue uses low/high water marks and reports underruns.
- The player clock follows actually consumed audio frames; if synthesis starves,
  visuals do not run ahead of the audible stream.
- A small final safe-render margin lets the audio clock cross the exact MIDI
  duration instead of deadlocking on a block that straddles the final timestamp.

## Black MIDI scheduling

- C++ sends only the new ~400 ms scheduling delta every 20 ms.
- Events are transferred in typed-array batches, not individual JS messages.
- Exact consecutive simultaneous NoteOns are packed using SnappySynthV2's high
  byte overlap stack-count convention.
- Seek reconstructs controller/program/pitch-bend state and notes that genuinely
  span the seek position.

## Keyboard / visual correctness

- Keyboard state uses independent Pass 8.3 VisualNote intervals instead of the
  old channel+pitch merged runtime state.
- A 4096-note block max-end index skips dead history during seeks while preserving
  long sustained notes.
- Fractional tick playback removes a key immediately after its real MIDI end
  instead of waiting for the next integer tick.
- Overlapping same-pitch notes remain counted independently.

## Number display

Large counters are explicitly formatted as ordinary decimal digits. QML no
longer relies on the engine's default Number-to-string conversion, so values are
shown like `44,750,700` rather than `4.47507E+07`. BPM retains ordinary decimal
places when needed. Text uses HorizontalFit instead of scientific abbreviation.

## UI

The former MIDI I/O / MIDI Out controls are replaced by SNAPPYSYNTH V2 controls:
- Load SF2
- Max voices
- Audio block size
- Overlap gain mode
- Sample rate
- Active synth voices
- Underrun count

The existing master volume controls SnappySynthV2 directly.

## Build/deploy

CMake builds two independent WebAssembly modules:
- wasmidi: Qt 6.8 wasm_singlethread
- snappysynth-core: pthread-enabled Emscripten worker module

GitHub Actions deploys the synth JS/WASM/pthread worker plus the bridge,
AudioWorklet, worker host and COI service worker. GitHub Pages receives
cross-origin-isolation headers through the service worker so SharedArrayBuffer
and Emscripten pthreads can operate.

See VALIDATION.md for the final pre-package checks.
