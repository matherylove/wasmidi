# WASMIDI Pass 6.3 — Qt scene-graph synchronization fix

The renderer itself was proven to work by Pass 6.2: real MIDI note pixels
became visible after a resize/window event. The freeze is caused by
QQuickFramebufferObject synchronization, not MIDI parsing or texture data.

## Root cause
`MainWindow::currentTimeChanged()` does not automatically dirty a
QQuickFramebufferObject whose `controller` property merely points at
MainWindow.

Pass 6 used `QQuickFramebufferObject::Renderer::update()` from the render
thread. That schedules additional render passes, but the GUI-side PianoRoll
item was not dirtied, so `synchronize()` kept supplying the old currentTime.
A window resize dirtied the item, causing exactly one fresh synchronized frame.

## Fix
PianoRoll now calls its GUI-side `update()` on:
- currentTimeChanged
- documentRevisionChanged
- noteSpeedChanged
- postBufferChanged
- perTrackColorsChanged
- channelColorsChanged
- playingChanged

Keyboard similarly updates on playback/document changes.

The autonomous Renderer::update() calls are removed. Every render request now
goes through Qt's normal GUI -> synchronize -> render sequence.

This package also restores the clean Pass 6.1 GL renderer, removing the
temporary Pass 6.2 red/yellow/green and magenta diagnostics while retaining
the required private-FBO restore fix.

Files changed:
- src/pianoroll.cpp
- src/keyboard.cpp
- src/renderer/gl_renderer.cpp
