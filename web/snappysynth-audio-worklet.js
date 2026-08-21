class SnappySynthOutputProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        this.workerPort = null;

        this.playing = false;
        this.baseSongTime = 0.0;
        this.playedFrames = 0;
        this.lastClockReportFrame = 0;
        this.underruns = 0;
        this.starved = false;

        // Qt's horizontal visualizer is the transport master. Audio may run a
        // small lead (one worst-case 50 ms visual step plus one worklet block)
        // to stay continuous at normal FPS, but it is never allowed to consume
        // indefinitely when rendering stalls.
        this.visualClockEnabled = false;
        this.visualClockTime = 0.0;
        this.visualLeadSeconds = 0.055;

        this.blockFrames = 512;
        this.numBuffers = 16;
        // Preserve Pass 10's post-synth gain semantics exactly. The shared-ring
        // optimization removes PCM transport copies, not the audible gain stage.
        this.outputGain = 1.0;
        this.lowWaterFrames = this.blockFrames * Math.max(2, Math.floor(this.numBuffers / 2));
        this.highWaterFrames = this.blockFrames * this.numBuffers;
        this.needOutstanding = false;

        // Shared WebAssembly.Memory ring descriptor. PCM stays interleaved in
        // exactly the buffer written by voice_render_float(); this processor
        // only deinterleaves into WebAudio's planar output buffers.
        this.RING_READ = 0;
        this.RING_WRITE = 1;
        this.RING_AVAILABLE = 2;
        this.RING_GENERATION = 3;
        this.RING_CAPACITY = 4;
        this.RING_CHANNELS = 5;
        this.RING_BLOCK_FRAMES = 6;
        this.RING_WORDS = 8;

        this.sharedMemory = null;
        this.headerPtr = 0;
        this.pcmPtr = 0;
        this.ringHeader = null;
        this.ringPcm = null;
        this.capacityFrames = 0;
        this.channels = 2;

        this.port.onmessage = event => {
            const data = event.data || {};

            if (data.type === "workerPort" && data.port) {
                this.workerPort = data.port;
                this.workerPort.onmessage = e => this.onWorkerMessage(e.data || {});
                this.workerPort.start();
                this.requestAudioIfNeeded();
                return;
            }

            if (data.type === "play") {
                if (data.resetClock) {
                    this.flushRing();
                    this.baseSongTime = Number(data.time) || 0.0;
                    this.playedFrames = 0;
                    this.lastClockReportFrame = 0;
                }
                this.visualClockEnabled = true;
                this.visualClockTime = Math.max(0.0, Number(data.time) || 0.0);
                this.playing = true;
                this.requestAudioIfNeeded();
                return;
            }

            if (data.type === "visualClock") {
                this.visualClockEnabled = true;
                this.visualClockTime =
                    Math.max(0.0, Number(data.time) || 0.0);
                return;
            }

            if (data.type === "pause") {
                this.playing = false;
                return;
            }

            if (data.type === "flush") {
                this.flushRing();
                if (Number.isFinite(data.time)) {
                    this.baseSongTime = Number(data.time);
                    this.visualClockTime = Math.max(0.0, Number(data.time));
                    this.visualClockEnabled = true;
                    this.playedFrames = 0;
                    this.lastClockReportFrame = 0;
                }
                return;
            }

            if (data.type === "volume") {
                this.outputGain =
                    Math.max(
                        0.0,
                        Math.min(
                            1.0,
                            Number(data.value) || 0.0));
                return;
            }

            if (data.type === "config") {
                const block =
                    Math.max(
                        1,
                        Number(data.blockFrames) | 0);

                const buffers =
                    Math.max(
                        1,
                        Math.min(
                            128,
                            Number(data.numBuffers) | 0 || this.numBuffers));

                this.blockFrames = block;
                this.numBuffers = buffers;
                this.updateWatermarks();
                this.requestAudioIfNeeded();
            }
        };
    }

    updateWatermarks() {
        const configuredHigh =
            Math.max(
                this.blockFrames,
                this.blockFrames * this.numBuffers);

        // When the Worker exposes a deep rolling pre-render ring, use the full
        // ring as the high-water mark instead of intentionally draining it back
        // to the old real-time NumBuffers depth. This keeps expensive synth work
        // several seconds ahead of the device whenever possible.
        const hardCapacity =
            this.capacityFrames > 0
                ? this.capacityFrames
                : configuredHigh;

        this.highWaterFrames = hardCapacity;

        if (this.highWaterFrames < 128 && hardCapacity >= 128)
            this.highWaterFrames = Math.min(hardCapacity, 256);

        this.lowWaterFrames =
            Math.max(
                Math.min(this.blockFrames, this.highWaterFrames),
                Math.floor(this.highWaterFrames / 2));
    }

    bindSharedRing(data) {
        if (!(data.memory instanceof SharedArrayBuffer))
            return;

        const headerPtr = Number(data.headerPtr) | 0;
        const pcmPtr = Number(data.pcmPtr) | 0;
        const capacityFrames = Math.max(1, Number(data.capacityFrames) | 0);
        const channels = Number(data.channels) === 1 ? 1 : 2;

        this.sharedMemory = data.memory;
        this.headerPtr = headerPtr;
        this.pcmPtr = pcmPtr;
        this.capacityFrames = capacityFrames;
        this.channels = channels;

        this.ringHeader =
            new Int32Array(
                this.sharedMemory,
                this.headerPtr,
                this.RING_WORDS);

        this.ringPcm =
            new Float32Array(
                this.sharedMemory,
                this.pcmPtr,
                this.capacityFrames * this.channels);

        if (Number.isFinite(data.blockFrames))
            this.blockFrames = Math.max(1, Number(data.blockFrames) | 0);

        this.needOutstanding = false;
        this.updateWatermarks();
        this.requestAudioIfNeeded();
    }

    flushRing() {
        if (this.ringHeader) {
            // Invalidate any block currently being rendered by the Worker.
            Atomics.add(this.ringHeader, this.RING_GENERATION, 1);
            Atomics.store(this.ringHeader, this.RING_READ, 0);
            Atomics.store(this.ringHeader, this.RING_WRITE, 0);
            Atomics.store(this.ringHeader, this.RING_AVAILABLE, 0);
        }

        this.needOutstanding = false;
        this.starved = false;
    }

    onWorkerMessage(data) {
        if (data.type === "sharedRing") {
            this.bindSharedRing(data);
            return;
        }

        if (data.type === "filled" || data.type === "needAck") {
            this.needOutstanding = false;
            this.requestAudioIfNeeded();
        }
    }

    requestAudioIfNeeded() {
        if (!this.workerPort || !this.playing || !this.ringHeader || this.needOutstanding)
            return;

        const available =
            Math.max(
                0,
                Atomics.load(
                    this.ringHeader,
                    this.RING_AVAILABLE));

        if (available >= this.lowWaterFrames)
            return;

        const missing =
            Math.max(
                this.blockFrames,
                this.highWaterFrames - available);

        // Worker renders only full source blocks. This preserves the exact
        // AudioConfig.buffer_size cadence used by the original engine.
        const frames =
            Math.ceil(
                missing /
                Math.max(1, this.blockFrames)) *
            Math.max(1, this.blockFrames);

        this.needOutstanding = true;
        this.workerPort.postMessage({
            type: "need",
            frames
        });
    }

    reportClockIfNeeded(force = false) {
        if (!this.workerPort)
            return;

        if (!force &&
            this.playedFrames - this.lastClockReportFrame < 2048)
            return;

        this.lastClockReportFrame = this.playedFrames;

        this.workerPort.postMessage({
            type: "clock",
            songTime:
                this.baseSongTime +
                this.playedFrames / sampleRate,
            underruns: this.underruns,
            starved: this.starved
        });
    }

    copyFromRing(left, right, written, frames) {
        if (!this.ringHeader ||
            !this.ringPcm ||
            frames <= 0)
            return 0;

        let readFrame =
            Atomics.load(
                this.ringHeader,
                this.RING_READ);

        if (readFrame < 0 ||
            readFrame >= this.capacityFrames)
            readFrame = 0;

        let remaining = frames;
        let out = written;
        const unity =
            this.outputGain === 1.0;

        while (remaining > 0) {
            const contiguous =
                Math.min(
                    remaining,
                    this.capacityFrames - readFrame);

            if (this.channels === 1) {
                let src = readFrame;
                const limit = out + contiguous;

                if (unity) {
                    for (let dst = out; dst < limit; ++dst) {
                        const sample = this.ringPcm[src++];
                        left[dst] = sample;
                        right[dst] = sample;
                    }
                } else {
                    const gain = this.outputGain;
                    for (let dst = out; dst < limit; ++dst) {
                        const sample =
                            this.ringPcm[src++] * gain;
                        left[dst] = sample;
                        right[dst] = sample;
                    }
                }
            } else {
                let src = readFrame * 2;
                const limit = out + contiguous;

                if (unity) {
                    for (let dst = out; dst < limit; ++dst) {
                        left[dst] = this.ringPcm[src++];
                        right[dst] = this.ringPcm[src++];
                    }
                } else {
                    const gain = this.outputGain;
                    for (let dst = out; dst < limit; ++dst) {
                        left[dst] =
                            this.ringPcm[src++] * gain;
                        right[dst] =
                            this.ringPcm[src++] * gain;
                    }
                }
            }

            out += contiguous;
            remaining -= contiguous;
            readFrame += contiguous;

            if (readFrame >= this.capacityFrames)
                readFrame = 0;
        }

        Atomics.store(
            this.ringHeader,
            this.RING_READ,
            readFrame);

        Atomics.sub(
            this.ringHeader,
            this.RING_AVAILABLE,
            frames);

        return frames;
    }

    process(inputs, outputs) {
        const output = outputs[0];

        if (!output || output.length === 0)
            return true;

        const left = output[0];
        const right = output.length > 1 ? output[1] : output[0];
        const frames = left.length;

        if (!this.playing ||
            !this.ringHeader ||
            !this.ringPcm) {
            left.fill(0.0);
            if (right !== left)
                right.fill(0.0);

            this.requestAudioIfNeeded();
            return true;
        }

        const available =
            Math.max(
                0,
                Atomics.load(
                    this.ringHeader,
                    this.RING_AVAILABLE));

        let visuallyAllowed = frames;
        if (this.visualClockEnabled) {
            const audioTime =
                this.baseSongTime +
                this.playedFrames / sampleRate;
            const visualCeiling =
                this.visualClockTime +
                this.visualLeadSeconds;
            visuallyAllowed =
                Math.max(
                    0,
                    Math.min(
                        frames,
                        Math.floor(
                            (visualCeiling - audioTime) *
                            sampleRate + 1e-6)));
        }

        const count =
            Math.min(
                visuallyAllowed,
                available);

        const written =
            this.copyFromRing(
                left,
                right,
                0,
                count);

        const starvedNow =
            written < visuallyAllowed;

        if (written < frames) {
            left.fill(0.0, written);
            if (right !== left)
                right.fill(0.0, written);
        }

        if (starvedNow)
            ++this.underruns;

        // The clock still advances only for PCM actually delivered to the
        // device, exactly as in Pass 10.
        this.playedFrames += written;

        const starvationChanged =
            starvedNow !== this.starved;
        this.starved = starvedNow;

        this.reportClockIfNeeded(starvationChanged);
        this.requestAudioIfNeeded();

        return true;
    }
}

registerProcessor("snappysynth-output", SnappySynthOutputProcessor);
