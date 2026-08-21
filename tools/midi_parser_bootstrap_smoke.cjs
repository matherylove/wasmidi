"use strict";

// Executes the *generated* modularized parser exactly enough to catch startup
// aborts before GitHub Pages deployment. The target includes ENVIRONMENT=node
// solely for this CI test; production still imports the same file in a Worker.
const fs = require("fs");
const path = require("path");

// Emscripten is intentionally built for both `worker` and `node`. The parser's
// EM_JS progress callbacks are optional in Node, but providing the tiny Worker
// surface here makes the smoke test exercise the same callback path that Pages
// uses instead of failing merely because Node has no `self`/`postMessage`.
const parserProgressMessages = [];
if (typeof globalThis.self === "undefined")
    globalThis.self = globalThis;
if (typeof globalThis.postMessage !== "function") {
    globalThis.postMessage = message => {
        parserProgressMessages.push(message);
    };
}

let parserPointerBits = 0;

function abiSize(value) {
    return parserPointerBits === 64 ? BigInt(value) : value;
}

function ptrNumber(value) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error("Generated parser returned a non-addressable pointer.");
    return n;
}

function sizeNumber(value) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error("Generated parser returned an unsafe size.");
    return n;
}

function makeDenseCrashMidi(noteCount) {
    // Format-0, one tick containing NOTECOUNT NoteOns followed by matching
    // NoteOffs. This exercises the black-MIDI keyboard compression path that
    // previously allocated an 8-byte sort key for every visual note.
    const trackLength = noteCount * 8 + 4;
    const bytes = new Uint8Array(14 + 8 + trackLength);
    let o = 0;
    const put = (...values) => {
        for (const value of values) bytes[o++] = value;
    };
    const put32 = value => {
        put(
            (value >>> 24) & 255,
            (value >>> 16) & 255,
            (value >>> 8) & 255,
            value & 255);
    };

    put(0x4d, 0x54, 0x68, 0x64);
    put32(6);
    put(0x00, 0x00, 0x00, 0x01, 0x01, 0xe0);
    put(0x4d, 0x54, 0x72, 0x6b);
    put32(trackLength);

    for (let i = 0; i < noteCount; ++i)
        put(0x00, 0x90, 60 + (i & 3), 100);
    for (let i = 0; i < noteCount; ++i)
        put(0x00, 0x80, 60 + (i & 3), 0);

    put(0x00, 0xff, 0x2f, 0x00);
    if (o !== bytes.length)
        throw new Error("Dense MIDI smoke generator length mismatch.");
    return bytes;
}

async function main() {
    const jsPath = path.resolve(
        process.argv[2] ||
        path.join("build", "wasmidi-midi-parser.js"));

    if (!fs.existsSync(jsPath))
        throw new Error("Missing generated parser module: " + jsPath);

    delete require.cache[require.resolve(jsPath)];
    const createParser = require(jsPath);

    if (typeof createParser !== "function")
        throw new Error("Generated parser did not export a module factory.");

    let abortReason = "";
    const Module = await createParser({
        noInitialRun: true,
        noExitRuntime: true,
        onAbort(reason) {
            abortReason = String(reason || "unknown abort");
        },
        print() {},
        printErr(text) {
            if (text)
                process.stderr.write(String(text) + "\n");
        }
    });

    if (abortReason)
        throw new Error("Parser aborted during bootstrap: " + abortReason);

    for (const name of [
        "_wmp_parse",
        "_wmp_pack",
        "_wmp_result_ptr",
        "_wmp_result_size",
        "_wmp_release_result",
        "_wmp_error_ptr",
        "_wmp_pointer_bits",
        "_malloc",
        "_free"
    ]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }

    parserPointerBits = Number(Module._wmp_pointer_bits()) | 0;
    if (parserPointerBits !== 64)
        throw new Error("Generated Pass 12.5 parser is not Memory64.");

    // Valid format-0 MIDI: header + one track containing only EndOfTrack.
    const midi = Uint8Array.from([
        0x4d, 0x54, 0x68, 0x64,
        0x00, 0x00, 0x00, 0x06,
        0x00, 0x00,
        0x00, 0x01,
        0x01, 0xe0,
        0x4d, 0x54, 0x72, 0x6b,
        0x00, 0x00, 0x00, 0x04,
        0x00, 0xff, 0x2f, 0x00
    ]);

    const ptr = Module._malloc(abiSize(midi.length));
    if (!ptr)
        throw new Error("Parser smoke test malloc failed.");

    try {
        Module.HEAPU8.set(midi, ptrNumber(ptr));
        if (!Module._wmp_parse(ptr, abiSize(midi.length))) {
            const ep = Module._wmp_error_ptr();
            const detail = ep
                ? Module.UTF8ToString(ep)
                : "unknown parse failure";
            throw new Error("Parser rejected smoke-test MIDI: " + detail);
        }
    } finally {
        // Production frees the raw MIDI allocation before packing too. Keep the
        // smoke test on the same lifetime boundary so CI exercises that API.
        Module._free(ptr);
    }

    if (!Module._wmp_pack()) {
        const ep = Module._wmp_error_ptr();
        const detail = ep
            ? Module.UTF8ToString(ep)
            : "unknown pack failure";
        throw new Error("Parser could not pack smoke-test MIDI: " + detail);
    }

    const resultPtr = Module._wmp_result_ptr();
    const resultSize = sizeNumber(Module._wmp_result_size());
    if (!resultPtr || resultSize === 0)
        throw new Error("Parser produced an empty serialized document.");

    Module._wmp_release_result();

    if (!parserProgressMessages.some(message =>
            message && message.type === "progress")) {
        throw new Error(
            "Parser smoke test completed without exercising progress callbacks.");
    }

    // Exercise a real dense crashpoint after the bootstrap MIDI. Keeping this
    // reasonably small makes CI fast, while still covering counted keyboard
    // starts/ends, zero-length minimum duration, phased parse/pack, and reuse
    // after releasing the previous serialized result.
    const denseMidi = makeDenseCrashMidi(250000);
    const densePtr = Module._malloc(abiSize(denseMidi.length));
    if (!densePtr)
        throw new Error("Dense parser smoke-test malloc failed.");

    try {
        Module.HEAPU8.set(denseMidi, ptrNumber(densePtr));
        if (!Module._wmp_parse(densePtr, abiSize(denseMidi.length))) {
            const ep = Module._wmp_error_ptr();
            throw new Error(
                "Parser rejected dense smoke-test MIDI: " +
                (ep ? Module.UTF8ToString(ep) : "unknown parse failure"));
        }
    } finally {
        Module._free(densePtr);
    }

    if (!Module._wmp_pack()) {
        const ep = Module._wmp_error_ptr();
        throw new Error(
            "Parser could not pack dense smoke-test MIDI: " +
            (ep ? Module.UTF8ToString(ep) : "unknown pack failure"));
    }

    const denseResultSize = sizeNumber(Module._wmp_result_size());
    if (!Module._wmp_result_ptr() || denseResultSize === 0)
        throw new Error("Dense parser smoke test produced an empty document.");

    // 250k duplicated crashpoint notes should remain compact enough that the
    // final wire image is dominated by the authoritative event/visual streams,
    // not by an accidental one-event-per-note keyboard index.
    if (denseResultSize > 8 * 1024 * 1024)
        throw new Error(
            "Dense parser smoke-test document unexpectedly expanded to " +
            denseResultSize + " bytes.");

    Module._wmp_release_result();

    console.log(
        "MIDI parser Memory64 bootstrap/dense parse smoke test OK");
}

main().catch(error => {
    console.error(error && error.stack ? error.stack : error);
    process.exitCode = 1;
});
