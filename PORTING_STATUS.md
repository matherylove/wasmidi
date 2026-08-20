# WASMIDI Pass 6.5 — DPR resolution + single frame scheduling

## Resolution fix
Qt's QQuickFramebufferObject::Renderer::createFramebufferObject(size) receives
a size that already includes devicePixelRatio. Pass 6.4 ignored that physical
size and resized GLRenderer using QML item's logical width/height.

The roll and keyboard now use framebufferObject()->size() as their OpenGL
viewport/private-texture resolution. No manual DPR multiplication is done.

## Frame scheduling
The piano roll no longer gets normal-playback frame requests from both:
- MainWindow::currentTimeChanged -> item/window update
- Renderer::update()

Renderer::update() is now the single continuous roll loop while playing.
GUI-side synchronization only happens for document/config/play state and
paused/large position jumps.

The keyboard also uses a render-thread monotonic clock and binary-searches
per-pitch note intervals, avoiding a GUI update on every 16 ms clock tick.

## QML performance
- Replaced the 16 ms neural-background Timer and fake timer-count FPS meter
  with Qt Quick FrameAnimation.
- FPS now uses FrameAnimation.smoothFrameTime.
- Neural background runs at display cadence while idle and every other frame
  while MIDI playback is active.
- Mini charts retain MPWGL2's 30 Hz sample rate but no longer have an extra
  60 Hz repaint Timer per chart.
- Timeline repaint reduced to 30 Hz.

Files:
- src/mainwindow.cpp
- src/pianoroll.cpp
- src/keyboard.cpp
- src/renderer/gl_renderer.cpp
- src/qml/PianoRoll.qml
- src/qml/Controls.qml
