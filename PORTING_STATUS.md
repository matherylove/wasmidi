# WASMIDI Pass 6.1 — WebGL FBO state fix

Fixes a direct mismatch with old/MPWGL2.html:

- The legacy `_scrollAndAdvance()` unbinds its private framebuffer before
  updating the newly scrolled texture with `texSubImage2D()`.
- Pass 6 left the private back framebuffer bound while updating the same
  attached texture. In WebGL this can invalidate the strip upload, which means
  notes entering the visible window never appear.
- `initTextures()` now also restores Qt's QQuickFramebufferObject FBO instead
  of leaving the second private FBO active.

Only `src/renderer/gl_renderer.cpp` changes.
