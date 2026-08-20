# WASMIDI Pass 5 — Horizontal MPWGL2 renderer fix

This overlay corrects three regressions:

1. Piano-roll orientation is restored to MPWGL2 semantics:
   - X = time
   - Y = pitch
   - vertical playhead at 18% width
   - notes move right-to-left toward the playhead
2. Notes are visible around currentTime using the same playhead-centered time model.
3. Keyboard black keys are top-anchored again instead of appearing upside-down in Qt WASM.

Files changed:
- src/renderer/gl_renderer.cpp
- src/keyboard.cpp
- src/qml/PianoRoll.qml

No CMake, workflow, parser, scheduler, or old/ files are modified.
