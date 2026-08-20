# WASMIDI Pass 7 — parser/RAM and visual parity

- Removes the browser-file QByteArray copy: selected file bytes are parsed
  directly from the temporary WASM HEAP allocation.
- Parser now follows MPWGL2's two-pass strategy, pre-counts notes/controls,
  reserves once, and uses a fixed 65536-bucket contiguous pending hash instead
  of unordered_map<key,deque>.
- Repeated same-pitch NoteOns remain stacked correctly.
- Large-note sorting is in-place to avoid another complete Black-MIDI buffer.
- pitchStarts_/pitchEnds_ duplicate arrays are removed.
- GLRenderer no longer clones the complete note list; it views document.notes.
- Piano-roll raster uses CSS-pixel dimensions like MPWGL2.html.
- Keyboard active window is t-0.03..t+0.05 and each active key uses its real
  global/per-track channel color.
- Dominant screen color uses t-0.05..t+0.15.
- Neural activity follows liveNps/peakNps with 0.7/0.3 smoothing.
- Neural background restores 95 nodes, hue interpolation, activity-controlled
  speed, connection intensity and gradient blobs from the original.

This keeps the existing Qt interface and WebGL roll while removing the largest
unnecessary data duplication. The next parser step, if required after testing,
is restoring the separate browser Worker so parsing itself never blocks the Qt
GUI thread.
