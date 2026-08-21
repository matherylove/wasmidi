/* global WasmidiMidiParserCore */

"use strict";

importScripts("./wasmidi-midi-parser.js");

let modulePromise = null;

function getModule() {
    if (!modulePromise) {
        modulePromise = WasmidiMidiParserCore({
            locateFile(path) {
                return "./" + path;
            }
        });
    }
    return modulePromise;
}

function progress(percent, stage) {
    postMessage({
        type: "progress",
        percent: Math.max(0, Math.min(100, percent | 0)),
        stage: String(stage || "Loading MIDI")
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

            // Keep the Worker message queue responsive even when File.stream()
            // supplies many chunks back-to-back from memory cache.
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

    try {
        const file = message.file;
        const input = await readFileWithProgress(file);
        const Module = await getModule();

        inputPtr = Module._malloc(Math.max(1, input.length));
        if (!inputPtr)
            throw new Error("Could not allocate parser WASM memory.");

        if (input.length)
            Module.HEAPU8.set(input, inputPtr);

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
        postMessage({
            type: "error",
            message: error && error.message
                ? error.message
                : String(error || "Could not parse MIDI")
        });
    } finally {
        if (inputPtr) {
            try {
                const Module = await getModule();
                Module._free(inputPtr);
            } catch (_) {}
        }
    }
};
