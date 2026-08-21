/* global SnappySynthCore */

"use strict";

importScripts("./snappysynth-core.js");

let Module = null;
let coreReady = false;
let audioPort = null;

let sampleRateHz = 44100;
let blockFrames = 512;
let numBuffers = 16;
let prebufferSeconds = 8.0;
let midiDuration = 0.0;
let prebufferFrames = 0;
const MAX_PREBUFFER_BYTES = 384 * 1024 * 1024;
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
let eventBatchHead = 0;
let messagePtr = 0;
let offsetPtr = 0;
let scratchCapacity = 0;

let pcmBlocksRendered = 0;
let lastStatsReport = 0;

// Shared PCM ring lives directly inside SnappySynth's pthread WebAssembly.Memory.
// AudioWorklet reads it in-place: no per-block ArrayBuffer allocation, copy,
// transfer, recycle message, or queue object is required.
const RING_READ = 0;
const RING_WRITE = 1;
const RING_AVAILABLE = 2;
const RING_GENERATION = 3;
const RING_CAPACITY = 4;
const RING_CHANNELS = 5;
const RING_BLOCK_FRAMES = 6;
const RING_WORDS = 8;

let audioRingHeaderPtr = 0;
let audioRingPcmPtr = 0;
let audioRingCapacityFrames = 0;
let audioRingMemory = null;

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
            coreReady && Module ? Module._ssw_num_buffers() : numBuffers,
        prebufferFrames
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

function ringHeader() {
    if (!Module || !audioRingHeaderPtr)
        return null;
    return new Int32Array(
        Module.HEAPU8.buffer,
        audioRingHeaderPtr,
        RING_WORDS);
}

function resetAudioRing() {
    const header = ringHeader();
    if (!header)
        return;

    Atomics.add(header, RING_GENERATION, 1);
    Atomics.store(header, RING_READ, 0);
    Atomics.store(header, RING_WRITE, 0);
    Atomics.store(header, RING_AVAILABLE, 0);
}

function freeAudioRing() {
    if (!Module)
        return;

    if (audioRingHeaderPtr)
        Module._free(audioRingHeaderPtr);
    if (audioRingPcmPtr)
        Module._free(audioRingPcmPtr);

    audioRingHeaderPtr = 0;
    audioRingPcmPtr = 0;
    audioRingCapacityFrames = 0;
    audioRingMemory = null;
}

function publishAudioRing(force = false) {
    if (!Module || !audioPort || !audioRingHeaderPtr || !audioRingPcmPtr)
        return;

    const memory = Module.HEAPU8.buffer;

    if (!(memory instanceof SharedArrayBuffer)) {
        throw new Error(
            "SnappySynthV2 pthread memory is not SharedArrayBuffer; " +
            "cross-origin isolation/pthread initialization is incomplete.");
    }

    if (!force && memory === audioRingMemory)
        return;

    audioRingMemory = memory;

    audioPort.postMessage({
        type: "sharedRing",
        memory,
        headerPtr: audioRingHeaderPtr,
        pcmPtr: audioRingPcmPtr,
        capacityFrames: audioRingCapacityFrames,
        channels: synthChannels,
        blockFrames
    });
}

function preferredRenderFrames() {
    // Browser pthread/futex synchronization is substantially more expensive
    // than the native Win32 event path. Render several source blocks per voice
    // pass while preserving sample-accurate event offsets inside the larger
    // call. At the default 512-frame source block this reduces worker barriers
    // by 8x without changing the AudioWorklet's 128-frame delivery cadence.
    const target = 4096;
    if (blockFrames >= target)
        return blockFrames;
    return Math.ceil(target / Math.max(1, blockFrames)) * Math.max(1, blockFrames);
}

function requestedPrebufferFrames() {
    const transportFrames =
        Math.max(1, blockFrames) *
        Math.max(1, numBuffers);

    let seconds = prebufferSeconds;
    if (midiDuration > 0.0) {
        seconds =
            seconds <= 0.0
                ? midiDuration
                : Math.min(seconds, midiDuration);
    }

    let requested =
        seconds > 0.0
            ? Math.ceil(seconds * sampleRateHz)
            : transportFrames;

    requested = Math.max(transportFrames, requested);

    const bytesPerFrame = Math.max(1, synthChannels) * 4;
    const memoryCapFrames =
        Math.max(transportFrames, Math.floor(MAX_PREBUFFER_BYTES / bytesPerFrame));

    requested = Math.min(requested, memoryCapFrames);

    const quantum = Math.max(1, preferredRenderFrames());
    const aligned = Math.ceil(requested / quantum) * quantum;
    return Math.max(quantum, Math.min(aligned, memoryCapFrames));
}

function allocateAudioRing() {
    if (!Module)
        return;

    freeAudioRing();

    // The shared ring is also the rolling pre-render cache. It is sized from
    // the user's requested ahead duration (0 = MIDI duration), with the source
    // NumBuffers transport size as a minimum and a browser-memory safety cap.
    const minimumTransportFrames = 256;
    audioRingCapacityFrames =
        Math.max(
            minimumTransportFrames,
            requestedPrebufferFrames());

    // Keep capacity an exact source-block multiple so wrap points can never
    // split one sample-accurate synth block.
    audioRingCapacityFrames =
        Math.ceil(
            audioRingCapacityFrames /
            Math.max(1, blockFrames)) *
        Math.max(1, blockFrames);

    prebufferFrames = audioRingCapacityFrames;

    audioRingHeaderPtr =
        Module._malloc(RING_WORDS * 4);
    audioRingPcmPtr =
        Module._malloc(
            audioRingCapacityFrames *
            Math.max(1, synthChannels) *
            4);

    if (!audioRingHeaderPtr || !audioRingPcmPtr) {
        freeAudioRing();
        throw new Error(
            "Could not allocate the shared SnappySynth audio ring.");
    }

    const header = ringHeader();
    for (let i = 0; i < RING_WORDS; ++i)
        Atomics.store(header, i, 0);
    Atomics.store(header, RING_CAPACITY, audioRingCapacityFrames);
    Atomics.store(header, RING_CHANNELS, synthChannels);
    Atomics.store(header, RING_BLOCK_FRAMES, blockFrames);

    publishAudioRing(true);
}

function resetEventQueue() {
    eventBatches.length = 0;
    eventBatchHead = 0;
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
}

function ensureScratch(required, preserveCount = 0) {
    if (required <= scratchCapacity)
        return;

    let capacity =
        Math.max(
            4096,
            scratchCapacity || 4096);

    while (capacity < required)
        capacity *= 2;

    const oldMessagePtr = messagePtr;
    const oldOffsetPtr = offsetPtr;
    const oldCapacity = scratchCapacity;

    const newMessagePtr =
        Module._malloc(capacity * 4);
    const newOffsetPtr =
        Module._malloc(capacity * 4);

    if (!newMessagePtr || !newOffsetPtr) {
        if (newMessagePtr)
            Module._free(newMessagePtr);
        if (newOffsetPtr)
            Module._free(newOffsetPtr);
        throw new Error(
            "Could not allocate SnappySynth event scratch buffers.");
    }

    if (preserveCount > 0 &&
        oldMessagePtr &&
        oldOffsetPtr) {
        const count =
            Math.min(
                preserveCount,
                oldCapacity);

        Module.HEAPU32.set(
            Module.HEAPU32.subarray(
                oldMessagePtr >>> 2,
                (oldMessagePtr >>> 2) + count),
            newMessagePtr >>> 2);

        Module.HEAPU32.set(
            Module.HEAPU32.subarray(
                oldOffsetPtr >>> 2,
                (oldOffsetPtr >>> 2) + count),
            newOffsetPtr >>> 2);
    }

    if (oldMessagePtr)
        Module._free(oldMessagePtr);
    if (oldOffsetPtr)
        Module._free(oldOffsetPtr);

    messagePtr = newMessagePtr;
    offsetPtr = newOffsetPtr;
    scratchCapacity = capacity;
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
    allocateAudioRing();
}

function reinitializeCore() {
    if (!Module)
        return;

    freeScratch();
    freeAudioRing();
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
        regions: Module._ssw_region_count(),
        prebufferFrames
    });

    renderSongTime = 0.0;
    resetEventQueue();
}

function collectEventsForBlock(blockStart, blockEnd, frames) {
    let out = 0;

    // One hot-path pass. Event ordering and sample-offset rounding are
    // identical to Pass 10/11.
    ensureScratch(4096, 0);

    while (eventBatchHead < eventBatches.length) {
        const batch =
            eventBatches[eventBatchHead];

        while (batch.index < batch.times.length) {
            const time =
                batch.times[batch.index];

            if (time >= blockEnd)
                break;

            if (out >= scratchCapacity)
                ensureScratch(out + 1, out);

            const frame =
                Math.max(
                    0,
                    Math.min(
                        frames - 1,
                        Math.round(
                            (time - blockStart) *
                            sampleRateHz)));

            Module.HEAPU32[
                (messagePtr >>> 2) + out] =
                    batch.messages[
                        batch.index];

            Module.HEAPU32[
                (offsetPtr >>> 2) + out] =
                    frame >>> 0;

            ++out;
            ++batch.index;
        }

        if (batch.index >= batch.times.length) {
            ++eventBatchHead;
            continue;
        }

        break;
    }

    if (eventBatchHead > 64 &&
        eventBatchHead * 2 >
            eventBatches.length) {
        eventBatches =
            eventBatches.slice(
                eventBatchHead);
        eventBatchHead = 0;
    }

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

    const header = ringHeader();
    if (!header || !audioRingPcmPtr)
        return false;

    const available =
        Atomics.load(
            header,
            RING_AVAILABLE);

    const freeFrames =
        audioRingCapacityFrames -
        available;

    if (freeFrames < frames)
        return false;

    const writeFrame =
        Atomics.load(
            header,
            RING_WRITE);

    // Capacity is an integer number of synth blocks, so a full synth render
    // never straddles the ring boundary. Keep a defensive guard anyway.
    if (writeFrame < 0 ||
        writeFrame + frames > audioRingCapacityFrames) {
        return false;
    }

    const generation =
        Atomics.load(
            header,
            RING_GENERATION);

    dispatchSysexForBlock(
        blockStart,
        blockEnd,
        frames);

    const eventCount =
        collectEventsForBlock(
            blockStart,
            blockEnd,
            frames);

    const outPtr =
        audioRingPcmPtr +
        writeFrame *
        Math.max(1, synthChannels) *
        4;

    const rendered =
        Module._ssw_render_into(
            outPtr,
            eventCount ? messagePtr : 0,
            eventCount ? offsetPtr : 0,
            eventCount,
            frames);

    if (!rendered)
        throw new Error(
            "SnappySynthV2 shared-ring render failed.");

    // A seek/flush can be issued by the AudioWorklet while pthread rendering
    // is in progress. In that case discard this old-generation block. The
    // worker's queued seek/reset will reset the synth state immediately after
    // this synchronous render returns.
    if (generation !==
        Atomics.load(
            header,
            RING_GENERATION)) {
        return false;
    }

    const nextWrite =
        writeFrame + frames >= audioRingCapacityFrames
            ? 0
            : writeFrame + frames;

    // Atomics publish the already-written PCM with sequential consistency.
    Atomics.store(
        header,
        RING_WRITE,
        nextWrite);
    Atomics.add(
        header,
        RING_AVAILABLE,
        frames);

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
        !audioPort ||
        !audioRingHeaderPtr) {
        return;
    }

    try {
        let guard = 0;
        let produced = 0;
        const header = ringHeader();
        const renderQuantum = Math.max(1, preferredRenderFrames());

        // Proactively pre-render until the rolling cache is full or the main
        // MIDI scheduler's safe horizon is reached. Yield periodically so seek,
        // config and catch-up messages can interrupt long background renders.
        while (header && guard++ < 16) {
            const available =
                Math.max(0, Atomics.load(header, RING_AVAILABLE));
            const freeFrames = audioRingCapacityFrames - available;

            if (freeFrames < Math.max(1, blockFrames))
                break;

            const writeFrame =
                Math.max(0, Atomics.load(header, RING_WRITE));
            const contiguous =
                Math.max(0, audioRingCapacityFrames - writeFrame);

            let safeFrames =
                Math.floor(
                    Math.max(0.0, safeUntil - renderSongTime) *
                    sampleRateHz +
                    1e-6);

            safeFrames =
                Math.floor(safeFrames / Math.max(1, blockFrames)) *
                Math.max(1, blockFrames);

            let frames =
                Math.min(renderQuantum, freeFrames, contiguous, safeFrames);

            frames =
                Math.floor(frames / Math.max(1, blockFrames)) *
                Math.max(1, blockFrames);

            if (frames <= 0 || !renderOneBlock(frames))
                break;

            produced += frames;
        }

        if (produced > 0) {
            publishAudioRing();
            audioPort.postMessage({
                type: "filled",
                frames: produced
            });
        }

        const available =
            header
                ? Math.max(0, Atomics.load(header, RING_AVAILABLE))
                : 0;
        const room = audioRingCapacityFrames - available;
        const safeFramesRemaining =
            Math.floor(
                Math.max(0.0, safeUntil - renderSongTime) *
                sampleRateHz +
                1e-6);

        if (room >= Math.max(1, blockFrames) &&
            safeFramesRemaining >= Math.max(1, blockFrames)) {
            setTimeout(pump, 0);
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
        // Repeated low-water notifications describe the same missing queue
        // capacity; use max rather than addition to avoid duplicate demand.
        pendingNeedFrames =
            Math.max(
                pendingNeedFrames,
                Math.max(
                    0,
                    Number(data.frames) | 0));

        pump();
        // Always acknowledge the demand message. If another trigger filled the
        // ring before this message ran, produced==0 is still healthy and the
        // worklet must be allowed to request again after it drains.
        if (audioPort) {
            audioPort.postMessage({
                type: "needAck"
            });
        }
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
            publishAudioRing(true);
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
            resetAudioRing();
            // SF2 sample/preset allocation may grow Shared WebAssembly.Memory.
            // Refresh the AudioWorklet's memory object without copying PCM.
            publishAudioRing(true);

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
            resetAudioRing();
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
                resetAudioRing();
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
            resetAudioRing();
            // Preserve cross-port demand for the same reason as play(reset).
            resetEventQueue();
            return;
        }

        if (data.type === "stop") {
            playing = false;
            Module._ssw_reset();
            renderSongTime = 0.0;
            resetAudioRing();
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

        if (data.type === "prebuffer") {
            prebufferSeconds =
                Math.max(
                    0.0,
                    Math.min(
                        3600.0,
                        Number(data.seconds) || 0.0));

            midiDuration =
                Math.max(
                    0.0,
                    Number(data.duration) || 0.0);

            if (Module && coreReady) {
                allocateAudioRing();
                resetAudioRing();
                publishAudioRing(true);
            }

            postState("configured", {
                prebufferFrames
            });
            pump();
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

            prebufferSeconds =
                Math.max(
                    0.0,
                    Math.min(
                        3600.0,
                        Number(data.prebufferSeconds) || prebufferSeconds));

            midiDuration =
                Math.max(
                    0.0,
                    Number(data.midiDuration) || midiDuration);

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
                softClip,
                prebufferFrames
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
        softClip,
        prebufferFrames
    });

    pump();
}).catch(setError);
