# WASMIDI Pass 9.2 — exact unified MIDI/SF2 picker

Apply on top of Pass 9.1.

The separate SF2 picker path has been removed completely.

Both:
- MainWindow::openMidiPicker()
- MainWindow::openSoundfontPicker()

now call the SAME Emscripten function:

    wasmidi_browser_open_file_picker(kind)

with:
- kind=0 for MIDI
- kind=1 for SF2

The DOM code, input creation, append-to-body, synchronous input.click(), and
cleanup path are identical. Only the accept filter and post-selection handler
differ.

A fresh input element is created for every click. No input is shared with the
SnappySynth bridge and there is no stale picker state.

No AudioContext, Worker, Promise, or SnappySynth initialization happens before
input.click().

This means that if the MIDI browser prompt opens on a given browser, SF2 now
uses exactly that same working prompt mechanism.
