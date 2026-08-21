# WASMIDI Pass 9.3 — GitHub Pages SnappySynthV2 deployment fix

Apply on top of Pass 9.x.

## Root cause

The active `.github/workflows/build-wasm.yml` in the repository was still the
old Qt-only deployment. It published:
- wasmidi.html / wasmidi.js / wasmidi.wasm
but did NOT publish the SnappySynthV2 runtime payload.

Selecting an SF2 therefore worked, but the browser could not reliably start the
SnappySynth worker/core, so the UI remained at "No SoundFont loaded".

## This overlay restores the complete deployment

GitHub Pages now publishes and verifies:
- snappysynth-core.js
- snappysynth-core.wasm
- snappysynth-core.worker.js
- snappysynth-worker.js
- snappysynth-audio-worklet.js
- snappysynth_bridge.js
- coi-serviceworker.js

`index.html` is patched during Actions to load:
1. coi-serviceworker.js
2. snappysynth_bridge.js

## Why COI remains required

The supplied SnappySynthV2 voice engine internally creates worker threads via
its Win32 compatibility layer (`_beginthreadex` -> pthread). Therefore its
Emscripten core correctly remains a pthread build. GitHub Pages does not emit
COOP/COEP headers itself, so the same-origin service worker injects them.

On the first deployment/navigation the page may reload once while the service
worker takes control. After that `crossOriginIsolated` should be true and SF2
loading can proceed.

GitHub Actions now fails before deployment if any required SnappySynthV2 file
is missing.
