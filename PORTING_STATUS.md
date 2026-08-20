# WASMIDI Pass 6.4 — render-thread clock + ACTIVE fix

## ACTIVE counter
Fixed undefined behavior in the live polyphony calculation. Previous code
subtracted iterators from two different std::vector instances. The MPWGL2
formula is now implemented as two independent counts:
upperBound(noteStarts,t) - lowerBound(noteEnds,t).

## Frozen piano roll
Pass 6.3 GUI-side update signals were present in main, but the roll could still
remain visually frozen in Qt/WASM.

Pass 6.4 no longer requires a fresh synchronize() on every visual frame:
- synchronize() copies a playback-time anchor + playing state.
- render() extrapolates current playback time with std::chrono::steady_clock.
- Renderer::update() keeps the FBO rendering while playback is active.
- GUI-side QQuickFramebufferObject::update() remains connected for seeks,
  config changes and normal synchronization.
- QQuickWindow::update() is requested as an additional WASM scene-graph wakeup.
- QQuickOpenGLUtils::resetOpenGLState() is called after custom GL rendering,
  as recommended by Qt because Qt Quick and the custom FBO share the same
  OpenGL context.

The clean Pass 6.1 GL/FBO fix is retained.

Files:
- src/mainwindow.cpp
- src/pianoroll.cpp
- src/keyboard.cpp
- src/renderer/gl_renderer.cpp
