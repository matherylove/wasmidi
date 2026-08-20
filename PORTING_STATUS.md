# WASMIDI Pass 6.6 — high-FPS hot-path optimization

## Ring texture
Replaces the two-texture / private-FBO scroll copy with one circular texture.
Playback advances an integer ring origin and uploads only the newly exposed
columns. The fullscreen scroll blit is eliminated.

## MPWGL2 raster density
The original rollCanvas uses CSS-pixel dimensions rather than multiplying by
devicePixelRatio. Qt supplies a DPR-scaled FBO request, so this pass divides it
back to CSS pixels when creating the roll and keyboard FBOs. Unlike the old
viewport mismatch, the FBO itself and renderer texture now have the same size.

## Keyboard dirty-only
MainWindow caches the 128-key active mask and emits activePitchesChanged only
when key state changes. Keyboard no longer renders continuously.

## Neural background
The expensive QML Canvas is frozen while MIDI is playing. It resumes at 30 Hz
when idle/paused.

## Charts
All mini charts and the NPS timeline share one 33 ms timer.

## FPS
PianoRollRenderer measures completed C++ render() calls directly. QML samples
that number every 500 ms without FrameAnimation or another frame-driving loop.
