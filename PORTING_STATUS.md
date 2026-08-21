# WASMIDI Pass 9.4 — SnappySynthV2 pthread startup + ready race fix

Apply on top of Pass 9.3.

## Browser error fixed

Chrome showed:

    snappysynth-core.worker.js:
    TypeError: Failed to execute 'createObjectURL' on 'URL':
    Overload resolution failed.

Emscripten's generated pthread worker was receiving `urlOrBlob=undefined`.
This happens because `snappysynth-core.js` is MODULARIZE+pthreads and is loaded
with importScripts() from another browser Worker. In that context the generated
runtime cannot reliably infer the URL of its own main JS file.

The worker now instantiates SnappySynthCore with:

    mainScriptUrlOrBlob:
        new URL("./snappysynth-core.js", self.location.href).href

and `locateFile()` also returns absolute URLs derived from the outer worker URL.

## Second race fixed

The console also showed:

    SnappySynthV2 core is still starting.

`ensureBackend()` previously returned after creating the Worker, not after the
SnappySynth WASM module was ready. A fast SF2 selection therefore posted
`loadSoundfont` while `Module === null`.

The bridge now owns a `workerReadyPromise`:
- resolves only on worker message `type: "ready"`
- rejects on startup `type: "error"` or Worker.onerror
- `ensureBackend()` awaits it
- `loadSoundfontFile()` cannot send the SF2 before core initialization finishes

Files:
- web/snappysynth-worker.js
- web/snappysynth_bridge.js
