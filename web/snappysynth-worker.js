/* global SnappySynthCore */

"use strict";

importScripts("./snappysynth-core.js");

let Module = null;
let coreReady = false;
let audioPort = null;

let sampleRateHz = 44100;
let blockFrames = 512;
let numBuffers = 16;
let maxVoices = 16384;
let minVoices = 0;
let synthChannels = 2;
let bitsPerSample = 32;
let realtimePriority = 1;

let requestedWorkers = 0;     // 0 = original automatic policy
let noteSharding = 0;         // 0 auto, 1 channel, 2 hash
let stealScoreCache = true;   // source default ON
let fastNoteOff = true;       // source default ON
let validateState = false;    // source default OFF
let softClip = true;          // source realtime default ON

let volume = 1.00;
let vorMode = 1; // 1 = one-voice loudness for exact overlap

let soundfontFiles = [];
let soundfontPaths = [];
let soundfontMounts = [];
let soundfontLoaded = false;
let sysexEvents = [];
let sysexPtr = 0;
let sysexCapacity = 0;

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
            coreReady && Module ? Module._ssw_active_voices() : 0,
        freeVoices:
            coreReady && Module ? Module._ssw_free_voices() : 0,
        steals:
            coreReady && Module ? Module._ssw_steals() : 0,
        layers:
            coreReady && Module ? Module._ssw_layer_count() : 0,
        regions:
            coreReady && Module ? Module._ssw_region_count() : 0,
        workerCount:
            coreReady && Module ? Module._ssw_worker_count() : 0,
        channels:
            coreReady && Module ? Module._ssw_channels() : synthChannels,
        bitsPerSample:
            coreReady && Module ? Module._ssw_bits_per_sample() : bitsPerSample,
        numBuffers:
            coreReady && Module ? Module._ssw_num_buffers() : numBuffers
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
    sysexEvents.length = 0;
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

function ensureDir(path) {
    try { Module.FS.mkdir(path); } catch (_) {}
}

function mountSoundfontFile(file) {
    if (!Module || !file)
        return "";
    if (!Module.FS)
        throw new Error("Emscripten FS is not available in SnappySynthV2 core.");
    if (!Module.WORKERFS)
        throw new Error("WORKERFS is not available in SnappySynthV2 core.");

    ensureDir("/soundfonts");
    const index = soundfontFiles.length;
    const mount = "/soundfonts/layer" + index;
    ensureDir(mount);

    Module.FS.mount(
        Module.WORKERFS,
        { files: [file] },
        mount);

    const path = mount + "/" + (file.name || ("layer" + index + ".sf2"));
    const stat = Module.FS.stat(path);
    if (!stat || stat.size <= 0) {
        try { Module.FS.unmount(mount); } catch (_) {}
        throw new Error("Selected SF2 is empty or WORKERFS could not expose it.");
    }

    soundfontFiles.push(file);
    soundfontPaths.push(path);
    soundfontMounts.push(mount);
    return path;
}

function unmountAllSoundfonts() {
    for (let i = soundfontMounts.length - 1; i >= 0; --i) {
        try { Module.FS.unmount(soundfontMounts[i]); } catch (_) {}
    }
    soundfontFiles.length = 0;
    soundfontPaths.length = 0;
    soundfontMounts.length = 0;
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
    // MIDI master volume belongs to the synth and may be changed by SysEx.
    // UI volume is applied after synthesis in the AudioWorklet.
    Module._ssw_set_vor_mode(vorMode);
}

function initCore() {
    if (!Module)
        return;

    Module._ssw_init_ex(
        sampleRateHz,
        synthChannels,
        bitsPerSample,
        blockFrames,
        numBuffers,
        realtimePriority,
        maxVoices,
        minVoices,
        requestedWorkers,
        noteSharding,
        stealScoreCache ? 1 : 0,
        fastNoteOff ? 1 : 0,
        validateState ? 1 : 0,
        softClip ? 1 : 0);

    applyCoreSettings();
    coreReady = true;
}

function reinitializeCore() {
    if (!Module)
        return;

    freeScratch();
    coreReady = false;

    // ssw_init() reinitializes only the voice engine and deliberately preserves
    // the already-loaded merged instrument, matching SnappySynth config reload.
    initCore();

    soundfontLoaded =
        Module._ssw_region_count() > 0;

    postState("configured", {
        maxVoices,
        minVoices,
        blockFrames,
        numBuffers,
        channels: synthChannels,
        bitsPerSample,
        realtimePriority,
        requestedWorkers,
        workerCount: Module._ssw_worker_count(),
        noteSharding,
        stealScoreCache,
        fastNoteOff,
        validateState,
        softClip,
        loaded: soundfontLoaded,
        layers: Module._ssw_layer_count(),
        regions: Module._ssw_region_count()
    });

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

function ensureSysexScratch(required) {
    if (required <= sysexCapacity)
        return;
    let capacity = Math.max(256, sysexCapacity || 256);
    while (capacity < required) capacity *= 2;
    if (sysexPtr) Module._free(sysexPtr);
    sysexPtr = Module._malloc(capacity);
    if (!sysexPtr) throw new Error("Could not allocate SysEx scratch buffer.");
    sysexCapacity = capacity;
}

function dispatchSysexForBlock(blockStart, blockEnd, frames) {
    while (sysexEvents.length > 0 && sysexEvents[0].time < blockEnd) {
        const event = sysexEvents.shift();
        const bytes = event.bytes;
        if (!(bytes instanceof Uint8Array) || bytes.length === 0)
            continue;
        ensureSysexScratch(bytes.length);
        Module.HEAPU8.set(bytes, sysexPtr);
        const frame = Math.max(0, Math.min(frames - 1,
            Math.round((event.time - blockStart) * sampleRateHz)));
        Module._ssw_send_sysex(sysexPtr, bytes.length, frame >>> 0);
    }
}

function renderOneBlock(frames) {
    const blockStart =
        renderSongTime;

    const blockEnd =
        blockStart +
        frames / sampleRateHz;

    if (blockEnd > safeUntil + 1e-7)
        return false;

    dispatchSysexForBlock(
        blockStart,
        blockEnd,
        frames);

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

    // Browser output remains stereo, but the synth engine itself honors the
    // original NumChannels configuration. Mono is duplicated only at the
    // Worker -> AudioWorklet boundary.
    const sourceChannels =
        Math.max(1, Module._ssw_channels() | 0);

    const outputSamples =
        frames * 2;

    const bytes =
        outputSamples * 4;

    let buffer = null;

    while (pcmBufferPool.length > 0 && !buffer) {
        const candidate = pcmBufferPool.pop();
        if (candidate && candidate.byteLength >= bytes)
            buffer = candidate;
    }

    if (!buffer)
        buffer = new ArrayBuffer(bytes);

    const pcm =
        new Float32Array(
            buffer,
            0,
            outputSamples);

    const source =
        Module.HEAPF32;

    const sourceIndex =
        pcmPtr >>> 2;

    if (sourceChannels === 1) {
        for (let i = 0; i < frames; ++i) {
            const sample =
                source[sourceIndex + i];

            pcm[i * 2] = sample;
            pcm[i * 2 + 1] = sample;
        }
    } else {
        pcm.set(
            source.subarray(
                sourceIndex,
                sourceIndex +
                outputSamples));
    }

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
            if (!Module || !coreReady)
                throw new Error("SnappySynthV2 core is still starting.");

            const file = data.file;
            if (!file)
                throw new Error("No SF2 file was provided.");

            const path = mountSoundfontFile(file);
            const regions = callLoadSoundfont(path);
            if (regions <= 0) {
                // Roll back the failed layer mount/list entry.
                const mount = soundfontMounts.pop();
                soundfontPaths.pop();
                soundfontFiles.pop();
                try { Module.FS.unmount(mount); } catch (_) {}
                throw new Error(
                    "SnappySynthV2 SF2 parser returned 0 regions for " +
                    (file.name || "soundfont.sf2") +
                    " (" + Number(file.size || 0) + " bytes).");
            }

            soundfontLoaded = true;
            renderSongTime = 0.0;
            resetEventQueue();

            const layers = Module._ssw_layer_count();
            postState("soundfont", {
                loaded: true,
                name: layers > 1
                    ? (file.name || "soundfont.sf2") + " (+" + (layers - 1) + " layer" + (layers > 2 ? "s" : "") + ")"
                    : (file.name || "soundfont.sf2"),
                layers,
                regions: Module._ssw_region_count()
            });

            pump();
            return;
        }

        if (data.type === "clearSoundfonts") {
            playing = false;
            resetEventQueue();
            Module._ssw_clear_soundfonts();
            unmountAllSoundfonts();
            soundfontLoaded = false;
            renderSongTime = 0.0;
            postState("soundfont", {
                loaded: false,
                name: "",
                layers: 0,
                regions: 0
            });
            return;
        }

        if (data.type === "sysex") {
            const bytes = data.bytes;
            if (bytes instanceof Uint8Array && bytes.length > 0) {
                const event = {
                    bytes,
                    time: Math.max(0.0, Number(data.time) || 0.0)
                };
                // C++ sends in chronological order; retain a defensive ordered insertion.
                let pos = sysexEvents.length;
                while (pos > 0 && sysexEvents[pos - 1].time > event.time) --pos;
                sysexEvents.splice(pos, 0, event);
            }
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
            volume = Math.max(0.0, Math.min(1.0, Number(data.value) || 0.0));
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
                    1,
                    Math.min(
                        5000000,
                        Number(data.maxVoices) |
                        0));

            minVoices =
                Math.max(
                    0,
                    Math.min(
                        5000000,
                        Number(data.minVoices) |
                        0));

            blockFrames =
                Math.max(
                    1,
                    Number(data.blockFrames) |
                    0);

            numBuffers =
                Math.max(
                    1,
                    Math.min(
                        128,
                        Number(data.numBuffers) |
                        0));

            synthChannels =
                Number(data.channels) === 1
                    ? 1
                    : 2;

            bitsPerSample =
                Number(data.bitsPerSample) === 16
                    ? 16
                    : 32;

            realtimePriority =
                data.realtimePriority
                    ? 1
                    : 0;

            requestedWorkers =
                Math.max(
                    0,
                    Number(data.workers) |
                    0);

            noteSharding =
                Math.max(
                    0,
                    Math.min(
                        2,
                        Number(data.noteSharding) |
                        0));

            stealScoreCache =
                data.stealScoreCache !== false;

            fastNoteOff =
                data.fastNoteOff !== false;

            validateState =
                !!data.validateState;

            softClip =
                data.softClip !== false;

            playing = false;
            reinitializeCore();

            if (audioPort) {
                audioPort.postMessage({
                    type: "engineConfig",
                    blockFrames,
                    numBuffers
                });
            }

            postState("configured", {
                maxVoices,
                minVoices,
                blockFrames,
                numBuffers,
                channels: synthChannels,
                bitsPerSample,
                realtimePriority,
                requestedWorkers,
                workerCount: Module._ssw_worker_count(),
                noteSharding,
                stealScoreCache,
                fastNoteOff,
                validateState,
                softClip
            });
            return;
        }

        if (data.type === "softClip") {
            softClip =
                data.enabled !== false;

            if (coreReady)
                Module._ssw_set_soft_clip(
                    softClip ? 1 : 0);

            postState("configured", {
                softClip
            });
            return;
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
        minVoices,
        blockFrames,
        numBuffers,
        channels: synthChannels,
        bitsPerSample,
        realtimePriority,
        requestedWorkers,
        workerCount: Module._ssw_worker_count(),
        noteSharding,
        stealScoreCache,
        fastNoteOff,
        validateState,
        softClip
    });

    pump();
}).catch(setError);
