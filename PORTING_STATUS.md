# WASMIDI Pass 9.1 — SF2 native picker fix

Apply on top of Pass 9.

## Fixed

The Load SF2 button previously called `WasmidiSnappyBridge.openSoundfont()`.
If `snappysynth_bridge.js` had not initialized yet, the C++ EM_JS function
silently returned and no Windows/browser file picker appeared.

Pass 9.1 makes the browser `<input type=file>` picker owned by the synchronous
EM_JS call reached directly from the QML click gesture. Therefore the SF2
dialog no longer depends on AudioContext, Worker, COI, or bridge readiness.

After a file is selected:
1. the code uses `WasmidiSnappyBridge.loadSoundfontFile(file)`;
2. if the bridge script is unexpectedly missing, it loads
   `snappysynth_bridge.js` dynamically and then passes the already-selected
   File object to it;
3. selecting the same SF2 twice is supported.

The bridge's existing `openSoundfont()` remains available as a compatibility
fallback, but WASMIDI's QML button no longer relies on it.

Files:
- src/mainwindow.cpp
- web/snappysynth_bridge.js
