/* global SnappySynthCore */

"use strict";

importScripts("./snappysynth-core.js");

let Module = null;
let coreReady = false;
let audioPort = null;

let sampleRateHz = 44100;
let blockFrames = 512;
let maxVoices = 16384;
let volume = 0.80;
let vorMode = 1; // 1 = one-voice loudness for exact overlap

let soundfontFile = null;
let soundfontPath = "";
let soundfontMounted = false;
let soundfontLoaded = false;

let playing = false;
let renderSongTime = 0.0;
let safeUntil = 0.0;
let pendingNeedFrames = 0;

let eventBatches = [];
let messagePtr = 0;
let offsetPtr = 0;
let scratchCapacity = 0;
let scratchMessages = new Uint32Array(0);
let scratchOffsets = new Uint32Array(0);

let pcmBlocksRendered = 0;
let lastStatsReport = 0;
const pcmBufferPool = [];
const pcmBufferPoolLimit = 32;

function postState(type, extra = {}) {
    postMessage(Object.assign({
        type,
        sampleRate: sampleRateHz,
        activeVoices:
            coreReady && Module
                ? Module._ssw_active_voices()
                : 0
    }, extra));
}

function setError(error) {
    const message =
        error && error.message
            ? error.message
            : String(error || "Unknown SnappySynthV2 error");

    console.error("[SnappySynthV2 worker]", error);
    postState("error", { message });
}

function resetEventQueue() {
    eventBatches.length = 0;
    safeUntil = renderSongTime;
}

function freeScratch() {
    if (!Module)
        return;

    if (messagePtr)
        Module._free(messagePtr);
    if (offsetPtr)
        Module._free(offsetPtr);

    messagePtr = 0;
    offsetPtr = 0;
    scratchCapacity = 0;
    scratchMessages = new Uint32Array(0);
    scratchOffsets = new Uint32Array(0);
}

function ensureScratch(required) {
    if (required <= scratchCapacity)
        return;

    let capacity = Math.max(1024, scratchCapacity || 1024);

    while (capacity < required)
        capacity *= 2;

    freeScratch();

    messagePtr = Module._malloc(capacity * 4);
    offsetPtr = Module._malloc(capacity * 4);

    if (!messagePtr || !offsetPtr)
        throw new Error("Could not allocate SnappySynth event scratch buffers.");

    scratchCapacity = capacity;
    scratchMessages = new Uint32Array(capacity);
    scratchOffsets = new Uint32Array(capacity);
}

function copyScratchToWasm(count) {
    if (count <= 0)
        return;

    Module.HEAPU32.set(
        scratchMessages.subarray(0, count),
        messagePtr >>> 2);

    Module.HEAPU32.set(
        scratchOffsets.subarray(0, count),
        offsetPtr >>> 2);
}

function mountSoundfontFile(file) {
    if (!Module || !file)
        return "";

    if (soundfontMounted) {
        try {
            Module.FS.unmount("/soundfont");
        } catch (_) {
        }
        soundfontMounted = false;
    }

    try {
        Module.FS.mkdir("/soundfont");
    } catch (_) {
        // Directory already exists.
    }

    Module.FS.mount(
        Module.WORKERFS,
        { files: [file] },
        "/soundfont");

    soundfontMounted = true;
    soundfontPath =
        "/soundfont/" +
        (file.name || "soundfont.sf2");

    return soundfontPath;
}

function callLoadSoundfont(path) {
    const bytes =
        Module.lengthBytesUTF8(path) + 1;

    const ptr =
        Module._malloc(bytes);

    if (!ptr)
        throw new Error("Could not allocate SF2 path string.");

    try {
        Module.stringToUTF8(
            path,
            ptr,
            bytes);

        return Module._ssw_load_sf2(ptr);
    } finally {
        Module._free(ptr);
    }
}

function applyCoreSettings() {
    Module._ssw_set_volume(volume);
    Module._ssw_set_vor_mode(vorMode);
}

function initCore() {
    if (!Module)
        return;

    Module._ssw_init(
        sampleRateHz,
        2,
        blockFrames,
        maxVoices);

    applyCoreSettings();
    coreReady = true;
}

function reinitializeCore() {
    if (!Module)
        return;

    Module._ssw_shutdown();
    freeScratch();

    coreReady = false;
    soundfontLoaded = false;

    initCore();

    if (soundfontFile && soundfontPath) {
        const regions =
            callLoadSoundfont(
                soundfontPath);

        soundfontLoaded =
            regions > 0;

        postState(
            soundfontLoaded
                ? "soundfont"
                : "error",
            soundfontLoaded
                ? {
                    loaded: true,
                    name:
                        soundfontFile.name ||
                        "soundfont.sf2",
                    regions
                }
                : {
                    message:
                        "SnappySynthV2 could not reload the selected SF2."
                });
    }

    renderSongTime = 0.0;
    resetEventQueue();
}

function collectEventsForBlock(blockStart, blockEnd, frames) {
    let count = 0;

    // First pass counts without creating per-event JS objects.
    outerCount:
    for (let b = 0; b < eventBatches.length; ++b) {
        const batch = eventBatches[b];

        for (let i = batch.index; i < batch.times.length; ++i) {
            const time = batch.times[i];

            if (time >= blockEnd)
                break outerCount;

            ++count;
        }
    }

    if (count === 0)
        return 0;

    ensureScratch(count);

    let out = 0;

    while (eventBatches.length > 0) {
        const batch = eventBatches[0];

        while (batch.index < batch.times.length) {
            const time =
                batch.times[batch.index];

            if (time >= blockEnd)
                break;

            const frame =
                Math.max(
                    0,
                    Math.min(
                        frames - 1,
                        Math.round(
                            (time - blockStart) *
                            sampleRateHz)));

            scratchMessages[out] =
                batch.messages[
                    batch.index];

            scratchOffsets[out] =
                frame >>> 0;

            ++out;
            ++batch.index;
        }

        if (batch.index >= batch.times.length) {
            eventBatches.shift();
            continue;
        }

        break;
    }

    copyScratchToWasm(out);
    return out;
}

function renderOneBlock(frames) {
    const blockStart =
        renderSongTime;

    const blockEnd =
        blockStart +
        frames / sampleRateHz;

    if (blockEnd > safeUntil + 1e-7)
        return false;

    const eventCount =
        collectEventsForBlock(
            blockStart,
            blockEnd,
            frames);

    const pcmPtr =
        Module._ssw_render(
            eventCount ? messagePtr : 0,
            eventCount ? offsetPtr : 0,
            eventCount,
            frames);

    if (!pcmPtr)
        throw new Error("SnappySynthV2 render returned a null PCM buffer.");

    const samples =
        frames * 2;

    // One unavoidable copy crosses from Shared WebAssembly memory to a
    // transferable ArrayBuffer. PCM then travels Worker -> AudioWorklet
    // directly through MessageChannel; the Qt/main thread never touches it.
    const bytes = samples * 4;
    let buffer = null;

    while (pcmBufferPool.length > 0 && !buffer) {
        const candidate = pcmBufferPool.pop();
        if (candidate && candidate.byteLength >= bytes)
            buffer = candidate;
    }

    if (!buffer)
        buffer = new ArrayBuffer(bytes);

    const pcm =
        new Float32Array(buffer, 0, samples);

    pcm.set(
        Module.HEAPF32.subarray(
            pcmPtr >>> 2,
            (pcmPtr >>> 2) +
            samples));

    audioPort.postMessage({
        type: "pcm",
        pcm,
        frames
    }, [pcm.buffer]);

    renderSongTime = blockEnd;
    pendingNeedFrames =
        Math.max(
            0,
            pendingNeedFrames - frames);

    ++pcmBlocksRendered;

    const now = performance.now();

    if (now - lastStatsReport > 250) {
        lastStatsReport = now;

        postState("stats", {
            activeVoices:
                Module._ssw_active_voices(),
            renderSongTime
        });
    }

    return true;
}

function pump() {
    if (!Module ||
        !coreReady ||
        !soundfontLoaded ||
        !playing ||
        !audioPort) {
        return;
    }

    try {
        let guard = 0;

        while (pendingNeedFrames > 0 &&
               guard++ < 128) {
            const frames =
                Math.min(
                    blockFrames,
                    Math.max(
                        128,
                        pendingNeedFrames));

            if (!renderOneBlock(frames))
                break;
        }
    } catch (error) {
        setError(error);
        playing = false;
    }
}

function onAudioPortMessage(data) {
    if (!data)
        return;

    if (data.type === "need") {
        pendingNeedFrames +=
            Math.max(
                0,
                Number(data.frames) | 0);

        pump();
        return;
    }

    if (data.type === "recycle" &&
        data.buffer instanceof ArrayBuffer) {
        if (pcmBufferPool.length < pcmBufferPoolLimit)
            pcmBufferPool.push(data.buffer);
        return;
    }

    if (data.type === "clock") {
        postState("clock", {
            songTime:
                Number(data.songTime) || 0.0,
            underruns:
                Number(data.underruns) | 0,
            starved:
                !!data.starved
        });
    }
}

onmessage = async event => {
    const data = event.data || {};

    try {
        if (data.type === "audioPort" && data.port) {
            audioPort = data.port;
            audioPort.onmessage =
                e =>
                    onAudioPortMessage(
                        e.data || {});
            audioPort.start();
            pump();
            return;
        }

        if (data.type === "loadSoundfont") {
            if (!Module)
                throw new Error("SnappySynthV2 core is still starting.");

            const file = data.file;

            if (!file)
                throw new Error("No SF2 file was provided.");

            soundfontFile = file;

            const path =
                mountSoundfontFile(file);

            Module._ssw_reset();

            const regions =
                callLoadSoundfont(path);

            soundfontLoaded =
                regions > 0;

            renderSongTime = 0.0;
            resetEventQueue();

            if (!soundfontLoaded)
                throw new Error("SnappySynthV2 could not parse this SF2.");

            postState("soundfont", {
                loaded: true,
                name:
                    file.name ||
                    "soundfont.sf2",
                regions
            });

            pump();
            return;
        }

        if (data.type === "schedule") {
            const messages =
                data.messages;

            const times =
                data.times;

            if (messages &&
                times &&
                messages.length ===
                    times.length &&
                messages.length > 0) {
                eventBatches.push({
                    messages,
                    times,
                    index: 0
                });
            }

            safeUntil =
                Math.max(
                    safeUntil,
                    Number(data.safeUntil) ||
                    renderSongTime);

            pump();
            return;
        }

        if (data.type === "play") {
            const time =
                Math.max(
                    0.0,
                    Number(data.time) || 0.0);

            if (data.reset) {
                Module._ssw_reset();
                renderSongTime = time;
                // Do not clear pendingNeedFrames here. The AudioWorklet sends
                // its fresh request on a different MessagePort and that request
                // may arrive just before this reset. Clearing it would leave the
                // worklet believing audio is on the way and cause a deadlock.
                resetEventQueue();
            }

            playing = true;
            pump();
            return;
        }

        if (data.type === "pause") {
            playing = false;
            return;
        }

        if (data.type === "seek") {
            const time =
                Math.max(
                    0.0,
                    Number(data.time) || 0.0);

            Module._ssw_reset();
            renderSongTime = time;
            // Preserve cross-port demand for the same reason as play(reset).
            resetEventQueue();
            return;
        }

        if (data.type === "stop") {
            playing = false;
            Module._ssw_reset();
            renderSongTime = 0.0;
            pendingNeedFrames = 0;
            resetEventQueue();
            return;
        }

        if (data.type === "volume") {
            volume =
                Math.max(
                    0.0,
                    Math.min(
                        1.0,
                        Number(data.value) ||
                        0.0));

            if (coreReady)
                Module._ssw_set_volume(volume);

            return;
        }

        if (data.type === "vor") {
            vorMode =
                data.overlapGain
                    ? 0
                    : 1;

            if (coreReady)
                Module._ssw_set_vor_mode(vorMode);

            return;
        }

        if (data.type === "configure") {
            sampleRateHz =
                Math.max(
                    8000,
                    Math.min(
                        384000,
                        Number(data.sampleRate) ||
                        sampleRateHz));

            maxVoices =
                Math.max(
                    128,
                    Number(data.maxVoices) |
                    0);

            blockFrames =
                Math.max(
                    128,
                    Number(data.blockFrames) |
                    0);

            playing = false;
            reinitializeCore();

            postState("configured", {
                maxVoices,
                blockFrames
            });
        }
    } catch (error) {
        setError(error);
    }
};

const coreScriptUrl =
    new URL(
        "./snappysynth-core.js",
        self.location.href).href;

console.info(
    "[SnappySynthV2 worker] core script:",
    coreScriptUrl);

SnappySynthCore({
    /*
     * This module is MODULARIZE + pthreads and is itself instantiated from
     * snappysynth-worker.js. In that nested-worker scenario Emscripten cannot
     * reliably infer the URL of the main generated JS file.
     *
     * Without this value the generated snappysynth-core.worker.js receives
     * urlOrBlob=undefined and executes URL.createObjectURL(undefined), which
     * is exactly the Chrome "Overload resolution failed" seen in the console.
     */
    mainScriptUrlOrBlob:
        coreScriptUrl,

    locateFile(path) {
        return new URL(
            path,
            self.location.href).href;
    },
    print(text) {
        if (text)
            console.log("[SnappySynthV2]", text);
    },
    printErr(text) {
        if (text)
            console.warn("[SnappySynthV2]", text);
    }
}).then(module => {
    Module = module;
    initCore();

    postState("ready", {
        ready: true,
        maxVoices,
        blockFrames
    });

    pump();
}).catch(setError);
