# WASMIDI Pass 8.3 — MPWGL2 visual order + exact NPS + numeric fit

Apply on top of Pass 8.2.2.

## Why the notes looked out of order

Pass 8/8.2 optimized visual state by merging NoteOns by channel+pitch and by
appending completed notes at NoteOff time. Both differ from MPWGL2:

- MPWGL2 pairs notes by (track, channel, pitch), FIFO.
- Every NoteOn remains an independent visual note.
- The final note buffer is stable-sorted by note START time.
- _writeStrip walks that start order, so later-starting notes overwrite earlier
  notes where rectangles overlap.

Pass 8.3 restores those semantics without restoring the old CPU texture path.

## New visual representation

`VisualNote` is 12 bytes:
- uint32 startTick
- uint32 endTick
- uint32 packedData (velocity, pitch, global color, per-track color)

A temporary uint16/event track side-buffer exists only while loading. It is
discarded before MidiDocument is returned, so steady-state event RAM remains
CompactEvent + TickGroup + VisualNote.

Visual notes are generated in global NoteOn order and paired FIFO using exact
(track,channel,pitch) keys. No visual sort is needed.

## Renderer

- Removes runtime NoteOn/NoteOff merging from the GL hot path.
- VBO ring contains an immutable contiguous slice of the start-ordered
  VisualNote stream.
- Forward playback uploads only newly entering source notes.
- No random NoteOff VBO writes.
- No per-frame sort.
- Draw calls preserve source/start order exactly like MPWGL2.
- X positions are snapped to CSS-pixel columns like Math.round() in _writeStrip.
- Pitch rows use pitch/127 and 1/128 row height to match MPWGL2 geometry.
- Existing optimized GPU neural background remains.

## NPS

Live NPS now implements MPWGL2 literally:
  round((upperBound(starts,t)-lowerBound(starts,t-0.25))*4)

The binary searches run directly on VisualNote.startTick with fractional
secondsToTick boundaries, avoiding the floor-rounded TickGroup approximation.

Peak NPS also restores MPWGL2's 0.1-second sampling cadence over a 1-second
window.

## Numbers

Live cards, mini-chart values, timeline NPS and File Info numeric values now use
Text.HorizontalFit + minimumPixelSize instead of clipping/eliding large numbers.
