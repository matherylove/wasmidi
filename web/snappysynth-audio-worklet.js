class SnappySynthOutputProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        this.workerPort = null;
        this.queue = [];
        this.queueOffsetFrames = 0;
        this.queuedFrames = 0;
        this.requestedFrames = 0;

        this.playing = false;
        this.baseSongTime = 0.0;
        this.playedFrames = 0;
        this.lastClockReportFrame = 0;
        this.underruns = 0;
        this.starved = false;

        this.blockFrames = 512;
        this.lowWaterFrames = 4096;
        this.highWaterFrames = 8192;

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
                    this.flush();
                    this.baseSongTime = Number(data.time) || 0.0;
                    this.playedFrames = 0;
                    this.lastClockReportFrame = 0;
                }
                this.playing = true;
                this.requestAudioIfNeeded();
                return;
            }

            if (data.type === "pause") {
                this.playing = false;
                return;
            }

            if (data.type === "flush") {
                this.flush();
                if (Number.isFinite(data.time)) {
                    this.baseSongTime = Number(data.time);
                    this.playedFrames = 0;
                    this.lastClockReportFrame = 0;
                }
                return;
            }

            if (data.type === "config") {
                const block = Math.max(128, Number(data.blockFrames) | 0);
                this.blockFrames = block;
                this.lowWaterFrames = Math.max(2048, block * 4);
                this.highWaterFrames = Math.max(4096, block * 10);
                this.requestAudioIfNeeded();
            }
        };
    }

    recycleChunk(chunk) {
        if (!this.workerPort ||
            !chunk ||
            !chunk.pcm ||
            !chunk.pcm.buffer ||
            chunk.pcm.buffer.byteLength === 0) {
            return;
        }

        const buffer = chunk.pcm.buffer;
        this.workerPort.postMessage({
            type: "recycle",
            buffer
        }, [buffer]);
    }

    flush() {
        for (let i = 0; i < this.queue.length; ++i)
            this.recycleChunk(this.queue[i]);

        this.queue.length = 0;
        this.queueOffsetFrames = 0;
        this.queuedFrames = 0;
        this.requestedFrames = 0;
        this.starved = false;
    }

    onWorkerMessage(data) {
        if (data.type !== "pcm" || !(data.pcm instanceof Float32Array))
            return;

        const frames = Number(data.frames) | 0;
        if (frames <= 0)
            return;

        this.queue.push({
            pcm: data.pcm,
            frames
        });

        this.queuedFrames += frames;
        this.requestedFrames = Math.max(0, this.requestedFrames - frames);
        this.requestAudioIfNeeded();
    }

    requestAudioIfNeeded() {
        if (!this.workerPort || !this.playing)
            return;

        const accounted = this.queuedFrames + this.requestedFrames;

        if (accounted >= this.lowWaterFrames)
            return;

        const frames = Math.max(
            this.blockFrames,
            this.highWaterFrames - accounted);

        this.requestedFrames += frames;
        this.workerPort.postMessage({
            type: "need",
            frames
        });
    }

    reportClockIfNeeded(force = false) {
        if (!this.workerPort)
            return;

        if (!force &&
            this.playedFrames - this.lastClockReportFrame < 1024)
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

    process(inputs, outputs) {
        const output = outputs[0];

        if (!output || output.length === 0)
            return true;

        const left = output[0];
        const right = output.length > 1 ? output[1] : output[0];
        const frames = left.length;

        left.fill(0.0);
        if (right !== left)
            right.fill(0.0);

        if (!this.playing) {
            this.requestAudioIfNeeded();
            return true;
        }

        let written = 0;

        while (written < frames && this.queue.length > 0) {
            const chunk = this.queue[0];
            const available = chunk.frames - this.queueOffsetFrames;
            const count = Math.min(frames - written, available);
            let source = this.queueOffsetFrames * 2;

            for (let i = 0; i < count; ++i) {
                left[written + i] = chunk.pcm[source++];
                right[written + i] = chunk.pcm[source++];
            }

            written += count;
            this.queueOffsetFrames += count;
            this.queuedFrames -= count;

            if (this.queueOffsetFrames >= chunk.frames) {
                const finished = this.queue.shift();
                this.queueOffsetFrames = 0;
                this.recycleChunk(finished);
            }
        }

        const starvedNow = written < frames;
        if (starvedNow)
            ++this.underruns;

        // Song time advances only for PCM actually consumed. If synthesis ever
        // underruns, the visual clock pauses with the music instead of racing
        // ahead while the device emits silence and then playing delayed audio.
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
