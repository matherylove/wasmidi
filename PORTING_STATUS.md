# WASMIDI GUI Pass 3

This overlay fixes three browser-facing parity issues from MPWGL2.html:

1. Browser-native MIDI file chooser
   - Removes use of QtQuick.Dialogs FileDialog for MIDI loading.
   - Adds MainWindow::openMidiPicker().
   - On WebAssembly, an Emscripten bridge creates a hidden HTML file input.
   - The browser opens the user's native OS file chooser and copies the selected MIDI bytes directly into the C++ parser.

2. Horizontal keyboard fidelity
   - Reworks the C++ WebGL keyboard shader and key geometry.
   - Restores dark navy "white" keys, near-black raised black keys, crisp separators, subtle dimensional shading, octave labels, and active-key purple glow.
   - Keeps all 128 MIDI notes across the renderer.

3. Neural background visibility
   - Increases the node count, link range, contrast, motion and pulse visibility.
   - Keeps a faint neural field while MIDI is loaded and a stronger version in the empty state.
   - Keeps the render path in Qt/QML over the native WebGL piano roll.

No old/ files are modified.
No CMake or GitHub Actions changes are required for this overlay.
