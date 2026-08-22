/* global WasmidiMidiParserCore */

"use strict";

// Pass 13.0: SharpMIDI-style mapped-source parser. The browser File remains Chromium 133+ can grow one wasm64
// outside the WASM heap; only compact indexes/checkpoints remain resident in
// Memory64, and render/playback data are decoded into bounded pages on demand.
const WASMIDI_MIDI_PARSER_BOOTSTRAP = "13.0";
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
                typeof Module._wmp_parse_file_js !== "function" ||
                typeof Module._wmp_pack !== "function" ||
                typeof Module._wmp_result_ptr_js !== "function" ||
                typeof Module._wmp_result_size_js !== "function" ||
                typeof Module._wmp_release_result !== "function" ||
                typeof Module._wmp_error_ptr_js !== "function" ||
                typeof Module._wmp_error_size_js !== "function" ||
                typeof Module._wmp_build_visual_page_js !== "function" ||
                typeof Module._wmp_visual_page_ptr_js !== "function" ||
                typeof Module._wmp_visual_page_count_js !== "function" ||
                typeof Module._wmp_build_key_snapshot_js !== "function" ||
                typeof Module._wmp_key_snapshot_ptr_js !== "function" ||
                typeof Module._wmp_key_snapshot_word_count_js !== "function" ||
                typeof Module._wmp_reset_event_cursor_js !== "function" ||
                typeof Module._wmp_build_event_batch_js !== "function" ||
                typeof Module._wmp_event_batch_ptr_js !== "function" ||
                typeof Module._wmp_event_batch_count_js !== "function" ||
                typeof Module._wmp_event_batch_complete_js !== "function" ||
                typeof Module._wmp_tick_to_seconds_js !== "function" ||
                typeof Module._wmp_pointer_bits !== "function" ||
                !Module.HEAPU8) {
                throw new Error(
                    "parser WASM initialized without its complete exported API");
            }

            pointerBits = Number(Module._wmp_pointer_bits()) | 0;
            if (pointerBits !== 64) {
                throw new Error(
                    "Pass 13.0 parser was built without Memory64 (pointer width " +
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
    pagedSource: true,
    mappedStore: true
});


let mappedFileReady = false;
let visualPrimeToken = 0;

function copyWasmBytes(Module, address, byteLength) {
    const base = pointerToNumber(address, "mapped result pointer");
    const size = sizeToNumber(byteLength, "mapped result size");
    if (!base || size < 0 || base + size > Module.HEAPU8.length)
        throw new Error("Mapped result points outside the Memory64 heap.");
    return Module.HEAPU8.slice(base, base + size);
}

async function buildVisualPages(message) {
    const Module = await getModule();
    if (!mappedFileReady)
        return;

    const generation = Number(message.generation) >>> 0;
    const span = Math.max(1, Number(message.spanTicks) >>> 0);
    const first = Number(message.firstPage) >>> 0;
    const count = Math.max(0, Math.min(64, Number(message.count) >>> 0));
    const current = Number(message.currentPage) >>> 0;
    const token = ++visualPrimeToken;

    const pages = [];
    for (let i = 0; i < count; ++i)
        pages.push(first + i);
    pages.sort((a, b) => {
        if (a === current) return -1;
        if (b === current) return 1;
        return Math.abs(a - current) - Math.abs(b - current);
    });

    for (const pageIndex of pages) {
        if (token !== visualPrimeToken)
            return;
        const start = pageIndex * span;
        const end = Math.min(0xffffffff, start + span);
        if (!Module._wmp_build_visual_page_js(start, end)) {
            throw new Error(parserErrorText(Module, "Could not build mapped visual page"));
        }
        const noteCount = sizeToNumber(Module._wmp_visual_page_count_js(), "visual page count");
        const bytes = noteCount
            ? copyWasmBytes(Module, Module._wmp_visual_page_ptr_js(), noteCount * 12)
            : new Uint8Array(0);
        postMessage({
            type: "visual-page",
            generation,
            spanTicks: span,
            pageIndex,
            sourceCount: noteCount,
            difficulty: noteCount,
            data: bytes.buffer
        }, [bytes.buffer]);
        await new Promise(resolve => setTimeout(resolve, 0));
    }
}

async function buildKeyState(message) {
    const Module = await getModule();
    if (!mappedFileReady)
        return;
    const tick = Math.max(0, Math.min(0xffffffff, Number(message.tick) || 0));
    if (!Module._wmp_build_key_snapshot_js(tick))
        throw new Error(parserErrorText(Module, "Could not build mapped key state"));
    const words = sizeToNumber(Module._wmp_key_snapshot_word_count_js(), "key snapshot words");
    const bytes = words
        ? copyWasmBytes(Module, Module._wmp_key_snapshot_ptr_js(), words * 4)
        : new Uint8Array(0);
    postMessage({
        type: "key-state",
        tick,
        data: bytes.buffer
    }, [bytes.buffer]);
}

async function resetSynthCursor(message) {
    const Module = await getModule();
    const tick = Math.max(0, Math.min(0xffffffff, Number(message.tick) || 0));
    Module._wmp_reset_event_cursor_js(tick);
    postMessage({ type: "synth-cursor-reset", tick });
}

async function pumpSynthWindow(message) {
    const Module = await getModule();
    if (!mappedFileReady)
        return;
    const endTick = Math.max(0, Math.min(0xffffffff, Number(message.endTick) || 0));
    const velocityFloor = Math.max(0, Math.min(127, Number(message.velocityFloor) | 0));
    const safeUntil = Math.max(0, Number(message.safeUntil) || 0);
    const maxBatches = Math.max(1, Math.min(64, Number(message.maxBatches) | 0 || 8));

    for (let batchIndex = 0; batchIndex < maxBatches; ++batchIndex) {
        if (!Module._wmp_build_event_batch_js(endTick, 65536))
            throw new Error(parserErrorText(Module, "Could not build mapped synth batch"));
        const count = sizeToNumber(Module._wmp_event_batch_count_js(), "event batch count");
        const complete = !!Module._wmp_event_batch_complete_js();
        const ptr = pointerToNumber(Module._wmp_event_batch_ptr_js(), "event batch pointer");
        const messages = [];
        const times = [];

        let lastTick = -1;
        let lastTime = 0;
        let i = 0;
        while (i < count) {
            const byte = ptr + i * 8;
            const tick = Module.HEAPU32[byte >>> 2] >>> 0;
            const packed = Module.HEAPU32[(byte >>> 2) + 1] >>> 0;
            const status = packed & 255;
            const command = status & 0xf0;
            const data1 = (packed >>> 8) & 127;
            const data2 = (packed >>> 16) & 127;

            if (tick !== lastTick) {
                lastTick = tick;
                lastTime = Number(Module._wmp_tick_to_seconds_js(tick)) || 0;
            }

            if (command === 0x90 && data2 !== 0 && data2 < velocityFloor) {
                ++i;
                continue;
            }

            let messageWord = status | (data1 << 8) | (data2 << 16);
            if (command === 0x90 && data2 !== 0) {
                // Same compact overlap optimization as the Qt scheduler: pack
                // up to 256 consecutive identical NoteOns into SnappySynth's
                // high-byte overlap count without reordering controller data.
                let run = 1;
                while (i + run < count && run < 256) {
                    const nextByte = ptr + (i + run) * 8;
                    const nextTick = Module.HEAPU32[nextByte >>> 2] >>> 0;
                    const nextPacked = Module.HEAPU32[(nextByte >>> 2) + 1] >>> 0;
                    if (nextTick !== tick || (nextPacked & 0x00ffffff) !== (packed & 0x00ffffff))
                        break;
                    ++run;
                }
                if (run > 1) {
                    messageWord |= (run - 1) << 24;
                    i += run - 1;
                }
            }

            messages.push(messageWord >>> 0);
            times.push(lastTime);
            ++i;
        }

        const msgArray = Uint32Array.from(messages);
        const timeArray = Float64Array.from(times);
        postMessage({
            type: "synth-batch",
            messages: msgArray.buffer,
            times: timeArray.buffer,
            safeUntil: complete ? safeUntil : 0,
            complete
        }, [msgArray.buffer, timeArray.buffer]);

        if (complete)
            return;
        await new Promise(resolve => setTimeout(resolve, 0));
    }

    // More data remains. Main thread will request another pump without moving
    // safeUntil, so SnappySynth can never render beyond a partial dense tick.
    postMessage({ type: "synth-more", endTick, safeUntil, velocityFloor });
}

self.onmessage = async event => {
    const message = event.data || {};
    try {
        if (message.type === "visual-prime") {
            await buildVisualPages(message);
            return;
        }
        if (message.type === "key-state") {
            await buildKeyState(message);
            return;
        }
        if (message.type === "synth-reset") {
            await resetSynthCursor(message);
            return;
        }
        if (message.type === "synth-pump") {
            await pumpSynthWindow(message);
            return;
        }
    } catch (error) {
        postMessage({
            type: "runtime-error",
            message: error && error.message ? error.message : String(error)
        });
        return;
    }

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

        // Pass 13 keeps the File as the browser equivalent of SharpMIDI's
        // MemoryMappedFile. Only metadata is packed to Qt; render/playback
        // pages continue to read bounded windows from this File after loading.
        mappedFileReady = true;

        self.__wasmidiMidiParserStage = "Packing mapped MIDI metadata";
        const packed = Module._wmp_pack();
        if (!packed) {
            throw new Error(
                parserErrorText(Module, "Could not pack parsed MIDI"));
        }
        resultOwned = true;

        progress(95, "Streaming mapped MIDI metadata to player");

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

        progress(96, "Installing mapped MIDI metadata");
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
        if (!mappedFileReady) {
            self.__wasmidiMidiParserFile = null;
            self.__wasmidiMidiParserFileReader = null;
        }
        self.__wasmidiMidiParserReadAt = null;
        if (resultOwned && Module) {
            try { Module._wmp_release_result(); } catch (_) {}
        }
    }
};
