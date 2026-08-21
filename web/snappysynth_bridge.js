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
        underruns: 0,
        regions: 0,
        playing: false,
        starved: false,
        audioClock: 0.0,
        audioClockPerf: 0.0,
        maxVoices: 16384,
        blockFrames: 512,
        overlapGain: false,
        volume: 0.80
    };

    let context = null;
    let node = null;
    let worker = null;
    let backendPromise = null;
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

            context =
                new AudioContext({
                    latencyHint: "interactive"
                });

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
                    state.blockFrames
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

                switch (data.type) {
                case "ready":
                    state.ready = true;
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
                    updateStatus(
                        state.soundfontLoaded
                            ? "SF2 ready"
                            : "SnappySynthV2 ready — load an SF2");
                    break;

                case "error":
                    updateStatus(
                        String(
                            data.message ||
                            "SnappySynthV2 error"));
                    console.error(
                        "[WASMIDI SnappySynthV2]",
                        data.message);
                    break;
                }
            };

            worker.onerror = error => {
                updateStatus(
                    "SnappySynthV2 worker error");

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
                blockFrames:
                    state.blockFrames
            });

            worker.postMessage({
                type: "volume",
                value:
                    state.volume
            });

            worker.postMessage({
                type: "vor",
                overlapGain:
                    state.overlapGain
            });

            updateStatus(
                "Starting SnappySynthV2…");

            return true;
        })();

        try {
            return await backendPromise;
        } catch (error) {
            backendPromise = null;
            throw error;
        }
    }

    async function loadSoundfontFile(file) {
        if (!file)
            return false;

        state.soundfontLoaded = false;
        state.soundfontName =
            copyFileName(file);

        updateStatus(
            "Loading SF2…");

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

    function setVolume(percent) {
        state.volume =
            Math.max(
                0.0,
                Math.min(
                    1.0,
                    Number(percent) /
                    100.0));

        if (worker) {
            worker.postMessage({
                type: "volume",
                value:
                    state.volume
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

    function configure(maxVoices, frames) {
        state.maxVoices =
            Math.max(
                128,
                Number(maxVoices) | 0);

        state.blockFrames =
            Math.max(
                128,
                Number(frames) | 0);

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
                    state.blockFrames
            });
        }

        if (worker) {
            // Reinitializing the voice engine invalidates the loaded instrument
            // for a moment. Expose that transition so Qt does not try to prime
            // playback against the old core generation.
            state.soundfontLoaded = false;

            worker.postMessage({
                type: "configure",
                sampleRate:
                    state.sampleRate,
                maxVoices:
                    state.maxVoices,
                blockFrames:
                    state.blockFrames
            });
        }

        updateStatus(
            "Reconfiguring SnappySynthV2…");
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
        setVolume,
        setOverlapGain,
        configure,
        getAudioClock
    };
})();
