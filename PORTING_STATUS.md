# WASMIDI Port — Pass 4 (Playback parity + vertical roll)

This overlay is intended to be applied on top of Pass 3. `old/` remains untouched.

## Fixed in this pass

- Restores the WebGL keyboard using the framebuffer construction that was known to render correctly in Qt/WASM.
- Piano-roll notes now use a vertical falling-note layout: MIDI pitch maps to X and time maps to Y.
- Falling notes are aligned to the real 75-white-key piano geometry rather than 128 equal-width columns.
- Playback uses a wall-clock-driven ~60 Hz visual/player loop, matching the requestAnimationFrame model from MPWGL2.
- MIDI scheduler now runs on a 5 ms precise timer with a 250 ms look-ahead and 50 ms lookback.
- Scheduler no longer advances its clock by the entire 250 ms horizon every scheduler callback.
- Live NPS now matches MPWGL2: starts in the previous 250 ms multiplied by 4.
- Live polyphony matches MPWGL2: upperBound(starts,t) - lowerBound(ends,t).
- CC/s is now a moving one-second window rather than a coarse integer-second bucket.
- BPM follows tempo changes at the current playback time.
- Mini-chart history now uses 280 samples at 30 Hz and repaints at ~60 Hz, matching MPWGL2's `_HIST_LEN`, `_HIST_RATE`, and RAF drawing cadence.
- Mini charts now use the original color mapping and filled-area visual treatment.
- NPS Timeline is a file-wide half-second limits chart with playback and peak markers.
- Peak NPS follows the old one-second / 100 ms sampled calculation.
- Peak polyphony keeps the old large-file sampling safeguard.
- EOF behavior returns transport to 0 like `stopPlayback()` in MPWGL2.

## Not faked

The 5 ms scheduler now exposes the correct timestamped event window, but Web MIDI transmission and the embedded SnappySynth/SF2 audio engine still need to be connected to that native event stream. The visual/player timing core no longer depends on those unfinished output backends.
