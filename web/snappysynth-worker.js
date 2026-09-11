/* global SnappySynthCore */

"use strict";

importScripts("./snappysynth-core.js?v=13.12.0");

let Module = null;
let coreReady = false;
let audioPort = null;

let sampleRateHz = 44100;
let blockFrames = 512;
let numBuffers = 16;
// Audio is rendered as a realtime transport queue. There is deliberately no
// seconds-long PCM prerender cache; only the configured NumBuffers worth of
// device safety audio stays resident.
let prebufferSeconds = 0.0;
let midiDuration = 0.0;
let prebufferFrames = 0;
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
// After play(reset)/seek, wait until the MIDI producer has acknowledged the
// new timeline at least once. Previously an AudioWorklet `need` could race the
// parser reset and make us fill NumBuffers with valid *silence* before the
// first MIDI batch arrived, producing a multi-buffer startup delay every Play.
let startupWaitingForSchedule = false;

// Schedule batches are copied once into the core's C-side chronological queue.
// This keeps per-event time->sample classification out of the JavaScript audio
// hot path on multi-million-NPS Black MIDIs.
let messagePtr = 0;
let timePtr = 0;
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
    // Match the supplied SnappySynthV2 driver's BufferSize semantics exactly.
    // The native driver invokes voice_render_float() one configured source
    // block at a time. Larger browser-only mega-blocks reduced message/futex
    // traffic, but they also lengthened a single synchronous render enough to
    // starve control/event delivery on dense files.
    return Math.max(1, blockFrames);
}

function requestedPrebufferFrames() {
    const transportFrames =
        Math.max(1, blockFrames) *
        Math.max(1, numBuffers);
    const quantum = Math.max(1, preferredRenderFrames());
    const aligned = Math.ceil(transportFrames / quantum) * quantum;
    return Math.max(quantum, aligned);
}

function allocateAudioRing() {
    if (!Module)
        return;

    freeAudioRing();

    // The shared ring is only the realtime device queue. It is never sized from
    // MIDI duration or a seconds-long prerender preference.
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
    if (coreReady && Module && Module._ssw_clear_events)
        Module._ssw_clear_events();
    sysexEvents.length = 0;
    safeUntil = renderSongTime;
}

function freeScratch() {
    if (!Module)
        return;

    if (messagePtr)
        Module._free(messagePtr);
    if (timePtr)
        Module._free(timePtr);

    messagePtr = 0;
    timePtr = 0;
    scratchCapacity = 0;
}

function ensureScratch(required) {
    if (required <= scratchCapacity)
        return;

    let capacity = Math.max(4096, scratchCapacity || 4096);
    while (capacity < required)
        capacity *= 2;

    const newMessagePtr = Module._malloc(capacity * 4);
    const newTimePtr = Module._malloc(capacity * 8);
    if (!newMessagePtr || !newTimePtr) {
        if (newMessagePtr) Module._free(newMessagePtr);
        if (newTimePtr) Module._free(newTimePtr);
        throw new Error(
            "Could not allocate SnappySynth schedule scratch buffers.");
    }

    if (messagePtr) Module._free(messagePtr);
    if (timePtr) Module._free(timePtr);
    messagePtr = newMessagePtr;
    timePtr = newTimePtr;
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

// WORKER_FREELIST_REFILL in voice.c: a worker takes this many voices at a time
// from the global pool, and it can only steal voices it owns. A worker that
// never gets one batch is dead weight, so this is the smallest per-worker value
// that actually does anything.
const VOICES_PER_WORKER_MIN = 512;
// Matches the Voices field's own maxValue, so the UI and the worker agree on
// the derived pool size and this clamp effectively never binds.
const VOICE_POOL_CEILING = 5000000;

/*
 * Voice pool sizing.
 *
 * With Workers = 0 the engine owns the policy and `maxVoices` is the total pool,
 * exactly as before.
 *
 * With an explicit Workers count, `maxVoices` means voices PER WORKER and the
 * pool is sized from it. Sizing the pool this way removes a configuration that
 * could not work: the voice pool is shared and handed out in 512-voice batches,
 * so asking for more workers than the pool can seed leaves the surplus workers
 * with nothing to allocate or steal, and the notes routed to them are lost. It
 * also matches how the cost actually behaves -- render time scales with ACTIVE
 * voices, not with the cap, so a pool sized for the thread count costs nothing
 * until the music genuinely uses it.
 */
function resolveVoicePool() {
    if (requestedWorkers <= 0) {
        return {
            total: maxVoices,
            perWorker: 0,
            workers: 0
        };
    }

    const perWorker = Math.max(1, maxVoices);
    const total = Math.min(
        VOICE_POOL_CEILING,
        perWorker * requestedWorkers);

    return {
        total,
        perWorker,
        workers: requestedWorkers
    };
}

let voicePool = { total: 16384, perWorker: 0, workers: 0 };

function initCore() {
    if (!Module)
        return;

    voicePool = resolveVoicePool();

    Module._ssw_init_ex(
        sampleRateHz,
        synthChannels,
        bitsPerSample,
        blockFrames,
        numBuffers,
        realtimePriority,
        voicePool.total,
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
        // Derived pool size. Reported under its own key: the bridge feeds
        // `maxVoices` straight back into the stored setting, so publishing the
        // product there would multiply it again on every reconfigure.
        totalVoices: voicePool.total,
        voicesPerWorker: voicePool.perWorker,
        voicesPerWorkerMin: VOICES_PER_WORKER_MIN,
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
    Module._ssw_set_song_time(0.0);
    resetEventQueue();
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

function admitSysExBatch(meta, bytes, times) {
    if (!(meta instanceof Uint32Array) ||
        !(bytes instanceof Uint8Array) ||
        !(times instanceof Float64Array)) {
        return;
    }

    const count = Math.min(times.length, Math.floor(meta.length / 3));
    for (let i = 0; i < count; ++i) {
        const offset = meta[i * 3 + 1] >>> 0;
        const length = meta[i * 3 + 2] >>> 0;
        if (!length || offset > bytes.length || length > bytes.length - offset)
            continue;
        const event = {
            // subarray retains one transferred arena; no per-SysEx payload
            // copy is needed on the realtime worker.
            bytes: bytes.subarray(offset, offset + length),
            time: Math.max(0.0, Number(times[i]) || 0.0)
        };
        let pos = sysexEvents.length;
        while (pos > 0 && sysexEvents[pos - 1].time > event.time) --pos;
        sysexEvents.splice(pos, 0, event);
    }
}

function admitShortSchedule(messages, times) {
    if (!messages || !times ||
        messages.length !== times.length || messages.length <= 0) {
        return;
    }

    const count = messages.length | 0;
    ensureScratch(count);
    Module.HEAPU32.set(messages, messagePtr >>> 2);
    Module.HEAPF64.set(times, timePtr >>> 3);
    if (!Module._ssw_queue_events(messagePtr, timePtr, count)) {
        throw new Error(
            "SnappySynthV2 could not queue the MIDI schedule batch.");
    }
}

function renderQueuedWithSysex(outPtr, blockStart, frames) {
    const blockEnd = blockStart + frames / sampleRateHz;
    const strideBytes = Math.max(1, synthChannels) * 4;
    let cursorFrame = 0;

    // SysEx is normally sparse, so splitting only at its exact sample costs
    // essentially nothing in note-heavy MIDIs while preventing a GM/GS/XG
    // reset or tuning message near the end of a block from affecting the
    // beginning of that block. CC/program/pitch segmentation happens inside
    // ssw_render_queued_into() and remains on the same absolute sample clock.
    while (sysexEvents.length > 0 && sysexEvents[0].time < blockEnd) {
        const event = sysexEvents.shift();
        const bytes = event.bytes;
        const frame = Math.max(0, Math.min(frames - 1,
            Math.round((event.time - blockStart) * sampleRateHz)));

        if (frame > cursorFrame) {
            if (!Module._ssw_render_queued_into(
                    outPtr + cursorFrame * strideBytes,
                    frame - cursorFrame)) {
                return false;
            }
            cursorFrame = frame;
        }

        if (bytes instanceof Uint8Array && bytes.length > 0) {
            ensureSysexScratch(bytes.length);
            Module.HEAPU8.set(bytes, sysexPtr);
            // The C render cursor is now exactly at this SysEx sample.
            Module._ssw_send_sysex(sysexPtr, bytes.length, 0);
        }
    }

    if (cursorFrame < frames) {
        return !!Module._ssw_render_queued_into(
            outPtr + cursorFrame * strideBytes,
            frames - cursorFrame);
    }
    return true;
}

function renderOneBlock(frames) {
    const blockStart =
        renderSongTime;

    const blockEnd =
        blockStart +
        frames / sampleRateHz;

    // Never gate PCM rendering on MIDI event coverage. The original
    // SnappySynth realtime backend keeps rendering the device timeline even
    // when the producer has not yet supplied a future MIDI event. `safeUntil`
    // is retained only as a scheduling/diagnostic watermark; using it as a hard
    // PCM stop is what produced the deterministic "about one second, then
    // silence" failure in WASMIDI. The C-side scheduled-event queue clamps late
    // events to the first not-yet-rendered sample while the multi-second event
    // scheduler normally keeps that path well ahead of the device.

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

    const outPtr =
        audioRingPcmPtr +
        writeFrame *
        Math.max(1, synthChannels) *
        4;

    const rendered = renderQueuedWithSysex(outPtr, blockStart, frames);

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
        startupWaitingForSchedule ||
        !audioPort ||
        !audioRingHeaderPtr ||
        pendingNeedFrames <= 0) {
        return;
    }

    try {
        let guard = 0;
        let produced = 0;
        const header = ringHeader();
        const renderQuantum = Math.max(1, preferredRenderFrames());

        // Render strictly in response to AudioWorklet demand. The ring is a
        // realtime transport queue, not a background PCM prerender cache.
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

            let frames =
                Math.min(
                    renderQuantum,
                    freeFrames,
                    contiguous,
                    Math.max(blockFrames, pendingNeedFrames));

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

        if (pendingNeedFrames > 0 &&
            room >= Math.max(1, blockFrames)) {
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

        // Demand is idempotent: keep the largest outstanding deficit. The
        // Worker always renders PCM continuously; MIDI coverage must never turn
        // this into a hard stop. A successful fill is the acknowledgement the
        // Worklet actually needs.
        pump();
        return;
    }

    if (data.type === "clock") {
        postState("clock", {
            epoch:
                Number(data.epoch) >>> 0,
            songTime:
                Number(data.songTime) || 0.0,
            underruns:
                Number(data.underruns) | 0,
            starved:
                !!data.starved
        });

        // A demand can remain outstanding while the ring was temporarily full
        // or while a long synth block was rendering. Clock messages provide a
        // cheap periodic retry after the device has consumed more frames.
        pump();
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
            Module._ssw_set_song_time(0.0);
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
            Module._ssw_set_song_time(0.0);
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

        if (data.type === "sysexBatch") {
            admitSysExBatch(data.meta, data.bytes, data.times);
            pump();
            return;
        }

        if (data.type === "scheduleBatch") {
            // Admit both streams before pumping.  This preserves sample timing
            // while preventing a same-batch GM/GS/XG reset from being delivered
            // after Program Change/Bank Select audio has already rendered.
            admitSysExBatch(data.meta, data.bytes, data.sysexTimes);
            admitShortSchedule(data.messages, data.times);

            safeUntil = Math.max(
                safeUntil,
                Number(data.safeUntil) || renderSongTime);
            startupWaitingForSchedule = false;
            pump();
            return;
        }

        if (data.type === "schedule") {
            const messages =
                data.messages;

            const times =
                data.times;

            admitShortSchedule(messages, times);

            safeUntil =
                Math.max(
                    safeUntil,
                    Number(data.safeUntil) ||
                    renderSongTime);

            // Even an empty schedule batch is a valid producer ACK (for a
            // silent interval). It is now safe to satisfy AudioWorklet demand
            // without accidentally pre-filling the ring from the old/reset
            // state.
            startupWaitingForSchedule = false;

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
                Module._ssw_set_song_time(renderSongTime);
                resetAudioRing();
                // Do not clear pendingNeedFrames here. The AudioWorklet sends
                // its fresh request on a different MessagePort and that request
                // may arrive just before this reset. Clearing it would leave the
                // worklet believing audio is on the way and cause a deadlock.
                resetEventQueue();
                startupWaitingForSchedule = true;
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
            Module._ssw_set_song_time(renderSongTime);
            resetAudioRing();
            // Preserve cross-port demand for the same reason as play(reset).
            resetEventQueue();
            startupWaitingForSchedule = playing;
            return;
        }

        if (data.type === "stop") {
            playing = false;
            Module._ssw_reset();
            renderSongTime = 0.0;
            Module._ssw_set_song_time(0.0);
            resetAudioRing();
            pendingNeedFrames = 0;
            resetEventQueue();
            startupWaitingForSchedule = false;
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
            // Protocol compatibility only. PCM pre-rendering no longer exists.
            // Most importantly, receiving this legacy setting must NOT tear
            // down/reset the live shared ring: MainWindow sends configuration
            // updates while a MIDI is being installed, and reallocating here
            // used to erase perfectly good realtime audio and leave the
            // AudioWorklet underrunning until another demand cycle happened.
            prebufferSeconds = 0.0;

            midiDuration =
                Math.max(
                    0.0,
                    Number(data.duration) || 0.0);

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

            prebufferSeconds = 0.0;

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

            // The bridge intentionally sends configuration immediately after
            // creating this Worker so startup settings are available before the
            // Emscripten core is instantiated. During that bootstrap window
            // Module is still null. Keep the settings above, but do not touch
            // exported WASM functions until SnappySynthCore(...).then() has
            // completed. initCore() below will consume these already-updated
            // values on its first initialization.
            if (!Module || !coreReady)
                return;

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
        totalVoices: voicePool.total,
        voicesPerWorker: voicePool.perWorker,
        voicesPerWorkerMin: VOICES_PER_WORKER_MIN,
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
