# WASMIDI Pass 8.2.2 — horizontal roll pipeline recovery

Apply on top of Pass 8.2.1.

A standalone renderer logic test proves the compact sweep/ring is producing
non-zero note draw instances correctly. The remaining failure is therefore in
the WebGL/FBO pipeline introduced by Pass 8.2.

Changes:
- Restores the proven color-only QQuickFramebufferObject used by the earlier
  working renderer; CombinedDepthStencil is no longer required.
- Disables depth testing for note rendering so depth attachment/state cannot
  hide the horizontal roll.
- Note shader uses explicit z=0.
- The note shader is now the only mandatory GLSL program.
- Background and neural shaders are optional. If either is rejected, notes
  still initialize and render; background falls back to the original dark fill.
- Keeps Pass 8/8.2 compact parser, TickGroup engine, scheduler removal, split
  open/completed VBOs, contiguous uploads, GPU neural path when supported, and
  single render-loop architecture.

Files:
- src/renderer/gl_renderer.cpp
- src/pianoroll.cpp
