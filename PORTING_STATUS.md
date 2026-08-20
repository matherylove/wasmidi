# WASMIDI MPWGL2 Port — Pass 1

This overlay is intentionally limited to files outside `old/`. The legacy folder remains untouched as the behavioral reference.

## Ported in this pass

- Qt/QML application shell modeled after MPWGL2's two-column player layout.
- File bar, transport, seek, note-window speed, post-buffer, live stats, detailed MIDI stats, channel/track color mode, 16-channel controls, MIDI output tabs, volume state and embedded-synth panel shell.
- Monotonic playback clock replacing the fixed `+0.016` timer increment.
- High-density live statistics using sorted start/end indices rather than a full note scan every frame.
- Peak NPS and peak polyphony precomputation.
- Native C++/WebGL2 piano-roll surface using instanced rendering.
- Native C++/WebGL2 128-key keyboard surface with active-pitch highlighting.
- MIDI parser two-pass tempo/event conversion, overlapping same-pitch NoteOn support, complete channel-event collection, sorted controls and final-tempo-map timing.
- Scheduler rebuilt around a single sorted event stream so note-offs are not lost when they fall outside the frame that emitted the matching note-on.
- CMake cleaned around the actual Qt/QML/WebGL source paths and duplicate/empty prototype files.

## Still to port from MPWGL2 / SnappySynth

- Browser Web MIDI bridge: device permission, enumeration, selection, send/batching, panic/reset and reconnect handling.
- SnappySynth C++/WASM implementation: SF2 parser, regions, voices, ADSR, sustain, pitch bend, bank/program state, exclusive classes, voice stealing, limiter and GM/GS/XG reset/SysEx handling.
- AudioWorklet/WASM bridge and audio-clock authority.
- SF2 drop/file/URL loading and synth runtime statistics.
- MPWGL2 parsing/loading progress UI and worker-style asynchronous large-file loading.
- More exact MPWGL2 render behavior: note culling/streaming for multi-million-note files, channel/track palettes, post-buffer trails and renderer stats/FPS.
- Remaining responsive/mobile behavior and small visual details from the original HTML.
- GitHub Actions replacement for the current obsolete `old/midi_parser.c` build workflow.

No files under `old/` are modified by this overlay.


## CI / deployment gate

- GitHub Actions now targets Qt 6.8.3 + Emscripten 3.1.56.
- The first Pages-compatible build uses `wasm_singlethread`; GitHub Pages does not provide the cross-origin-isolation headers required by pthread-based WebAssembly.
- Pull requests build and upload a reproducible WASM artifact but do not deploy.
- Only a successful push to `main` uploads the Pages artifact and deploys it.
- `QQuickFramebufferObject` requires the Qt Quick OpenGL backend, so `main.cpp` forces `QSGRendererInterface::OpenGL` before creating any window.
