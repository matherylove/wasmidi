# WASMIDI Pass 6.2 — consolidated FBO fix + renderer diagnostics

This overlay contains the Pass 6.1 framebuffer restore fix and adds temporary
diagnostics so the next browser run identifies the exact failure stage.

## Visible indicators
A 12x12 square appears at the upper-left of the piano-roll FBO:
- RED: private WebGL framebuffer is incomplete.
- YELLOW: notes are loaded but the renderer has not generated any MIDI pixels.
- GREEN: the renderer has generated real MIDI pixels.

An 8x8 MAGENTA marker is also inserted into the generated scroll texture at
the upper-right. This marker travels through the exact same texture/blit path
as MIDI notes.

Interpretation:
- GREEN square + magenta marker visible + no notes => tick/window/note geometry.
- GREEN square + magenta marker absent => texture sampling/blit problem.
- YELLOW square => writeStrip/filter/tick range problem.
- RED square => private FBO creation problem.

## Browser console
Every ~60 render frames it emits:
[WASMIDI-ROLL] notes=... firstTick=... lastTick=... currentTime=...
currentTick=... windowTicks=... postTicks=... tpc=... search=...
writtenNotes=... writtenPixels=... tex=... fbo=...

Only src/renderer/gl_renderer.cpp changes.
