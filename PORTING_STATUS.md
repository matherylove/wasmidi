# WASMIDI Pass 8 — compact tick engine + GPU note ring

This pass changes the internal engine while keeping the existing QML/UI look.

## Loading / RAM
- Browser File API now streams chunks directly into one WASM allocation.
  `file.arrayBuffer()` is no longer required on modern browsers, removing a
  second full-file-sized JS allocation during load.
- MIDI parser no longer constructs `NoteEvent` objects.
- Authoritative channel events are 4-byte `CompactEvent` values.
- Events are indexed by sparse `TickGroup` entries.
- Pass 1 counts/indexes track ticks, notes and controls.
- A k-way merge combines track histograms without allocating maxTick+1 slots.
- Pass 2 writes events directly into their final global tick-group positions.
- There is no global note pairing and no global note sort.
- Both global/channel and per-track color slots are packed into one byte.
- Tempo and SysEx remain separate.

## Playback
- Scheduler walks the same TickGroup/event stream directly.
- Removes `unordered_map` sounding-note state and separate note/control cursors.
- Seeks are binary-searches into TickGroup.

## Renderer
- Removes CPU note rasterization, strip buffers, scroll textures and private
  scroll FBOs entirely.
- Visible notes are represented as 12-byte `RenderNote` instances.
- A CPU/GPU ring keeps only the visual working set.
- Incremental sweep appends only newly entering notes.
- NoteOff end-tick changes are batched/coalesced before `glBufferSubData`.
- WebGL vertex shader converts start/end tick and pitch directly to geometry.
- Normally one or two instanced draw calls render the complete note roll.
- The playhead is drawn with scissor/clear; no second shader/QML item.
- Tempo-dependent tick width is recalculated at the current playback position.

## UI stats / keyboard
- Removes noteStarts_, noteEnds_ and controlTimes_ full-size duplicates.
- Peak NPS and timeline are derived from TickGroup counts.
- Peak polyphony is an exact one-pass CompactEvent calculation, with no Edge
  vector and no sorting.
- Live NPS/CC use incremental sliding TickGroup windows.
- Keyboard state is incrementally maintained from the same event stream.
- Keyboard redraws only when active keys/colors or reactive hue actually change.
- Existing active-note palette and neural background inputs are preserved.

## Files
- src/midi/midi_parser.hpp
- src/midi/midi_parser.cpp
- src/midi/scheduler.hpp
- src/midi/scheduler.cpp
- src/renderer/gl_renderer.hpp
- src/renderer/gl_renderer.cpp
- src/mainwindow.hpp
- src/mainwindow.cpp
- src/pianoroll.cpp
- src/keyboard.cpp

No QML layout/theme file is changed.
