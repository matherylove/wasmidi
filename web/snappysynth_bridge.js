(() => {
    "use strict";

    if (globalThis.WasmidiSnappyBridge)
        return;

    const state = {
        ready: false,
        soundfontLoaded: false,
        soundfontName: "",
        status: "SnappySynthV2 idle",
        sampleRate: 0,
        activeVoices: 0,
        freeVoices: 0,
        steals: 0,
        underruns: 0,
        layers: 0,
        regions: 0,
        playing: false,
        starved: false,
        audioClock: 0.0,
        audioClockPerf: 0.0,

        // SnappySynth.cfg / source-exposed settings.
        maxVoices: 16384,
        minVoices: 0,
        blockFrames: 512,
        numBuffers: 16,
        requestedSampleRate: 44100, // supplied SnappySynth.cfg default
        channels: 2,
        bitsPerSample: 32,
        realtimePriority: true,

        // voice.c runtime tuning envs.
        workers: 0,            // 0 = auto
        workerCount: 0,        // actual source-selected worker count
        noteSharding: 0,       // 0 auto, 1 channel, 2 hash
        stealScoreCache: true,
        fastNoteOff: true,
        validateState: false,

        // Voice renderer runtime switches.
        softClip: true,
        overlapGain: false,

        volume: 1.00
    };

    let context = null;
    let node = null;
    let worker = null;
    let backendPromise = null;
    let workerReadyPromise = null;
    let workerReadyResolve = null;
    let workerReadyReject = null;
    let input = null;

    function updateStatus(text) {
        state.status = String(text || "");
    }

    function getAudioClock() {
        if (!state.soundfontLoaded)
            return -1.0;

        let value = state.audioClock;

        if (state.playing && !state.starved && state.audioClockPerf > 0.0) {
            value +=
                (performance.now() -
                 state.audioClockPerf) /
                1000.0;
        }

        return value;
    }

    function copyFileName(file) {
        return file && file.name
            ? file.name
            : "soundfont.sf2";
    }

    async function ensureBackend() {
        if (backendPromise)
            return backendPromise;

        backendPromise = (async () => {
            if (!globalThis.crossOriginIsolated) {
                updateStatus(
                    "Preparing SnappySynthV2 audio isolation — reload required");

                throw new Error(
                    "SnappySynthV2 pthread core requires crossOriginIsolated=true. " +
                    "The Pages COI service worker has not taken control yet; reload the page and select the SF2 again.");
            }

            const audioOptions = {
                latencyHint: "interactive"
            };

            // AudioContext supports a requested sample rate in modern browsers.
            // The actual device/context rate is still authoritative and is sent
            // to the synth core after construction so pitch never drifts.
            if (state.requestedSampleRate > 0)
                audioOptions.sampleRate =
                    state.requestedSampleRate;

            context =
                new AudioContext(
                    audioOptions);

            state.sampleRate =
                Math.round(
                    context.sampleRate);

            await context.audioWorklet.addModule(
                "./snappysynth-audio-worklet.js");

            node =
                new AudioWorkletNode(
                    context,
                    "snappysynth-output",
                    {
                        numberOfInputs: 0,
                        numberOfOutputs: 1,
                        outputChannelCount: [2]
                    });

            node.connect(
                context.destination);

            worker =
                new Worker(
                    "./snappysynth-worker.js");

            workerReadyPromise =
                new Promise(
                    (resolve, reject) => {
                        workerReadyResolve = resolve;
                        workerReadyReject = reject;
                    });

            const channel =
                new MessageChannel();

            worker.postMessage({
                type: "audioPort",
                port: channel.port1
            }, [channel.port1]);

            node.port.postMessage({
                type: "workerPort",
                port: channel.port2
            }, [channel.port2]);

            node.port.postMessage({
                type: "config",
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers
            });

            node.port.postMessage({
                type: "volume",
                value: state.volume
            });

            worker.onmessage = event => {
                const data =
                    event.data || {};

                if (Number.isFinite(data.sampleRate))
                    state.sampleRate =
                        Math.round(
                            data.sampleRate);

                if (Number.isFinite(data.activeVoices))
                    state.activeVoices =
                        Math.max(
                            0,
                            Math.round(
                                data.activeVoices));

                if (Number.isFinite(data.freeVoices))
                    state.freeVoices = Math.max(0, Math.round(data.freeVoices));
                if (Number.isFinite(data.steals))
                    state.steals = Math.max(0, Math.round(data.steals));
                if (Number.isFinite(data.layers))
                    state.layers = Math.max(0, Math.round(data.layers));
                if (Number.isFinite(data.regions))
                    state.regions = Math.max(0, Math.round(data.regions));

                if (Number.isFinite(data.workerCount))
                    state.workerCount = Math.max(0, Math.round(data.workerCount));
                if (Number.isFinite(data.channels))
                    state.channels = Number(data.channels) === 1 ? 1 : 2;
                if (Number.isFinite(data.bitsPerSample))
                    state.bitsPerSample = Number(data.bitsPerSample) === 16 ? 16 : 32;
                if (Number.isFinite(data.numBuffers))
                    state.numBuffers = Math.max(1, Math.round(data.numBuffers));

                switch (data.type) {
                case "ready":
                    state.ready = true;

                    if (workerReadyResolve) {
                        workerReadyResolve(true);
                        workerReadyResolve = null;
                        workerReadyReject = null;
                    }

                    updateStatus(
                        "SnappySynthV2 ready — load an SF2");
                    break;

                case "soundfont":
                    state.soundfontLoaded =
                        !!data.loaded;

                    state.soundfontName =
                        String(
                            data.name || "");

                    state.regions =
                        Math.max(
                            0,
                            Number(data.regions) | 0);

                    updateStatus(
                        state.soundfontLoaded
                            ? "SF2 ready"
                            : "SF2 load failed");
                    break;

                case "clock":
                    state.audioClock =
                        Math.max(
                            0.0,
                            Number(data.songTime) ||
                            0.0);

                    state.audioClockPerf =
                        performance.now();

                    state.underruns =
                        Math.max(
                            0,
                            Number(data.underruns) |
                            0);

                    state.starved =
                        !!data.starved;
                    break;

                case "configured":
                    if (typeof data.loaded === "boolean")
                        state.soundfontLoaded = data.loaded;

                    if (Number.isFinite(data.maxVoices))
                        state.maxVoices = Math.max(1, Math.round(data.maxVoices));
                    if (Number.isFinite(data.minVoices))
                        state.minVoices = Math.max(0, Math.round(data.minVoices));
                    if (Number.isFinite(data.blockFrames))
                        state.blockFrames = Math.max(1, Math.round(data.blockFrames));
                    if (Number.isFinite(data.requestedWorkers))
                        state.workers = Math.max(0, Math.round(data.requestedWorkers));
                    if (Number.isFinite(data.noteSharding))
                        state.noteSharding = Math.max(0, Math.min(2, Math.round(data.noteSharding)));
                    if (typeof data.stealScoreCache === "boolean")
                        state.stealScoreCache = data.stealScoreCache;
                    if (typeof data.fastNoteOff === "boolean")
                        state.fastNoteOff = data.fastNoteOff;
                    if (typeof data.validateState === "boolean")
                        state.validateState = data.validateState;
                    if (typeof data.softClip === "boolean")
                        state.softClip = data.softClip;
                    if (typeof data.realtimePriority === "number" ||
                        typeof data.realtimePriority === "boolean")
                        state.realtimePriority = !!data.realtimePriority;

                    updateStatus(
                        state.soundfontLoaded
                            ? "SF2 ready"
                            : "SnappySynthV2 ready — load an SF2");
                    break;

                case "error": {
                    const message =
                        String(
                            data.message ||
                            "SnappySynthV2 error");

                    if (!state.ready &&
                        workerReadyReject) {
                        workerReadyReject(
                            new Error(message));

                        workerReadyResolve = null;
                        workerReadyReject = null;
                    }

                    updateStatus(message);

                    console.error(
                        "[WASMIDI SnappySynthV2]",
                        message);
                    break;
                }
                }
            };

            worker.onerror = error => {
                const message =
                    error && error.message
                        ? String(error.message)
                        : "SnappySynthV2 worker error";

                if (!state.ready &&
                    workerReadyReject) {
                    workerReadyReject(
                        new Error(message));

                    workerReadyResolve = null;
                    workerReadyReject = null;
                }

                updateStatus(message);

                console.error(
                    "[WASMIDI SnappySynthV2 worker]",
                    error);
            };

            // Worker core initialization values.
            worker.postMessage({
                type: "configure",
                sampleRate:
                    state.sampleRate,
                maxVoices:
                    state.maxVoices,
                minVoices:
                    state.minVoices,
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers,
                channels:
                    state.channels,
                bitsPerSample:
                    state.bitsPerSample,
                realtimePriority:
                    state.realtimePriority,
                workers:
                    state.workers,
                noteSharding:
                    state.noteSharding,
                stealScoreCache:
                    state.stealScoreCache,
                fastNoteOff:
                    state.fastNoteOff,
                validateState:
                    state.validateState,
                softClip:
                    state.softClip
            });

            worker.postMessage({
                type: "vor",
                overlapGain:
                    state.overlapGain
            });

            updateStatus(
                "Starting SnappySynthV2 core…");

            // Do not let loadSoundfontFile() post a File until the modularized
            // Emscripten core has instantiated and the worker has sent ready.
            await workerReadyPromise;

            return true;
        })();

        try {
            return await backendPromise;
        } catch (error) {
            backendPromise = null;
            workerReadyPromise = null;
            workerReadyResolve = null;
            workerReadyReject = null;
            throw error;
        }
    }

    async function loadSoundfontFile(file) {
        if (!file)
            return false;

        const addingLayer = state.soundfontLoaded;
        state.soundfontName = copyFileName(file);

        updateStatus(
            addingLayer ? "Adding SoundFont layer…" : "Loading SF2…");

        try {
            await ensureBackend();

            worker.postMessage({
                type: "loadSoundfont",
                file
            });

            return true;
        } catch (error) {
            updateStatus(
                error &&
                error.message
                    ? error.message
                    : "Could not start SnappySynthV2");

            console.error(
                "[WASMIDI] SnappySynthV2 SF2 load failed:",
                error);

            return false;
        }
    }

    function getInput() {
        if (input)
            return input;

        input =
            document.createElement(
                "input");

        input.type = "file";
        input.accept =
            ".sf2,audio/x-soundfont,application/octet-stream";

        input.style.position = "fixed";
        input.style.left = "-10000px";
        input.style.top = "-10000px";
        input.style.width = "1px";
        input.style.height = "1px";
        input.style.opacity = "0";

        document.body.appendChild(input);

        input.onchange = async () => {
            const file =
                input.files &&
                input.files.length
                    ? input.files[0]
                    : null;

            input.value = null;

            if (!file)
                return;

            await loadSoundfontFile(file);
        };

        return input;
    }

    function openSoundfont() {
        // `click()` stays synchronous with the QML button's user gesture.
        const picker =
            getInput();

        picker.click();

        // Start the backend in parallel. The change handler awaits the same
        // promise before handing the File object to WORKERFS.
        ensureBackend().catch(error => {
            console.warn(
                "[WASMIDI] SnappySynth backend not ready:",
                error);
        });
    }

    async function resumeAudio() {
        try {
            await ensureBackend();

            if (context &&
                context.state !== "running") {
                await context.resume();
            }
        } catch (error) {
            console.warn(
                "[WASMIDI] Could not resume SnappySynth audio:",
                error);
        }
    }

    function play(time, reset) {
        const value =
            Math.max(
                0.0,
                Number(time) ||
                0.0);

        state.playing = true;
        state.starved = false;

        if (reset) {
            state.audioClock = value;
            state.audioClockPerf =
                performance.now();
        }

        // A loaded SoundFont means the backend already exists. Post the reset
        // synchronously so the first schedule batch sent by Qt can never race
        // ahead of it and then be erased by a delayed reset.
        if (worker) {
            worker.postMessage({
                type: "play",
                time: value,
                reset: !!reset
            });
        }

        if (node) {
            node.port.postMessage({
                type: "play",
                time: value,
                resetClock: !!reset
            });
        }

        // AudioContext.resume() is intentionally independent of control-message
        // ordering. It still runs from the user's Play gesture.
        resumeAudio();
    }

    function pause() {
        // Capture the extrapolated device clock before clearing `playing`;
        // otherwise getAudioClock() would return the last periodic report and
        // jump backwards by up to one report interval on pause.
        const pausedClock =
            getAudioClock();

        state.playing = false;
        state.audioClock =
            pausedClock;
        state.audioClockPerf =
            performance.now();

        if (worker)
            worker.postMessage({
                type: "pause"
            });

        if (node)
            node.port.postMessage({
                type: "pause"
            });
    }

    function seek(time) {
        const value =
            Math.max(
                0.0,
                Number(time) ||
                0.0);

        state.audioClock = value;
        state.audioClockPerf =
            performance.now();
        state.starved = false;

        if (worker)
            worker.postMessage({
                type: "seek",
                time: value
            });

        if (node) {
            node.port.postMessage({
                type: "flush",
                time: value
            });
        }
    }

    function stop() {
        state.playing = false;
        state.starved = false;
        state.audioClock = 0.0;
        state.audioClockPerf =
            performance.now();

        if (worker)
            worker.postMessage({
                type: "stop"
            });

        if (node) {
            node.port.postMessage({
                type: "pause"
            });

            node.port.postMessage({
                type: "flush",
                time: 0.0
            });
        }
    }

    function schedule(messages, times, safeUntil) {
        if (!worker ||
            !state.soundfontLoaded)
            return;

        worker.postMessage({
            type: "schedule",
            messages,
            times,
            safeUntil:
                Number(safeUntil) ||
                0.0
        }, [
            messages.buffer,
            times.buffer
        ]);
    }

    function scheduleSysEx(bytes, time) {
        if (!worker || !state.soundfontLoaded || !(bytes instanceof Uint8Array))
            return;
        worker.postMessage({
            type: "sysex",
            bytes,
            time: Math.max(0.0, Number(time) || 0.0)
        }, [bytes.buffer]);
    }

    function clearSoundfonts() {
        state.soundfontLoaded = false;
        state.soundfontName = "";
        state.layers = 0;
        state.regions = 0;
        updateStatus("SnappySynthV2 ready — load an SF2");
        if (worker) worker.postMessage({ type: "clearSoundfonts" });
        if (node) {
            node.port.postMessage({ type: "pause" });
            node.port.postMessage({ type: "flush", time: 0.0 });
        }
    }

    function setVolume(percent) {
        state.volume =
            Math.max(
                0.0,
                Math.min(
                    1.0,
                    Number(percent) /
                    100.0));

        // UI volume is an output gain after SnappySynth. This does not overwrite
        // the synth's MIDI Universal Master Volume / GS state.
        if (node) {
            node.port.postMessage({
                type: "volume",
                value: state.volume
            });
        }
    }

    function setOverlapGain(enabled) {
        state.overlapGain =
            !!enabled;

        if (worker) {
            worker.postMessage({
                type: "vor",
                overlapGain:
                    state.overlapGain
            });
        }
    }

    function configure(options, legacyFrames) {
        // Backward compatibility with Pass 9/10 callers.
        if (typeof options !== "object" || options === null) {
            options = {
                maxVoices: options,
                blockFrames: legacyFrames
            };
        }

        if (Number.isFinite(Number(options.maxVoices))) {
            state.maxVoices =
                Math.max(
                    1,
                    Math.min(
                        5000000,
                        Number(options.maxVoices) |
                        0));
        }

        if (Number.isFinite(Number(options.minVoices))) {
            state.minVoices =
                Math.max(
                    0,
                    Math.min(
                        5000000,
                        Number(options.minVoices) |
                        0));
        }

        if (Number.isFinite(Number(options.blockFrames))) {
            state.blockFrames =
                Math.max(
                    1,
                    Math.min(
                        65536,
                        Number(options.blockFrames) |
                        0));
        }

        if (Number.isFinite(Number(options.numBuffers))) {
            state.numBuffers =
                Math.max(
                    1,
                    Math.min(
                        128,
                        Number(options.numBuffers) |
                        0));
        }

        if (Number.isFinite(Number(options.requestedSampleRate))) {
            state.requestedSampleRate =
                Math.max(
                    0,
                    Math.min(
                        384000,
                        Number(options.requestedSampleRate) |
                        0));
        }

        if (Number.isFinite(Number(options.channels)))
            state.channels =
                Number(options.channels) === 1 ? 1 : 2;

        if (Number.isFinite(Number(options.bitsPerSample)))
            state.bitsPerSample =
                Number(options.bitsPerSample) === 16 ? 16 : 32;

        if (typeof options.realtimePriority === "boolean")
            state.realtimePriority =
                options.realtimePriority;

        if (Number.isFinite(Number(options.workers))) {
            state.workers =
                Math.max(
                    0,
                    Math.min(
                        256,
                        Number(options.workers) |
                        0));
        }

        if (Number.isFinite(Number(options.noteSharding))) {
            state.noteSharding =
                Math.max(
                    0,
                    Math.min(
                        2,
                        Number(options.noteSharding) |
                        0));
        }

        if (typeof options.stealScoreCache === "boolean")
            state.stealScoreCache =
                options.stealScoreCache;

        if (typeof options.fastNoteOff === "boolean")
            state.fastNoteOff =
                options.fastNoteOff;

        if (typeof options.validateState === "boolean")
            state.validateState =
                options.validateState;

        if (typeof options.softClip === "boolean")
            state.softClip =
                options.softClip;

        if (node) {
            node.port.postMessage({
                type: "pause"
            });

            node.port.postMessage({
                type: "flush",
                time:
                    state.audioClock
            });

            node.port.postMessage({
                type: "config",
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers
            });
        }

        if (worker) {
            // Core reinitialization preserves the merged instrument and reapplies
            // presampling at the active AudioContext sample rate.
            state.soundfontLoaded = false;

            worker.postMessage({
                type: "configure",
                sampleRate:
                    state.sampleRate,
                maxVoices:
                    state.maxVoices,
                minVoices:
                    state.minVoices,
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers,
                channels:
                    state.channels,
                bitsPerSample:
                    state.bitsPerSample,
                realtimePriority:
                    state.realtimePriority,
                workers:
                    state.workers,
                noteSharding:
                    state.noteSharding,
                stealScoreCache:
                    state.stealScoreCache,
                fastNoteOff:
                    state.fastNoteOff,
                validateState:
                    state.validateState,
                softClip:
                    state.softClip
            });
        }

        updateStatus(
            worker
                ? "Reconfiguring SnappySynthV2…"
                : "SnappySynthV2 settings ready");
    }

    globalThis.WasmidiSnappyBridge = {
        state,
        ensureBackend,
        loadSoundfontFile,
        openSoundfont,
        play,
        pause,
        stop,
        seek,
        schedule,
        scheduleSysEx,
        clearSoundfonts,
        setVolume,
        setOverlapGain,
        configure,
        getAudioClock
    };
})();
