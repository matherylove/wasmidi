/* global WasmidiMidiParserCore */

"use strict";

// Pass 12.9: parser is a Memory64 module with a Number-only JS ABI. Chromium 133+ can grow one wasm64
// memory to 16 GiB; physical pages are committed on demand, so the effective
// limit below that is whatever the browser/OS can actually provide.
const WASMIDI_MIDI_PARSER_BOOTSTRAP = "12.9";
const RESULT_CHUNK_BYTES = 16 * 1024 * 1024;

self.__wasmidiMidiParserStage = "Loading parser core";
self.__wasmidiMidiParserPercent = 14;

// When MainWindow fetches this Worker with cache: no-store and launches it
// from a Blob URL, relative importScripts() would resolve against blob:. Keep
// the real deployment directory in a tiny prelude so the generated parser
// core is always fetched from the current site and current bootstrap version.
const WASMIDI_MIDI_PARSER_BASE_URL = (() => {
    const configured = self.__wasmidiMidiParserBaseUrl;
    if (configured)
        return String(configured);
    try {
        return new URL("./", self.location.href).href;
    } catch (_) {
        return "./";
    }
})();

importScripts(new URL(
    "wasmidi-midi-parser.js?v=" +
        encodeURIComponent(WASMIDI_MIDI_PARSER_BOOTSTRAP),
    WASMIDI_MIDI_PARSER_BASE_URL).href);

let modulePromise = null;
let lastAbortReason = "";
let lastRuntimeError = "";
let pointerBits = 0;

function currentStage() {
    return String(
        self.__wasmidiMidiParserStage ||
        "Initializing parser core");
}

function conciseError(value) {
    if (!value)
        return "";
    if (value instanceof Error)
        return value.message || String(value);
    return String(value);
}

function enrichedCoreError(error) {
    const primary = conciseError(error);
    const detail =
        lastAbortReason ||
        lastRuntimeError ||
        primary ||
        "unknown parser runtime error";

    return new Error(
        "Background MIDI parser failed while " +
        currentStage() +
        ": " + detail);
}

function isMemory64() {
    return pointerBits === 64;
}


function pointerToNumber(value, label) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error((label || "WASM pointer") + " exceeds JavaScript's exact integer range.");
    return n;
}

function sizeToNumber(value, label) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error((label || "WASM size") + " exceeds JavaScript's exact integer range.");
    return n;
}


function parserErrorText(Module, fallback) {
    const address = Number(Module._wmp_error_ptr_js());
    const byteLength = Number(Module._wmp_error_size_js());

    if (!Number.isSafeInteger(address) || address < 0 ||
        !Number.isSafeInteger(byteLength) || byteLength < 0 ||
        !address || !byteLength) {
        return String(fallback || "Could not parse MIDI");
    }

    const end = address + byteLength;
    if (!Number.isSafeInteger(end) || end > Module.HEAPU8.length)
        return String(fallback || "Could not parse MIDI");

    try {
        return new TextDecoder("utf-8", { fatal: false }).decode(
            Module.HEAPU8.subarray(address, end));
    } catch (_) {
        return String(fallback || "Could not parse MIDI");
    }
}

function getModule() {
    if (!modulePromise) {
        lastAbortReason = "";
        lastRuntimeError = "";
        self.__wasmidiMidiParserStage = "Initializing Memory64 parser core";
        self.__wasmidiMidiParserPercent = 15;

        modulePromise = Promise.resolve().then(() =>
            WasmidiMidiParserCore({
                noInitialRun: true,
                noExitRuntime: true,
                onAbort(reason) {
                    lastAbortReason =
                        conciseError(reason) ||
                        "Emscripten aborted without a reason";
                    console.error(
                        "[WASMIDI MIDI parser] abort during " +
                        currentStage() + ":",
                        lastAbortReason);
                },
                print(text) {
                    if (text)
                        console.log("[WASMIDI MIDI parser]", text);
                },
                printErr(text) {
                    if (!text)
                        return;
                    lastRuntimeError = String(text);
                    console.error(
                        "[WASMIDI MIDI parser runtime]",
                        text);
                }
            })
        ).then(Module => {
            if (!Module ||
                typeof Module._wmp_alloc_js !== "function" ||
                typeof Module._wmp_free_js !== "function" ||
                typeof Module._wmp_parse_js !== "function" ||
                typeof Module._wmp_parse_file_js !== "function" ||
                typeof Module._wmp_pack !== "function" ||
                typeof Module._wmp_result_ptr_js !== "function" ||
                typeof Module._wmp_result_size_js !== "function" ||
                typeof Module._wmp_release_result !== "function" ||
                typeof Module._wmp_error_ptr_js !== "function" ||
                typeof Module._wmp_error_size_js !== "function" ||
                typeof Module._wmp_pointer_bits !== "function" ||
                !Module.HEAPU8) {
                throw new Error(
                    "parser WASM initialized without its complete exported API");
            }

            pointerBits = Number(Module._wmp_pointer_bits()) | 0;
            if (pointerBits !== 64) {
                throw new Error(
                    "Pass 12.8 parser was built without Memory64 (pointer width " +
                    pointerBits + ").");
            }

            // Probe the public ABI before accepting any user file. The facade
            // must remain Number-only even though the module itself is wasm64.
            const abiProbe = Module._wmp_alloc_js(1);
            if (typeof abiProbe !== "number" ||
                !Number.isSafeInteger(abiProbe) || abiProbe <= 0) {
                throw new Error(
                    "Memory64 parser JS facade did not return a Number address.");
            }
            Module._wmp_free_js(abiProbe);

            self.__wasmidiMidiParserStage = "Memory64 parser core ready";
            self.__wasmidiMidiParserPercent = 15;
            return Module;
        }).catch(error => {
            modulePromise = null;
            pointerBits = 0;
            throw enrichedCoreError(error);
        });
    }
    return modulePromise;
}

function progress(percent, stage) {
    const bounded = Math.max(0, Math.min(100, percent | 0));
    const text = String(stage || "Loading MIDI");
    self.__wasmidiMidiParserStage = text;
    self.__wasmidiMidiParserPercent = bounded;
    postMessage({
        type: "progress",
        percent: bounded,
        stage: text
    });
}

async function streamPackedResult(Module, file, resultPtr, resultSize) {
    const base = pointerToNumber(resultPtr, "Parsed-document pointer");
    const total = sizeToNumber(resultSize, "Parsed-document size");

    if (!base || total <= 0)
        throw new Error("MIDI parser returned an empty document.");

    postMessage({
        type: "result-begin",
        name: String(file.name || "browser.mid"),
        size: total,
        parserPointerBits: pointerBits
    });

    for (let offset = 0; offset < total; offset += RESULT_CHUNK_BYTES) {
        const end = Math.min(total, offset + RESULT_CHUNK_BYTES);
        const chunk = Module.HEAPU8.slice(base + offset, base + end);

        postMessage({
            type: "result-chunk",
            offset,
            data: chunk.buffer
        }, [chunk.buffer]);

        if ((offset & ((64 * 1024 * 1024) - 1)) === 0)
            await new Promise(resolve => setTimeout(resolve, 0));
    }

    postMessage({
        type: "result-end",
        name: String(file.name || "browser.mid"),
        size: total
    });
}

// Explicit source-version handshake. MainWindow does not send the File until
// this arrives, so a stale Worker can never silently execute an older loading
// path after a GitHub Pages deployment.
postMessage({
    type: "worker-ready",
    bootstrap: WASMIDI_MIDI_PARSER_BOOTSTRAP,
    pagedSource: true
});

self.onmessage = async event => {
    const message = event.data || {};
    if (message.type !== "parse" || !message.file)
        return;

    let Module = null;
    let resultOwned = false;

    try {
        const file = message.file;
        const total = Number(file.size);
        if (!Number.isSafeInteger(total) || total < 0)
            throw new Error("Invalid MIDI file size.");

        Module = await getModule();

        // Pass 12.9: keep the browser File outside the wasm heap. C++ requests
        // only bounded 4 MiB windows through FileReaderSync as its two parser
        // passes move through the track ranges. A 500 MB/5 GB source therefore
        // does not require a 500 MB/5 GB raw allocation before parsing begins.
        self.__wasmidiMidiParserFile = file;
        self.__wasmidiMidiParserFileReader = null;
        self.__wasmidiMidiParserReadError = "";
        self.__wasmidiMidiParserStage = "Parsing MIDI from bounded file windows";
        progress(1, "Opening MIDI as a paged source");

        const ok = Module._wmp_parse_file_js(total);
        if (!ok) {
            const parserText = parserErrorText(Module, "Could not parse MIDI");
            const readText = String(self.__wasmidiMidiParserReadError || "");
            throw new Error(
                readText && !parserText.includes(readText)
                    ? parserText + ": " + readText
                    : parserText);
        }

        // The C++ document is now independent of the File source. Drop all
        // browser-side source references before allocating the packed result.
        self.__wasmidiMidiParserFile = null;
        self.__wasmidiMidiParserFileReader = null;

        self.__wasmidiMidiParserStage = "Packing parsed MIDI";
        const packed = Module._wmp_pack();
        if (!packed) {
            throw new Error(
                parserErrorText(Module, "Could not pack parsed MIDI"));
        }
        resultOwned = true;

        progress(95, "Streaming parsed MIDI to player");

        await streamPackedResult(
            Module,
            file,
            Module._wmp_result_ptr_js(),
            Module._wmp_result_size_js());

        // Every chunk has been copied to a transferable buffer. Drop the giant
        // parser-side capacity immediately; the next MIDI starts from a clean
        // memory budget.
        Module._wmp_release_result();
        resultOwned = false;

        progress(96, "Installing parsed MIDI");
    } catch (error) {
        const failure =
            (lastAbortReason || lastRuntimeError)
                ? enrichedCoreError(error)
                : error;

        postMessage({
            type: "error",
            message: failure && failure.message
                ? failure.message
                : String(failure || "Could not parse MIDI")
        });
    } finally {
        self.__wasmidiMidiParserFile = null;
        self.__wasmidiMidiParserFileReader = null;
        self.__wasmidiMidiParserReadAt = null;
        if (resultOwned && Module) {
            try { Module._wmp_release_result(); } catch (_) {}
        }
    }
};
