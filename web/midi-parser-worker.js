/* global WasmidiMidiParserCore */

"use strict";

// Bump this whenever the generated parser ABI/bootstrap changes. The parser is
// SINGLE_FILE, so this one cache-busted URL contains both glue and WASM bytes.
const WASMIDI_MIDI_PARSER_BOOTSTRAP = "12.2";

self.__wasmidiMidiParserStage = "Loading parser core";
self.__wasmidiMidiParserPercent = 14;

importScripts(
    "./wasmidi-midi-parser.js?v=" +
    encodeURIComponent(WASMIDI_MIDI_PARSER_BOOTSTRAP));

let modulePromise = null;
let lastAbortReason = "";
let lastRuntimeError = "";

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

    // Do not return the old opaque `Aborted()` dialog. Always identify which
    // stage failed and preserve Emscripten's assertion/fetch/trap diagnostic.
    return new Error(
        "Background MIDI parser failed while " +
        currentStage() +
        ": " + detail);
}

function getModule() {
    if (!modulePromise) {
        lastAbortReason = "";
        lastRuntimeError = "";
        self.__wasmidiMidiParserStage = "Initializing parser core";
        self.__wasmidiMidiParserPercent = 15;

        modulePromise = Promise.resolve().then(() =>
            WasmidiMidiParserCore({
                // The generated module now also has a real inert main() and is
                // linked NO_EXIT_RUNTIME=1, making either startup convention
                // safe. Keep these explicit for current Emscripten glue.
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
                typeof Module._wmp_result_ptr !== "function" ||
                typeof Module._wmp_result_size !== "function" ||
                typeof Module._wmp_error_ptr !== "function" ||
                typeof Module._malloc !== "function" ||
                typeof Module._free !== "function" ||
                !Module.HEAPU8) {
                throw new Error(
                    "parser WASM initialized without its complete exported API");
            }

            self.__wasmidiMidiParserStage = "Parser core ready";
            self.__wasmidiMidiParserPercent = 15;
            return Module;
        }).catch(error => {
            modulePromise = null;
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

async function readFileWithProgress(file) {
    const total = Math.max(0, Number(file.size) || 0);
    if (total > 0x7fffffff)
        throw new Error("MIDI is too large for this wasm32 build.");

    const bytes = new Uint8Array(total);
    let offset = 0;

    progress(1, "Reading MIDI file");

    if (file.stream && total > 0) {
        const reader = file.stream().getReader();
        while (true) {
            const result = await reader.read();
            if (result.done)
                break;

            const chunk = result.value;
            if (offset + chunk.byteLength > bytes.length)
                throw new Error("MIDI file changed while it was being read.");

            bytes.set(chunk, offset);
            offset += chunk.byteLength;

            progress(
                1 + Math.floor(13 * offset / Math.max(1, total)),
                "Reading MIDI file");

            if ((offset & ((4 * 1024 * 1024) - 1)) === 0)
                await new Promise(resolve => setTimeout(resolve, 0));
        }
    } else {
        const source = new Uint8Array(await file.arrayBuffer());
        if (source.length !== bytes.length)
            throw new Error("MIDI file size changed while loading.");
        bytes.set(source);
        offset = source.length;
    }

    if (offset !== bytes.length)
        throw new Error("MIDI file ended before the advertised size.");

    progress(15, "Starting MIDI parser");
    return bytes;
}

self.onmessage = async event => {
    const message = event.data || {};
    if (message.type !== "parse" || !message.file)
        return;

    let inputPtr = 0;
    let Module = null;

    try {
        const file = message.file;
        const input = await readFileWithProgress(file);
        Module = await getModule();

        self.__wasmidiMidiParserStage = "Allocating MIDI input";
        inputPtr = Module._malloc(Math.max(1, input.length));
        if (!inputPtr)
            throw new Error("Could not allocate parser WASM memory.");

        if (input.length)
            Module.HEAPU8.set(input, inputPtr);

        self.__wasmidiMidiParserStage = "Parsing MIDI";
        const ok = Module._wmp_parse(inputPtr, input.length);
        if (!ok) {
            const errorPtr = Module._wmp_error_ptr();
            const error = errorPtr
                ? Module.UTF8ToString(errorPtr)
                : "Could not parse MIDI";
            throw new Error(error);
        }

        progress(95, "Transferring parsed MIDI");

        const resultPtr = Module._wmp_result_ptr();
        const resultSize = Number(Module._wmp_result_size()) >>> 0;

        if (!resultPtr || !resultSize)
            throw new Error("MIDI parser returned an empty document.");

        const result = new Uint8Array(resultSize);
        result.set(
            Module.HEAPU8.subarray(
                resultPtr,
                resultPtr + resultSize));

        progress(96, "Installing parsed MIDI");

        postMessage({
            type: "result",
            name: String(message.name || file.name || "browser.mid"),
            data: result.buffer
        }, [result.buffer]);
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
            try {
                Module._free(inputPtr);
            } catch (_) {}
        }
    }
};
