/* global WasmidiMidiParserCore */

"use strict";

// Pass 12.5: parser is a Memory64 module. Chromium 133+ can grow one wasm64
// memory to 16 GiB; physical pages are committed on demand, so the effective
// limit below that is whatever the browser/OS can actually provide.
const WASMIDI_MIDI_PARSER_BOOTSTRAP = "12.5";
const RESULT_CHUNK_BYTES = 16 * 1024 * 1024;

self.__wasmidiMidiParserStage = "Loading parser core";
self.__wasmidiMidiParserPercent = 14;

importScripts(
    "./wasmidi-midi-parser.js?v=" +
    encodeURIComponent(WASMIDI_MIDI_PARSER_BOOTSTRAP));

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

function abiSize(value) {
    const n = Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error("MIDI size exceeds JavaScript's exact integer range.");
    return isMemory64() ? BigInt(n) : n;
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
                typeof Module._wmp_parse !== "function" ||
                typeof Module._wmp_pack !== "function" ||
                typeof Module._wmp_result_ptr !== "function" ||
                typeof Module._wmp_result_size !== "function" ||
                typeof Module._wmp_release_result !== "function" ||
                typeof Module._wmp_error_ptr !== "function" ||
                typeof Module._wmp_pointer_bits !== "function" ||
                typeof Module._malloc !== "function" ||
                typeof Module._free !== "function" ||
                !Module.HEAPU8) {
                throw new Error(
                    "parser WASM initialized without its complete exported API");
            }

            pointerBits = Number(Module._wmp_pointer_bits()) | 0;
            if (pointerBits !== 64) {
                throw new Error(
                    "Pass 12.5 parser was built without Memory64 (pointer width " +
                    pointerBits + ").");
            }

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

async function streamFileIntoWasm(file, Module, inputPtr) {
    const total = Number(file.size);
    if (!Number.isSafeInteger(total) || total < 0)
        throw new Error("Invalid MIDI file size.");

    const base = pointerToNumber(inputPtr, "MIDI input pointer");
    let offset = 0;

    progress(1, "Streaming MIDI directly into parser memory");

    if (file.stream && total > 0) {
        const reader = file.stream().getReader();
        try {
            while (true) {
                const result = await reader.read();
                if (result.done)
                    break;

                const chunk = result.value;
                if (!chunk || offset + chunk.byteLength > total)
                    throw new Error("MIDI file changed while it was being read.");

                // Do not retain a second file-sized JS Uint8Array. The File
                // stream writes each chunk straight into the already-grown
                // Memory64 heap.
                Module.HEAPU8.set(chunk, base + offset);
                offset += chunk.byteLength;

                progress(
                    1 + Math.floor(13 * offset / Math.max(1, total)),
                    "Streaming MIDI directly into parser memory");

                if ((offset & ((16 * 1024 * 1024) - 1)) === 0)
                    await new Promise(resolve => setTimeout(resolve, 0));
            }
        } finally {
            try { reader.releaseLock(); } catch (_) {}
        }
    } else {
        // Fallback only for browsers without File.stream(). This path may need a
        // transient ArrayBuffer, but never an additional persistent copy.
        const source = new Uint8Array(await file.arrayBuffer());
        if (source.length !== total)
            throw new Error("MIDI file size changed while loading.");
        Module.HEAPU8.set(source, base);
        offset = source.length;
    }

    if (offset !== total)
        throw new Error("MIDI file ended before the advertised size.");

    progress(15, "Starting MIDI parser");
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

self.onmessage = async event => {
    const message = event.data || {};
    if (message.type !== "parse" || !message.file)
        return;

    let inputPtr = null;
    let Module = null;
    let resultOwned = false;

    try {
        const file = message.file;
        const total = Number(file.size);
        if (!Number.isSafeInteger(total) || total < 0)
            throw new Error("Invalid MIDI file size.");

        Module = await getModule();

        self.__wasmidiMidiParserStage = "Allocating parser memory on demand";
        inputPtr = Module._malloc(abiSize(Math.max(1, total)));
        if (!inputPtr) {
            throw new Error(
                "The browser/OS could not commit enough Memory64 pages for this MIDI. " +
                "There is no lower WASMIDI RAM cap; this is the effective process/system memory limit.");
        }

        await streamFileIntoWasm(file, Module, inputPtr);

        self.__wasmidiMidiParserStage = "Parsing MIDI";
        const ok = Module._wmp_parse(inputPtr, abiSize(total));
        if (!ok) {
            const errorPtr = Module._wmp_error_ptr();
            const error = errorPtr
                ? Module.UTF8ToString(errorPtr)
                : "Could not parse MIDI";
            throw new Error(error);
        }

        // Parsing owns everything needed. Release the raw file allocation before
        // packing so input + document + wire image are never all resident.
        Module._free(inputPtr);
        inputPtr = null;

        self.__wasmidiMidiParserStage = "Packing parsed MIDI";
        const packed = Module._wmp_pack();
        if (!packed) {
            const errorPtr = Module._wmp_error_ptr();
            const error = errorPtr
                ? Module.UTF8ToString(errorPtr)
                : "Could not pack parsed MIDI";
            throw new Error(error);
        }
        resultOwned = true;

        progress(95, "Streaming parsed MIDI to player");

        await streamPackedResult(
            Module,
            file,
            Module._wmp_result_ptr(),
            Module._wmp_result_size());

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
        if (inputPtr && Module) {
            try { Module._free(inputPtr); } catch (_) {}
        }
        if (resultOwned && Module) {
            try { Module._wmp_release_result(); } catch (_) {}
        }
    }
};
