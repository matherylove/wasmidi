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
let activePagedSource = null;
let pagedReadCalls = 0;
let pagedMaxRead = 0;

globalThis.__wasmidiMidiParserReadAt = (offset, count) => {
    if (!activePagedSource)
        throw new Error("Paged parser smoke source is not installed.");

    const sourceSize = activePagedSource instanceof Uint8Array
        ? activePagedSource.length
        : Number(activePagedSource.size);

    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(count) ||
        !Number.isSafeInteger(sourceSize) ||
        offset < 0 || count < 0 || offset + count > sourceSize) {
        throw new Error("Generated parser requested an invalid source window.");
    }

    ++pagedReadCalls;
    pagedMaxRead = Math.max(pagedMaxRead, count);

    if (activePagedSource instanceof Uint8Array)
        return activePagedSource.subarray(offset, offset + count);

    const result = activePagedSource.read(offset, count);
    if (!(result instanceof Uint8Array) || result.length !== count)
        throw new Error("Virtual paged source returned an invalid window.");
    return result;
};

function parsePaged(Module, source) {
    const size = source instanceof Uint8Array
        ? source.length
        : Number(source && source.size);
    if (!Number.isSafeInteger(size) || size < 0)
        throw new Error("Invalid paged smoke source size.");

    activePagedSource = source;
    pagedReadCalls = 0;
    pagedMaxRead = 0;
    try {
        return Module._wmp_parse_file_js(size);
    } finally {
        activePagedSource = null;
    }
}

function sizeNumber(value) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error("Generated parser returned an unsafe size.");
    return n;
}


function parserErrorText(Module, fallback) {
    const address = Number(Module._wmp_error_ptr_js());
    const byteLength = Number(Module._wmp_error_size_js());
    if (!Number.isSafeInteger(address) || address < 0 ||
        !Number.isSafeInteger(byteLength) || byteLength < 0 ||
        !address || !byteLength) {
        return fallback;
    }
    return new TextDecoder().decode(
        Module.HEAPU8.subarray(address, address + byteLength));
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


function encodeVarLen(value) {
    let buffer = value & 0x7f;
    const bytes = [];
    while ((value >>>= 7) !== 0) {
        buffer <<= 8;
        buffer |= ((value & 0x7f) | 0x80);
    }
    while (true) {
        bytes.push(buffer & 0xff);
        if (buffer & 0x80)
            buffer >>>= 8;
        else
            break;
    }
    return bytes;
}

function makeHugeSparseMidiSource(payloadBytes = 240 * 1024 * 1024) {
    if (!Number.isSafeInteger(payloadBytes) ||
        payloadBytes <= 0 || payloadBytes > 0x0fffffff) {
        throw new Error("Invalid virtual MIDI payload size.");
    }

    const metaPrefix = Uint8Array.from([
        0x00, 0xff, 0x7f,
        ...encodeVarLen(payloadBytes)
    ]);
    const eot = Uint8Array.from([0x00, 0xff, 0x2f, 0x00]);
    const trackLength =
        metaPrefix.length + payloadBytes +
        metaPrefix.length + payloadBytes +
        eot.length;

    const header = new Uint8Array(22);
    let o = 0;
    const put = (...values) => {
        for (const value of values) header[o++] = value;
    };
    const put32 = value => put(
        (value >>> 24) & 255,
        (value >>> 16) & 255,
        (value >>> 8) & 255,
        value & 255);

    put(0x4d, 0x54, 0x68, 0x64);
    put32(6);
    put(0x00, 0x00, 0x00, 0x01, 0x01, 0xe0);
    put(0x4d, 0x54, 0x72, 0x6b);
    put32(trackLength);

    const trackStart = header.length;
    const prefix1 = trackStart;
    const prefix2 = prefix1 + metaPrefix.length + payloadBytes;
    const eotOffset = prefix2 + metaPrefix.length + payloadBytes;
    const size = eotOffset + eot.length;

    const segments = [
        [0, header],
        [prefix1, metaPrefix],
        [prefix2, metaPrefix],
        [eotOffset, eot]
    ];

    return {
        size,
        read(offset, count) {
            const out = new Uint8Array(count); // zero-filled skipped payload
            const readEnd = offset + count;
            for (const [segmentOffset, data] of segments) {
                const segmentEnd = segmentOffset + data.length;
                const begin = Math.max(offset, segmentOffset);
                const end = Math.min(readEnd, segmentEnd);
                if (begin >= end)
                    continue;
                out.set(
                    data.subarray(begin - segmentOffset, end - segmentOffset),
                    begin - offset);
            }
            return out;
        }
    };
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
        "_wmp_parse_file_js",
        "_wmp_pack",
        "_wmp_result_ptr_js",
        "_wmp_result_size_js",
        "_wmp_release_result",
        "_wmp_build_visual_page_js",
        "_wmp_visual_page_ptr_js",
        "_wmp_visual_page_count_js",
        "_wmp_build_key_snapshot_js",
        "_wmp_key_snapshot_ptr_js",
        "_wmp_key_snapshot_word_count_js",
        "_wmp_reset_event_cursor_js",
        "_wmp_build_event_batch_js",
        "_wmp_event_batch_ptr_js",
        "_wmp_event_batch_count_js",
        "_wmp_event_batch_complete_js",
        "_wmp_tick_to_seconds_js",
        "_wmp_error_ptr_js",
        "_wmp_error_size_js",
        "_wmp_pointer_bits",
    ]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }

    parserPointerBits = Number(Module._wmp_pointer_bits()) | 0;
    if (parserPointerBits !== 64)
        throw new Error("Generated Pass 13.2 parser is not Memory64.");

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

    // Exercise the production paged-source path: raw MIDI bytes stay outside
    // the wasm heap and C++ requests only bounded source windows.
    if (!parsePaged(Module, midi)) {
        throw new Error(
            "Parser rejected smoke-test MIDI: " +
            parserErrorText(Module, "unknown parse failure"));
    }
    if (pagedReadCalls === 0 || pagedMaxRead > 4 * 1024 * 1024)
        throw new Error("Paged parser bootstrap exceeded its 4 MiB source window.");

    if (!Module._wmp_pack()) {
        throw new Error(
            "Parser could not pack smoke-test MIDI: " +
            parserErrorText(Module, "unknown pack failure"));
    }

    const resultPtr = Module._wmp_result_ptr_js();
    const resultSizeRaw = Module._wmp_result_size_js();
    if (typeof resultPtr !== "number" || typeof resultSizeRaw !== "number")
        throw new Error("Parser result facade leaked a non-Number wasm64 value.");
    const resultSize = sizeNumber(resultSizeRaw);
    if (!resultPtr || resultSize === 0)
        throw new Error("Parser produced an empty serialized document.");

    Module._wmp_release_result();

    const validProgress = parserProgressMessages.some(message =>
        message &&
        message.type === "progress" &&
        typeof message.stage === "string" &&
        message.stage.length > 0 &&
        Number.isFinite(Number(message.percent)));
    if (!validProgress) {
        throw new Error(
            "Parser smoke test did not receive a decoded string progress stage.");
    }

    // Memory64 progress callbacks must legalize const char* to Number before
    // entering EM_JS. A raw wasm64 pointer would surface as BigInt and make
    // Emscripten's UTF8ToString assert before this point.
    if (parserProgressMessages.some(message =>
            message && typeof message.stage !== "string")) {
        throw new Error(
            "Parser progress callback leaked a non-string Memory64 stage value.");
    }

    // Regression for the browser failure that motivated Pass 12.8/12.9: the old
    // Worker tried to grow a 64 MiB heap directly to ~500 MiB just to hold the
    // raw file. This virtual ~480 MiB MIDI contains two large skipped meta
    // payloads but never exists as one JS/WASM allocation. The generated module
    // must parse it through <=4 MiB windows and produce a tiny document.
    const hugeSparseMidi = makeHugeSparseMidiSource();
    if (hugeSparseMidi.size < 480 * 1024 * 1024)
        throw new Error("Huge paged-source smoke MIDI is unexpectedly small.");

    if (!parsePaged(Module, hugeSparseMidi)) {
        throw new Error(
            "Parser rejected huge paged-source smoke MIDI: " +
            parserErrorText(Module, "unknown parse failure"));
    }
    if (pagedReadCalls < 3 || pagedMaxRead > 4 * 1024 * 1024) {
        throw new Error(
            "Huge MIDI parsing escaped the 4 MiB bounded-source window.");
    }

    if (!Module._wmp_pack()) {
        throw new Error(
            "Parser could not pack huge paged-source MIDI: " +
            parserErrorText(Module, "unknown pack failure"));
    }
    const hugeResultSize = sizeNumber(Module._wmp_result_size_js());
    if (!Module._wmp_result_ptr_js() || hugeResultSize === 0 ||
        hugeResultSize > 1024 * 1024) {
        throw new Error(
            "Huge sparse MIDI produced an unexpected document size: " +
            hugeResultSize);
    }
    Module._wmp_release_result();

    // Exercise a real dense crashpoint after the bootstrap MIDI. Keeping this
    // reasonably small makes CI fast, while still covering counted keyboard
    // starts/ends, zero-length minimum duration, phased parse/pack, and reuse
    // after releasing the previous serialized result.
    const denseMidi = makeDenseCrashMidi(600000);
    if (!parsePaged(Module, denseMidi)) {
        throw new Error(
            "Parser rejected dense smoke-test MIDI: " +
            parserErrorText(Module, "unknown parse failure"));
    }

    // The dense source is >4 MiB, so this proves the generated module crossed
    // at least two windows instead of allocating/copying the entire raw file.
    if (denseMidi.length <= 4 * 1024 * 1024 ||
        pagedReadCalls < 2 ||
        pagedMaxRead > 4 * 1024 * 1024) {
        throw new Error(
            "Dense paged-source smoke test did not honor the 4 MiB window cap.");
    }

    // Pass 13's critical invariant: the parsed source remains a mapped store,
    // not a monolithic event/note document. Exercise every bounded consumer
    // directly against the same File-backed index before packing metadata.
    if (!Module._wmp_build_visual_page_js(0, 1) ||
        sizeNumber(Module._wmp_visual_page_count_js()) === 0) {
        throw new Error(
            "Mapped parser could not build a visual page after dense indexing: " +
            parserErrorText(Module, "unknown visual-page failure"));
    }

    if (!Module._wmp_build_key_snapshot_js(0) ||
        sizeNumber(Module._wmp_key_snapshot_word_count_js()) !== 384) {
        throw new Error(
            "Mapped parser did not produce the fixed 384-word keyboard state.");
    }

    Module._wmp_reset_event_cursor_js(0);
    if (!Module._wmp_build_event_batch_js(0, 65536) ||
        sizeNumber(Module._wmp_event_batch_count_js()) === 0) {
        throw new Error(
            "Mapped parser could not stream a bounded playback batch.");
    }

    if (!Module._wmp_pack()) {
        throw new Error(
            "Parser could not pack dense smoke-test MIDI: " +
            parserErrorText(Module, "unknown pack failure"));
    }

    const denseResultSize = sizeNumber(Module._wmp_result_size_js());
    if (!Module._wmp_result_ptr_js() || denseResultSize === 0)
        throw new Error("Dense parser smoke test produced an empty document.");

    // Pass 13 sends metadata only. Even 600k source notes must not make the
    // Qt wire image scale with event/note count.
    if (denseResultSize > 2 * 1024 * 1024)
        throw new Error(
            "Dense parser smoke-test document unexpectedly expanded to " +
            denseResultSize + " bytes.");

    Module._wmp_release_result();

    console.log(
        "MIDI parser Pass 13 mapped-source/streaming-render-playback smoke test OK");
}

main().catch(error => {
    console.error(error && error.stack ? error.stack : error);
    process.exitCode = 1;
});
