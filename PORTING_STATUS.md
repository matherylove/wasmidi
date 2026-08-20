# WASMIDI Pass 6 — literal MPWGL2 playback/render port

This pass stops approximating the legacy player and ports its optimized model.

## Playback/scheduler
- Authoritative wall-clock remains in MainWindow.
- Scheduler cadence remains 5 ms.
- NoteOn and CC look-ahead = 250 ms.
- Resume/seek lookback = 50 ms.
- NoteOff scheduling = when <= 8 ms from end.
- Sounding-note map follows the legacy channel*128+pitch key model.

## MIDI representation
- NoteEvent now preserves original startTick/endTick.
- Seconds remain available for transport, metrics and scheduler.

## Piano-roll renderer
The instanced-note shader is replaced by the MPWGL2 texture-scroll architecture:
- playback seconds -> MIDI tick
- tick window based on NOTE_SPEED / POST_BUFFER
- full texture only on load/resize/seek/config changes
- normal playback scrolls the existing texture
- only the newly exposed right-hand strip is rebuilt
- pitch occupies the texture rows
- time occupies the texture columns
- vertical playhead is fixed at 18%
- notes move right-to-left exactly like MPWGL2
- global/per-track color mapping follows the legacy algorithm

Files changed:
- src/midi/midi_parser.hpp
- src/midi/midi_parser.cpp
- src/midi/scheduler.hpp
- src/midi/scheduler.cpp
- src/renderer/gl_renderer.hpp
- src/renderer/gl_renderer.cpp
- src/pianoroll.cpp

No files in old/ are modified.
