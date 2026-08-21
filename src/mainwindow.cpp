#include "mainwindow.hpp"

#include "midi/midi_document_codec.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTimer>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

MainWindow* g_browserMainWindow = nullptr;

float colorHue(const QColor& color)
{
    const qreal hue = color.hslHueF();
    return hue < 0.0
        ? 230.0f
        : static_cast<float>(hue * 360.0);
}

} // namespace

#ifdef __EMSCRIPTEN__

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_file_selected(
    const unsigned char* data,
    int dataSize,
    const char* fileName,
    int fileNameSize)
{
    if (!g_browserMainWindow ||
        !data ||
        dataSize <= 0) {
        return;
    }

    const QString name =
        fileName && fileNameSize > 0
            ? QString::fromUtf8(
                fileName,
                fileNameSize)
            : QStringLiteral("browser.mid");

    g_browserMainWindow->loadMidiRaw(
        data,
        static_cast<std::size_t>(dataSize),
        name);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_loading_progress(
    int percent,
    const char* stage,
    int stageSize)
{
    if (!g_browserMainWindow)
        return;

    const QString text =
        stage && stageSize > 0
            ? QString::fromUtf8(stage, stageSize)
            : QStringLiteral("Loading MIDI");

    g_browserMainWindow->setMidiLoadingProgress(
        percent,
        text);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_loading_failed(
    const char* message,
    int messageSize)
{
    if (!g_browserMainWindow)
        return;

    g_browserMainWindow->failMidiLoading(
        message && messageSize > 0
            ? QString::fromUtf8(message, messageSize)
            : QStringLiteral("Could not parse MIDI"));
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_parsed_midi_selected(
    const unsigned char* data,
    double dataSize,
    const char* fileName,
    int fileNameSize)
{
    if (!g_browserMainWindow ||
        !data ||
        !std::isfinite(dataSize) ||
        dataSize <= 0.0 ||
        dataSize > static_cast<double>(
            std::numeric_limits<std::size_t>::max())) {
        return;
    }

    const QString name =
        fileName && fileNameSize > 0
            ? QString::fromUtf8(fileName, fileNameSize)
            : QStringLiteral("browser.mid");

    g_browserMainWindow->loadMidiSerializedRaw(
        data,
        static_cast<std::size_t>(dataSize),
        name);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_visual_key_page_ready(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t wordCount)
{
    if (!g_browserMainWindow)
        return;

    g_browserMainWindow->receiveKeyboardVisualPage(
        generation,
        spanTicks,
        pageIndex,
        words,
        wordCount);
}

// Browser MIDI loading is delegated to a cache-busted Worker. The Worker keeps
// the File outside WebAssembly and lets the parser read bounded windows, so no
// allocation proportional to the raw MIDI file size occurs on the Qt thread.
EM_JS(void, wasmidi_browser_open_file_picker, (int kind), {
    /*
     * One single native browser file picker for BOTH MIDI and SF2.
     *
     * kind == 0 -> MIDI
     * kind == 1 -> SoundFont 2
     *
     * This is deliberately the exact same DOM/user-gesture path for both
     * formats. The only differences are the accept filter and what happens
     * after a File has already been selected.
     */
    const isSf2 =
        (kind | 0) === 1;

    console.debug(
        '[WASMIDI picker] opening',
        isSf2 ? 'SF2' : 'MIDI');

    const input =
        document.createElement(
            'input');

    input.type = 'file';

    input.accept =
        isSf2
            ? '.sf2'
            : '.mid,.midi,audio/midi,audio/x-midi';

    input.style.position = 'fixed';
    input.style.left = '-10000px';
    input.style.top = '-10000px';
    input.style.width = '1px';
    input.style.height = '1px';
    input.style.opacity = '0';

    document.body.appendChild(
        input);

    const cleanup = () => {
        try {
            input.onchange = null;

            if (input.parentNode)
                input.parentNode.removeChild(
                    input);
        } catch (_) {}
    };

    input.onchange = async () => {
        const file =
            input.files &&
            input.files.length
                ? input.files[0]
                : null;

        if (!file) {
            cleanup();
            return;
        }

        if (isSf2) {
            try {
                let bridge =
                    globalThis.WasmidiSnappyBridge;

                if (!bridge) {
                    await new Promise(
                        (resolve, reject) => {
                            const script =
                                document.createElement(
                                    'script');

                            script.src =
                                './snappysynth_bridge.js';

                            script.async = false;
                            script.onload = resolve;
                            script.onerror = reject;

                            document.head.appendChild(
                                script);
                        });

                    bridge =
                        globalThis.WasmidiSnappyBridge;
                }

                if (!bridge ||
                    typeof bridge.loadSoundfontFile !==
                        'function') {
                    throw new Error(
                        'SnappySynthV2 bridge is unavailable.');
                }

                await bridge.loadSoundfontFile(
                    file);
            } catch (error) {
                console.error(
                    '[WASMIDI] Could not load selected SF2:',
                    error);
            } finally {
                cleanup();
            }

            return;
        }

        const callWithUtf8 =
            (callback, first, text) => {
                const bytes =
                    new TextEncoder().encode(
                        String(text || ''));

                const ptr =
                    _malloc(
                        Math.max(1, bytes.length));

                if (!ptr)
                    return;

                if (bytes.length)
                    HEAPU8.set(bytes, ptr);

                try {
                    callback(first, ptr, bytes.length);
                } finally {
                    _free(ptr);
                }
            };

        const setProgress =
            (percent, stage) => {
                callWithUtf8(
                    _wasmidi_browser_loading_progress,
                    Math.max(0, Math.min(100, percent | 0)),
                    stage || 'Loading MIDI');
            };

        let worker = null;
        let parsedInstallPtr = 0;
        let parsedInstallSize = 0;
        let parsedInstallOffset = 0;
        let parsedInstallName = '';

        const releaseParsedInstall = () => {
            if (parsedInstallPtr) {
                try { _free(parsedInstallPtr); } catch (_) {}
            }
            parsedInstallPtr = 0;
            parsedInstallSize = 0;
            parsedInstallOffset = 0;
            parsedInstallName = '';
        };

        const failLoading =
            (text) => {
                releaseParsedInstall();
                const bytes =
                    new TextEncoder().encode(
                        String(text || 'Could not parse MIDI'));
                const ptr = _malloc(Math.max(1, bytes.length));
                if (!ptr) {
                    _wasmidi_browser_loading_failed(0, 0);
                    return;
                }
                if (bytes.length)
                    HEAPU8.set(bytes, ptr);
                try {
                    _wasmidi_browser_loading_failed(ptr, bytes.length);
                } finally {
                    _free(ptr);
                }
            };

        try {
            if (typeof Worker !== 'function') {
                throw new Error(
                    'This browser does not support the MIDI parser Worker.');
            }

            setProgress(0, 'Starting MIDI loader');

            // Fetch the Worker source explicitly with cache:no-store instead of
            // trusting the browser/Pages HTTP cache for a Worker constructor.
            // This prevents a successful 12.9 deployment from silently running
            // the old 12.7 "allocate file.size" loader.
            const parserWorkerUrl =
                new URL('./midi-parser-worker.js?v=12.9', window.location.href);
            const parserWorkerResponse =
                await fetch(parserWorkerUrl.href, { cache: 'no-store' });
            if (!parserWorkerResponse.ok) {
                throw new Error(
                    'Could not fetch current MIDI parser Worker (' +
                    parserWorkerResponse.status + ').');
            }

            const parserWorkerSource = await parserWorkerResponse.text();
            if (!parserWorkerSource.includes(
                    'WASMIDI_MIDI_PARSER_BOOTSTRAP = "12.9"')) {
                throw new Error(
                    'GitHub Pages returned a stale MIDI parser Worker. ' +
                    'Expected bootstrap 12.9.');
            }

            const parserBaseUrl =
                new URL('./', window.location.href).href;
            const parserWorkerPrelude =
                'self.__wasmidiMidiParserBaseUrl = ' +
                JSON.stringify(parserBaseUrl) + ';\n';
            const parserWorkerBlobUrl = URL.createObjectURL(
                new Blob(
                    [parserWorkerPrelude, parserWorkerSource],
                    { type: 'text/javascript' }));

            try {
                worker = new Worker(parserWorkerBlobUrl);
            } finally {
                URL.revokeObjectURL(parserWorkerBlobUrl);
            }

            worker.onmessage =
                (event) => {
                    const message =
                        event.data || {};

                    if (message.type === 'worker-ready') {
                        console.info(
                            '[WASMIDI MIDI parser] worker bootstrap',
                            String(message.bootstrap || '?'),
                            message.pagedSource === true ? 'paged-source' : 'legacy-source');
                        if (String(message.bootstrap || '') !== '12.9' ||
                            message.pagedSource !== true) {
                            failLoading(
                                'Stale or incompatible MIDI parser Worker loaded. ' +
                                'Expected paged-source bootstrap 12.9.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                        }
                        return;
                    }

                    if (message.type === 'progress') {
                        setProgress(
                            Number(message.percent) || 0,
                            message.stage || 'Loading MIDI');
                        return;
                    }

                    if (message.type === 'error') {
                        failLoading(
                            message.message || 'Could not parse MIDI');

                        if (worker) {
                            worker.terminate();
                            worker = null;
                        }
                        cleanup();
                        return;
                    }

                    if (message.type === 'result-begin') {
                        releaseParsedInstall();

                        const total = Number(message.size);
                        if (!Number.isSafeInteger(total) || total <= 0) {
                            failLoading('Parser returned an invalid document size.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        // Qt itself is still wasm32. Give it the full 4 GiB
                        // address space instead of the historical 2 GiB default,
                        // but never allow JS -> i32 truncation for a >4 GiB wire
                        // image. The Memory64 parser can be much larger because
                        // it no longer shares this heap.
                        if (total > 0xffffffff) {
                            failLoading(
                                'Parsed MIDI document is larger than the 4 GiB Qt wasm32 address space. ' +
                                'The Memory64 parser accepted it, but this file requires segmented player residency.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        parsedInstallPtr =
                            _malloc(Math.max(1, total)) >>> 0;
                        if (!parsedInstallPtr) {
                            failLoading(
                                'The browser/OS could not commit enough memory to install the parsed MIDI in the player.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        parsedInstallSize = total;
                        parsedInstallOffset = 0;
                        parsedInstallName =
                            String(message.name || file.name || 'browser.mid');
                        setProgress(95, 'Streaming parsed MIDI into player memory');
                        return;
                    }

                    if (message.type === 'result-chunk') {
                        if (!parsedInstallPtr ||
                            !(message.data instanceof ArrayBuffer)) {
                            failLoading('Unexpected parsed-MIDI chunk.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        const offset = Number(message.offset);
                        const bytes = new Uint8Array(message.data);
                        if (!Number.isSafeInteger(offset) ||
                            offset !== parsedInstallOffset ||
                            offset + bytes.length > parsedInstallSize) {
                            failLoading('Parsed-MIDI transfer became out of order.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        HEAPU8.set(bytes, parsedInstallPtr + offset);
                        parsedInstallOffset += bytes.length;
                        return;
                    }

                    if (message.type !== 'result-end')
                        return;

                    if (!parsedInstallPtr ||
                        parsedInstallOffset !== parsedInstallSize ||
                        Number(message.size) !== parsedInstallSize) {
                        failLoading('Parsed-MIDI transfer ended before all bytes arrived.');
                        if (worker) worker.terminate();
                        worker = null;
                        cleanup();
                        return;
                    }

                    const nameBytes =
                        new TextEncoder().encode(
                            parsedInstallName || file.name || 'browser.mid');
                    const namePtr =
                        _malloc(Math.max(1, nameBytes.length));

                    if (!namePtr) {
                        failLoading('Could not allocate MIDI filename memory.');
                        if (worker) worker.terminate();
                        worker = null;
                        cleanup();
                        return;
                    }

                    if (nameBytes.length)
                        HEAPU8.set(nameBytes, namePtr);

                    setProgress(96, 'Installing parsed MIDI');

                    try {
                        _wasmidi_browser_parsed_midi_selected(
                            parsedInstallPtr,
                            parsedInstallSize,
                            namePtr,
                            nameBytes.length);
                    } finally {
                        _free(namePtr);
                        releaseParsedInstall();
                        if (worker) worker.terminate();
                        worker = null;
                        cleanup();
                    }
                };

            worker.onerror =
                (event) => {
                    failLoading(
                        event && event.message
                            ? event.message
                            : 'MIDI parser Worker failed.');
                    if (worker) worker.terminate();
                    worker = null;
                    cleanup();
                };

            worker.postMessage({
                type: 'parse',
                file,
                name: file.name || 'browser.mid'
            });
        } catch (error) {
            failLoading(
                error && error.message
                    ? error.message
                    : String(error));

            if (worker)
                worker.terminate();

            cleanup();
        }
    };

    /*
     * IMPORTANT: this is synchronous and happens before AudioContext,
     * Worker creation, promises, bridge loading, or file processing.
     */
    input.click();
});


EM_JS(int, wasmidi_snappy_ready, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state && b.state.ready ? 1 : 0;
});

EM_JS(int, wasmidi_snappy_soundfont_loaded, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state && b.state.soundfontLoaded ? 1 : 0;
});

EM_JS(int, wasmidi_snappy_sample_rate, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.sampleRate) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_active_voices, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.activeVoices) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_free_voices, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.freeVoices) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_steals, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.steals) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_layers, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.layers) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_regions, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.regions) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_worker_count, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.workerCount) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_underruns, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.underruns) | 0) : 0;
});

EM_JS(double, wasmidi_snappy_audio_clock, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.getAudioClock ? Number(b.getAudioClock()) : -1.0;
});

EM_JS(int, wasmidi_snappy_starved, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state && b.state.starved ? 1 : 0;
});

EM_JS(void, wasmidi_snappy_sync_visual_clock, (double time), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.syncVisualClock)
        b.syncVisualClock(Number(time) || 0.0);
});

EM_JS(int, wasmidi_snappy_copy_string, (int which, char* dst, int capacity), {
    if (!dst || capacity <= 0)
        return 0;
    const b = globalThis.WasmidiSnappyBridge;
    let text = '';
    if (b && b.state) {
        text = which === 0
            ? String(b.state.soundfontName || '')
            : String(b.state.status || '');
    }
    stringToUTF8(text, dst, capacity);
    return lengthBytesUTF8(text);
});



EM_JS(void, wasmidi_snappy_clear_soundfonts, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.clearSoundfonts)
        b.clearSoundfonts();
});

EM_JS(void, wasmidi_snappy_schedule_sysex,
      (const uint8_t* data, int length, double time), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.scheduleSysEx || !data || length <= 0)
        return;
    const bytes = new Uint8Array(length);
    bytes.set(HEAPU8.subarray(data, data + length));
    b.scheduleSysEx(bytes, Number(time) || 0.0);
});

EM_JS(void, wasmidi_snappy_play, (double time, int reset), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.play)
        b.play(time, !!reset);
});

EM_JS(void, wasmidi_snappy_pause, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.pause)
        b.pause();
});

EM_JS(void, wasmidi_snappy_stop, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.stop)
        b.stop();
});

EM_JS(void, wasmidi_snappy_seek, (double time), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.seek)
        b.seek(time);
});

EM_JS(void, wasmidi_snappy_set_prebuffer, (double seconds, double duration), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setPrebuffer)
        b.setPrebuffer(Number(seconds) || 0.0, Number(duration) || 0.0);
});

EM_JS(void, wasmidi_snappy_set_volume, (int percent), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setVolume)
        b.setVolume(percent);
});

EM_JS(void, wasmidi_snappy_configure,
      (int maxVoices,
       int minVoices,
       int blockFrames,
       int numBuffers,
       int requestedSampleRate,
       int channels,
       int bitsPerSample,
       int realtimePriority,
       int workers,
       int noteSharding,
       int stealScoreCache,
       int fastNoteOff,
       int validateState,
       int softClip), {
    const b = globalThis.WasmidiSnappyBridge;

    if (!b || !b.configure)
        return;

    b.configure({
        maxVoices,
        minVoices,
        blockFrames,
        numBuffers,
        requestedSampleRate,
        channels,
        bitsPerSample,
        realtimePriority: !!realtimePriority,
        workers,
        noteSharding,
        stealScoreCache: !!stealScoreCache,
        fastNoteOff: !!fastNoteOff,
        validateState: !!validateState,
        softClip: !!softClip
    });
});

EM_JS(void, wasmidi_snappy_set_overlap_gain, (int enabled), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setOverlapGain)
        b.setOverlapGain(!!enabled);
});

EM_JS(void, wasmidi_snappy_schedule,
      (const uint32_t* messages, const double* times, int count, double safeUntil), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.schedule)
        return;

    const n = Math.max(0, count | 0);
    const m = new Uint32Array(n);
    const t = new Float64Array(n);

    if (n > 0) {
        m.set(HEAPU32.subarray(messages >>> 2, (messages >>> 2) + n));
        t.set(HEAPF64.subarray(times >>> 3, (times >>> 3) + n));
    }

    b.schedule(m, t, Number(safeUntil) || 0.0);
});

#endif

MainWindow::MainWindow(QObject* parent)
    : QObject(parent)
{
    g_browserMainWindow = this;

    static const QColor defaults[16] = {
        QColor("#818cf8"), QColor("#a78bfa"), QColor("#c4b5fd"), QColor("#fb923c"),
        QColor("#4ade80"), QColor("#38bdf8"), QColor("#f472b6"), QColor("#facc15"),
        QColor("#f87171"), QColor("#34d399"), QColor("#60a5fa"), QColor("#e879f9"),
        QColor("#fb7185"), QColor("#a3e635"), QColor("#22d3ee"), QColor("#fbbf24")
    };

    channelColors_.reserve(16);

    for (const auto& color : defaults)
        channelColors_.push_back(color);

    visualPitchColor_.fill(-1);

    auto* frameTimer = new QTimer(this);
    frameTimer->setTimerType(Qt::PreciseTimer);

    connect(
        frameTimer,
        &QTimer::timeout,
        this,
        &MainWindow::updateCurrentTime);

    frameTimer->start(16);

    // SnappySynth is scheduled in coarse 20 ms batches. dispatchScheduler()
    // walks CompactEvent directly and transfers only the new ~400 ms look-ahead
    // delta to the synth Worker; there is no browser MIDI-output path.
    auto* synthScheduleTimer = new QTimer(this);
    synthScheduleTimer->setTimerType(Qt::PreciseTimer);
    connect(synthScheduleTimer, &QTimer::timeout,
            this, &MainWindow::dispatchScheduler);
    synthScheduleTimer->start(20);

    auto* synthStateTimer = new QTimer(this);
    connect(synthStateTimer, &QTimer::timeout,
            this, &MainWindow::pollSynthState);
    synthStateTimer->start(200);

#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_volume(volume_);
    publishSynthPrebufferConfig();
#endif
}

MainWindow::~MainWindow()
{
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_stop();
#endif
    if (g_browserMainWindow == this)
        g_browserMainWindow = nullptr;
}

QVariantList MainWindow::channelColorList() const
{
    QVariantList result;
    result.reserve(channelColors_.size());

    for (const auto& color : channelColors_)
        result.push_back(color);

    return result;
}

void MainWindow::setPlaying(bool playing)
{
    if (isPlaying_ == playing)
        return;

    isPlaying_ = playing;
    emit playingChanged();
}

bool MainWindow::loadMidiUrl(const QUrl& url)
{
    const QString localPath =
        url.isLocalFile()
            ? url.toLocalFile()
            : url.toString();

    QFile file(localPath);

    if (!file.open(QIODevice::ReadOnly)) {
        emit loadFailed(
            QStringLiteral("Could not open %1")
                .arg(localPath));
        return false;
    }

    return loadMidiFileNamed(
        file.readAll(),
        QFileInfo(localPath).fileName());
}

bool MainWindow::loadMidiFile(const QByteArray& data)
{
    return loadMidiRaw(
        reinterpret_cast<const uint8_t*>(
            data.constData()),
        static_cast<std::size_t>(
            data.size()),
        fileName_);
}

bool MainWindow::loadMidiFileNamed(
    const QByteArray& data,
    const QString& fileName)
{
    return loadMidiRaw(
        reinterpret_cast<const uint8_t*>(
            data.constData()),
        static_cast<std::size_t>(
            data.size()),
        fileName);
}

bool MainWindow::adoptParsedDocument(
    wasmidi::MidiDocument&& parsed,
    const QString& fileName)
{
    clearKeyboardVisualPageCache();
    document_ = std::move(parsed);
    scheduler_.setDocument(&document_);

    if (!fileName.isEmpty()) {
        fileName_ = fileName;
        emit fileNameChanged();
    }

    publishDocumentMetadata();
    rebuildDerivedStats();

    invalidateLiveTrackers();
    visualStateValid_ = false;

    ++documentRevision_;
    emit documentRevisionChanged();

    publishSynthPrebufferConfig();
    updateLiveStats();
    emit fileLoaded();

    return true;
}

bool MainWindow::loadMidiRaw(
    const uint8_t* data,
    std::size_t size,
    const QString& fileName)
{
    stop();

    wasmidi::MidiDocument parsed;

    if (!parser_.parse(
            data,
            size,
            parsed)) {
        emit loadFailed(
            QString::fromUtf8(
                parser_.error()));
        return false;
    }

    return adoptParsedDocument(
        std::move(parsed),
        fileName);
}

void MainWindow::setMidiLoadingProgress(
    int progress,
    const QString& stage)
{
    const int clamped =
        std::clamp(progress, 0, 100);

    const bool starting =
        !midiLoading_ && clamped < 100;

    if (starting)
        stop();

    const bool nextLoading =
        clamped < 100;

    const bool changed =
        midiLoading_ != nextLoading ||
        midiLoadingProgress_ != clamped ||
        midiLoadingStage_ != stage;

    midiLoading_ = nextLoading;
    midiLoadingProgress_ = clamped;
    midiLoadingStage_ = stage;

    if (changed)
        emit midiLoadingChanged();
}

void MainWindow::failMidiLoading(
    const QString& message)
{
    midiLoading_ = false;
    midiLoadingProgress_ = 0;
    midiLoadingStage_.clear();
    emit midiLoadingChanged();
    emit loadFailed(message);
}

bool MainWindow::loadMidiSerializedRaw(
    const uint8_t* data,
    std::size_t size,
    const QString& fileName)
{
    stop();
    setMidiLoadingProgress(
        97,
        QStringLiteral("Installing MIDI document"));

    wasmidi::MidiDocument parsed;
    std::string error;

    if (!wasmidi::deserializeMidiDocument(
            data,
            size,
            parsed,
            error)) {
        failMidiLoading(
            QString::fromStdString(error));
        return false;
    }

    // Install the large compact arrays immediately, then yield once before the
    // derived-stat pass. That yield lets Qt paint the 98% loading state instead
    // of making the final main-thread work look like another browser freeze.
    clearKeyboardVisualPageCache();
    document_ = std::move(parsed);
    scheduler_.setDocument(&document_);

    if (!fileName.isEmpty()) {
        fileName_ = fileName;
        emit fileNameChanged();
    }

    publishDocumentMetadata();
    setMidiLoadingProgress(
        98,
        QStringLiteral("Building playback statistics"));

    QTimer::singleShot(
        0,
        this,
        [this]() {
            rebuildDerivedStats();

            invalidateLiveTrackers();
            visualStateValid_ = false;

            setMidiLoadingProgress(
                99,
                QStringLiteral("Preparing visualizers"));

            ++documentRevision_;
            emit documentRevisionChanged();

            publishSynthPrebufferConfig();
            updateLiveStats();
            emit fileLoaded();

            setMidiLoadingProgress(
                100,
                QStringLiteral("Ready"));
        });

    return true;
}

void MainWindow::openMidiPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_open_file_picker(0);
#else
    emit loadFailed(
        QStringLiteral(
            "The browser MIDI picker is available in the WebAssembly build."));
#endif
}

void MainWindow::openSoundfontPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_open_file_picker(1);
#else
    synthStatus_ = QStringLiteral("SnappySynthV2 SF2 picker is available in the WebAssembly build.");
    emit synthStateChanged();
#endif
}


void MainWindow::clearSoundfonts()
{
#ifdef __EMSCRIPTEN__
    if (isPlaying_)
        stop();
    wasmidi_snappy_clear_soundfonts();
#endif
    soundfontLoaded_ = false;
    soundfontName_.clear();
    synthLayers_ = 0;
    synthRegions_ = 0;
    synthPlaybackPrimed_ = false;
    synthWasSoundfontLoaded_ = false;
    emit synthStateChanged();
}


void MainWindow::clearVisualState()
{
    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);

    visualActiveVoices_ = 0;
    visualTick_ = 0.0;
    visualKeyStartCursor_ = 0;
    visualKeyEndCursor_ = 0;
    visualKeyOwnerCursor_ = 0;
    visualStateValid_ = false;
}

void MainWindow::clearKeyboardVisualPageCache()
{
    keyboardVisualPages_.clear();
    keyboardVisualGeneration_ = 0;
}

void MainWindow::receiveKeyboardVisualPage(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t wordCount)
{
    constexpr uint32_t HeaderWords = 3;
    constexpr uint32_t CountWords = 128u * 16u;
    constexpr uint32_t OwnerWords = 128u;
    constexpr uint32_t ExpectedWords =
        HeaderWords + CountWords * 2u + OwnerWords;

    if (!words || spanTicks == 0 || wordCount != ExpectedWords)
        return;

    if (keyboardVisualGeneration_ != generation) {
        keyboardVisualPages_.clear();
        keyboardVisualGeneration_ = generation;
    }

    KeyboardVisualPage page;
    page.generation = generation;
    page.spanTicks = spanTicks;
    page.pageIndex = pageIndex;
    page.startCursor = words[0];
    page.endCursor = words[1];
    page.ownerCursor = words[2];

    uint32_t offset = HeaderWords;
    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        for (std::size_t color = 0; color < 16; ++color)
            page.globalCounts[pitch][color] = words[offset++];
    }
    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        for (std::size_t color = 0; color < 16; ++color)
            page.trackCounts[pitch][color] = words[offset++];
    }
    for (std::size_t pitch = 0; pitch < 128; ++pitch)
        page.ownerColors[pitch] = words[offset++];

    auto existing = std::find_if(
        keyboardVisualPages_.begin(),
        keyboardVisualPages_.end(),
        [generation, spanTicks, pageIndex](const KeyboardVisualPage& item) {
            return item.generation == generation &&
                   item.spanTicks == spanTicks &&
                   item.pageIndex == pageIndex;
        });

    if (existing != keyboardVisualPages_.end()) {
        *existing = std::move(page);
        return;
    }

    if (keyboardVisualPages_.size() >= 64) {
        // Keep the 64 snapshots nearest the newest page. This mirrors the roll
        // worker's rolling page cache without tying keyboard correctness to it:
        // a missing page simply falls back to the parser's exact seek index.
        auto victim = keyboardVisualPages_.begin();
        uint64_t farthest = 0;
        const uint64_t incomingTick = page.startTick();
        for (auto it = keyboardVisualPages_.begin();
             it != keyboardVisualPages_.end();
             ++it) {
            const uint64_t tick = it->startTick();
            const uint64_t distance =
                tick > incomingTick ? tick - incomingTick : incomingTick - tick;
            if (it == keyboardVisualPages_.begin() || distance > farthest) {
                victim = it;
                farthest = distance;
            }
        }
        keyboardVisualPages_.erase(victim);
    }

    keyboardVisualPages_.push_back(std::move(page));
}

void MainWindow::invalidateLiveTrackers()
{
    liveWindowsValid_ = false;
    liveLastTime_ = 0.0f;
    liveHi_ = 0;
    liveNpsLo_ = 0;
    liveCcLo_ = 0;
    liveNpsCount_ = 0;
    liveCcCount_ = 0;

    neuralWindowValid_ = false;
    neuralFutureLo_ = 0;
    neuralFutureHi_ = 0;
    neuralFutureColors_.fill(0);
}

void MainWindow::clearFile()
{
    stop();

    scheduler_.setDocument(nullptr);
    clearKeyboardVisualPageCache();
    document_ = wasmidi::MidiDocument{};

    fileName_.clear();

    duration_ = 0.0f;
    noteCount_ = 0;
    trackCount_ = 0;
    activeVoices_ = 0;
    activeChannelCount_ = 0;
    nps_ = 0;
    ccPerSecond_ = 0;
    bpm_ = 120.0f;
    midiFormat_ = 0;
    ppq_ = 480;
    tempoChangeCount_ = 0;
    controlEventCount_ = 0;
    peakNps_ = 0;
    peakNpsTime_ = 0.0f;
    peakPolyphony_ = 0;
    pitchRange_ = QStringLiteral("—");
    synthEffectiveVelocityFloor_ = synthVelocityFloor_;
    skippedVelocity_ = synthEffectiveVelocityFloor_;

    npsTimeline_.clear();
    tempoTimes_.clear();
    tempoBpms_.clear();

    clearVisualState();
    invalidateLiveTrackers();

    dominantHue_ = 230.0f;
    neuralActivity_ = 0.0f;

    ++documentRevision_;

    emit fileNameChanged();
    emit durationChanged();
    emit noteCountChanged();
    emit trackCountChanged();
    emit activeVoicesChanged();
    emit activeChannelCountChanged();
    emit npsChanged();
    emit ccPerSecondChanged();
    emit bpmChanged();
    emit midiFormatChanged();
    emit ppqChanged();
    emit tempoChangeCountChanged();
    emit controlEventCountChanged();
    emit peakNpsChanged();
    emit peakPolyphonyChanged();
    emit pitchRangeChanged();
    emit skippedVelocityChanged();
    emit timelineChanged();
    emit activePitchesChanged();
    emit neuralVisualChanged();
    emit documentRevisionChanged();
}

void MainWindow::publishDocumentMetadata()
{
    duration_ =
        document_.durationSeconds;

    noteCount_ =
        static_cast<int>(
            std::min<uint64_t>(
                document_.noteCount,
                uint64_t(
                    std::numeric_limits<int>::max())));

    trackCount_ =
        document_.trackCount;

    midiFormat_ =
        document_.format;

    ppq_ =
        document_.ticksPerBeat;

    tempoChangeCount_ =
        static_cast<int>(
            std::min<std::size_t>(
                document_.tempoMap.size(),
                std::size_t(
                    std::numeric_limits<int>::max())));

    controlEventCount_ =
        static_cast<int>(
            std::min<uint64_t>(
                document_.controlEventCount,
                uint64_t(
                    std::numeric_limits<int>::max())));

    uint32_t channelMask = 0;

    for (uint32_t mask :
         document_.activeChannelMasks) {
        channelMask |= mask;
    }

    activeChannelCount_ = 0;

    for (int channel = 0;
         channel < 16;
         ++channel) {
        activeChannelCount_ +=
            (channelMask & (1u << channel))
                ? 1
                : 0;
    }

    if (document_.hasPitch) {
        pitchRange_ =
            QStringLiteral("%1–%2")
                .arg(document_.minPitch)
                .arg(document_.maxPitch);
    } else {
        pitchRange_ =
            QStringLiteral("—");
    }

    emit durationChanged();
    emit noteCountChanged();
    emit trackCountChanged();
    emit midiFormatChanged();
    emit ppqChanged();
    emit tempoChangeCountChanged();
    emit controlEventCountChanged();
    emit activeChannelCountChanged();
    emit pitchRangeChanged();
}

void MainWindow::rebuildDerivedStats()
{
    tempoTimes_.clear();
    tempoBpms_.clear();
    npsTimeline_.clear();

    peakNps_ = 0;
    peakNpsTime_ = 0.0f;
    peakPolyphony_ = 0;

    tempoTimes_.reserve(
        document_.tempoMap.size());

    tempoBpms_.reserve(
        document_.tempoMap.size());

    for (std::size_t i = 0;
         i < document_.tempoMap.size();
         ++i) {
        tempoTimes_.push_back(
            static_cast<float>(
                i < document_.tempoSeconds.size()
                    ? document_.tempoSeconds[i]
                    : document_.tickToSeconds(
                        document_.tempoMap[i].tick)));

        const uint32_t us =
            document_.tempoMap[i]
                .microsecondsPerBeat;

        tempoBpms_.push_back(
            us != 0
                ? 60'000'000.0f /
                  float(us)
                : 120.0f);
    }

    if (tempoBpms_.empty()) {
        tempoTimes_.push_back(0.0f);
        tempoBpms_.push_back(120.0f);
    }

    bpm_ = tempoBpms_.front();

    if (document_.derivedStatsReady) {
        peakNps_ = static_cast<int>(
            std::min<uint32_t>(
                document_.derivedPeakNps,
                static_cast<uint32_t>(std::numeric_limits<int>::max())));
        peakNpsTime_ = document_.derivedPeakNpsTime;
        peakPolyphony_ = static_cast<int>(
            std::min<uint32_t>(
                document_.derivedPeakPolyphony,
                static_cast<uint32_t>(std::numeric_limits<int>::max())));

        npsTimeline_.reserve(
            static_cast<int>(
                std::min<std::size_t>(
                    document_.derivedNpsTimeline.size(),
                    std::size_t(std::numeric_limits<int>::max()))));
        for (uint32_t value : document_.derivedNpsTimeline) {
            npsTimeline_.push_back(
                static_cast<int>(
                    std::min<uint32_t>(
                        value,
                        static_cast<uint32_t>(std::numeric_limits<int>::max()))));
        }

        emit bpmChanged();
        emit peakNpsChanged();
        emit peakPolyphonyChanged();
        emit timelineChanged();
        return;
    }

    // Fallback for hand-constructed/legacy in-process documents. Browser-loaded
    // MIDI files arrive with these expensive statistics already built in the
    // parser Worker, so Qt never performs this sort on the UI thread.
    // Half-second NPS timeline: same visual data as before, now accumulated
    // directly from TickGroup::noteOnCount instead of per-note start arrays.
    if (duration_ > 0.0f) {
        constexpr double step = 0.5;

        const std::size_t bucketCount =
            static_cast<std::size_t>(
                std::ceil(
                    double(duration_) /
                    step)) + 1;

        std::vector<int> buckets(
            bucketCount,
            0);

        for (const auto& group :
             document_.tickGroups) {
            if (group.noteOnCount == 0)
                continue;

            const double seconds =
                document_.tickToSeconds(
                    group.tick);

            const std::size_t bucket =
                std::min(
                    bucketCount - 1,
                    static_cast<std::size_t>(
                        std::max(
                            0.0,
                            std::floor(
                                seconds /
                                step))));

            const uint64_t sum =
                uint64_t(buckets[bucket]) +
                group.noteOnCount;

            buckets[bucket] =
                static_cast<int>(
                    std::min<uint64_t>(
                        sum,
                        uint64_t(
                            std::numeric_limits<int>::max())));
        }

        npsTimeline_.reserve(
            static_cast<int>(
                std::min<std::size_t>(
                    bucketCount,
                    std::size_t(
                        std::numeric_limits<int>::max()))));

        for (int value : buckets)
            npsTimeline_.push_back(value);
    }

    // MPWGL2 peak NPS samples every 0.1 s and counts starts in [t-1,t].
    // Use the exact same cadence/bounds over the immutable visual note stream.
    for (double t = 0.0;
         t <= double(duration_) + 0.000001;
         t += 0.1) {
        const double hiTick =
            document_.secondsToTick(t);

        const double loTick =
            document_.secondsToTick(
                std::max(
                    0.0,
                    t - 1.0));

        const std::size_t lo =
            document_.lowerBoundVisualStart(
                loTick);

        const std::size_t hi =
            document_.upperBoundVisualStart(
                hiTick);

        const uint64_t count =
            hi >= lo
                ? uint64_t(hi - lo)
                : 0u;

        if (count >
            static_cast<uint64_t>(
                peakNps_)) {
            peakNps_ =
                static_cast<int>(
                    std::min<uint64_t>(
                        count,
                        uint64_t(
                            std::numeric_limits<int>::max())));

            peakNpsTime_ =
                static_cast<float>(t);
        }
    }

    // MPWGL2 samples very large files for peak-poly calculation.
    // Preserve that behavior, but use the independent VisualNote pairs so
    // repeated/overlapping same-key notes are not merged.
    struct PolyEdge {
        uint32_t tick = 0;
        int8_t direction = 0;
    };

    constexpr std::size_t MaxPeakNotes =
        400000;

    const std::size_t visualCount =
        document_.visualNotes.size();

    const std::size_t stride =
        visualCount > MaxPeakNotes
            ? static_cast<std::size_t>(
                std::ceil(
                    double(visualCount) /
                    double(MaxPeakNotes)))
            : 1;

    std::vector<PolyEdge> edges;

    edges.reserve(
        (visualCount /
         std::max<std::size_t>(
             1,
             stride) + 1) *
        2);

    for (std::size_t i = 0;
         i < visualCount;
         i += stride) {
        const auto& note =
            document_.visualNotes[i];

        edges.push_back({
            note.startTick,
            +1
        });

        edges.push_back({
            note.endTick,
            -1
        });
    }

    std::sort(
        edges.begin(),
        edges.end(),
        [](const PolyEdge& a,
           const PolyEdge& b) {
            if (a.tick != b.tick)
                return a.tick < b.tick;

            // Same as MPWGL2: NoteOns before NoteOffs at equal time.
            return a.direction >
                   b.direction;
        });

    int active = 0;

    for (const auto& edge : edges) {
        active += edge.direction;

        peakPolyphony_ =
            std::max(
                peakPolyphony_,
                active);
    }

    emit bpmChanged();
    emit peakNpsChanged();
    emit peakPolyphonyChanged();
    emit timelineChanged();
}

void MainWindow::notifyVisualizerFramePresented()
{
    ++visualFrameSerial_;
}

void MainWindow::play()
{
    if (!hasMidi())
        return;

    if (currentTime_ >= duration_)
        currentTime_ = 0.0f;

    playbackAnchorSeconds_ = currentTime_;
    playbackClock_.restart();
    playbackLastElapsedMs_ = 0;
    playbackConsumedVisualFrameSerial_ = visualFrameSerial_;
    synthLastHardResyncElapsedMs_ = -100000;
    synthWasStarved_ = false;

    scheduler_.seek(currentTime_);
    scheduler_.start();

    invalidateLiveTrackers();
    visualStateValid_ = false;

    // Mark playback active before priming the synth so scheduleSynthAhead() can
    // immediately fill the first look-ahead window instead of waiting 20 ms.
    setPlaying(true);

#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_) {
        if (!synthPlaybackPrimed_) {
            // Reset the worker first. resetSynthSchedule() sends controller and
            // held-note reconstruction data; doing it in the opposite order
            // would let the subsequent reset erase that batch.
            wasmidi_snappy_play(currentTime_, 1);
            resetSynthSchedule(currentTime_);
            synthPlaybackPrimed_ = true;
            scheduleSynthAhead();
        } else {
            wasmidi_snappy_play(currentTime_, 0);
            scheduleSynthAhead();
        }
    }
    wasmidi_snappy_sync_visual_clock(currentTime_);
#endif
}

void MainWindow::pause()
{
    if (!isPlaying_)
        return;

    updateCurrentTime();
    scheduler_.pause();
#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_)
        wasmidi_snappy_pause();
#endif
    setPlaying(false);
}

void MainWindow::stop()
{
    scheduler_.stop();
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_stop();
#endif
    synthPlaybackPrimed_ = false;
    synthGroupCursor_ = 0;
    synthScheduledUntil_ = 0.0;
    setPlaying(false);

    playbackAnchorSeconds_ = 0.0f;
    playbackLastElapsedMs_ = 0;
    playbackConsumedVisualFrameSerial_ = visualFrameSerial_;
    synthWasStarved_ = false;
    playbackClock_.invalidate();

    if (currentTime_ != 0.0f) {
        currentTime_ = 0.0f;
        emit currentTimeChanged();
    }

    if (activeVoices_ != 0) {
        activeVoices_ = 0;
        emit activeVoicesChanged();
    }

    if (nps_ != 0) {
        nps_ = 0;
        emit npsChanged();
    }

    if (ccPerSecond_ != 0) {
        ccPerSecond_ = 0;
        emit ccPerSecondChanged();
    }

    if (!tempoBpms_.empty() &&
        !qFuzzyCompare(bpm_, tempoBpms_.front())) {
        bpm_ = tempoBpms_.front();
        emit bpmChanged();
    }

    clearVisualState();
    invalidateLiveTrackers();
    emit activePitchesChanged();
}

void MainWindow::seek(float seconds)
{
    const float clamped =
        std::clamp(seconds, 0.0f, duration_);

    currentTime_ = clamped;
    playbackAnchorSeconds_ = clamped;

    if (isPlaying_) {
        playbackClock_.restart();
        playbackLastElapsedMs_ = 0;
        playbackConsumedVisualFrameSerial_ = visualFrameSerial_;
    }

    scheduler_.seek(clamped);
    invalidateLiveTrackers();
    visualStateValid_ = false;

#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_) {
        wasmidi_snappy_seek(clamped);
        resetSynthSchedule(clamped);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
    wasmidi_snappy_sync_visual_clock(currentTime_);
#endif

    emit currentTimeChanged();
    updateLiveStats();
}

void MainWindow::setNoteSpeed(float speed)
{
    const float clamped =
        std::clamp(
            speed,
            0.1f,
            60.0f);

    if (qFuzzyCompare(
            noteSpeed_,
            clamped)) {
        return;
    }

    noteSpeed_ = clamped;
    emit noteSpeedChanged();
}

void MainWindow::setPostBuffer(float buffer)
{
    const float clamped =
        std::clamp(
            buffer,
            0.0f,
            10.0f);

    const bool autoWasEnabled =
        postBufferAuto_;

    postBufferAuto_ = false;

    if (autoWasEnabled)
        emit postBufferAutoChanged();

    if (qFuzzyCompare(
            postBuffer_,
            clamped)) {
        return;
    }

    postBuffer_ = clamped;
    emit postBufferChanged();
}

void MainWindow::setPostBufferAuto()
{
    const bool changed =
        !postBufferAuto_;

    postBufferAuto_ = true;

    if (!qFuzzyIsNull(postBuffer_)) {
        postBuffer_ = 0.0f;
        emit postBufferChanged();
    }

    if (changed)
        emit postBufferAutoChanged();
}

void MainWindow::setPerTrackColors(bool enable)
{
    if (perTrackColors_ == enable)
        return;

    perTrackColors_ = enable;
    visualStateValid_ = false;
    neuralWindowValid_ = false;

    emit perTrackColorsChanged();

    updateLiveStats();
}

void MainWindow::setVolume(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            100);

    if (volume_ == clamped)
        return;

    volume_ = clamped;
    emit volumeChanged();
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_volume(volume_);

    // PCM already sitting in the rolling pre-render ring was generated with
    // the old gain. Audio-setting changes must never leak stale cached audio.
    if (soundfontLoaded_ && isPlaying_) {
        wasmidi_snappy_seek(currentTime_);
        resetSynthSchedule(currentTime_);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
#endif
}


void MainWindow::applySynthConfig()
{
    synthPlaybackPrimed_ = false;

#ifdef __EMSCRIPTEN__
    wasmidi_snappy_configure(
        synthMaxVoices_,
        synthMinVoices_,
        synthBufferFrames_,
        synthNumBuffers_,
        synthRequestedSampleRate_,
        synthChannels_,
        synthBitsPerSample_,
        synthRealtimePriority_ ? 1 : 0,
        synthWorkers_,
        synthNoteSharding_,
        synthStealScoreCache_ ? 1 : 0,
        synthFastNoteOff_ ? 1 : 0,
        synthValidateState_ ? 1 : 0,
        synthSoftClip_ ? 1 : 0);
#endif
}

void MainWindow::setSynthMaxVoices(int value)
{
    // SnappySynth_SetMinimumVoices() treats the minimum as a floor for the
    // effective voice cap. Keep the UI state internally consistent with that
    // source behavior instead of showing Max < Min while the core silently
    // raises Max during initialization.
    const int clamped =
        std::clamp(
            value,
            std::max(1, synthMinVoices_),
            5000000);

    if (synthMaxVoices_ == clamped)
        return;

    synthMaxVoices_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthMinVoices(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            5000000);

    bool changed = false;

    if (synthMinVoices_ != clamped) {
        synthMinVoices_ = clamped;
        changed = true;
    }

    if (synthMaxVoices_ < synthMinVoices_) {
        synthMaxVoices_ = synthMinVoices_;
        changed = true;
    }

    if (!changed)
        return;

    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthBufferFrames(int value)
{
    // The original SnappySynth configuration accepts any positive BufferSize
    // (examples include 48, 96, 256, 480, 512, 1024). Do not force powers of 2.
    const int clamped =
        std::clamp(
            value,
            1,
            65536);

    if (synthBufferFrames_ == clamped)
        return;

    synthBufferFrames_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthNumBuffers(int value)
{
    const int clamped =
        std::clamp(
            value,
            1,
            128);

    if (synthNumBuffers_ == clamped)
        return;

    synthNumBuffers_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthPrebufferSeconds(float value)
{
    // 0 means "up to the remaining/full MIDI duration". Keep a generous
    // explicit upper bound for user-entered values; the worker additionally
    // applies a memory-safe byte cap before allocating the shared PCM ring.
    const float clamped =
        value <= 0.0f
            ? 0.0f
            : std::clamp(value, 0.25f, 3600.0f);

    if (std::abs(synthPrebufferSeconds_ - clamped) < 0.0001f)
        return;

    synthPrebufferSeconds_ = clamped;
    emit synthConfigChanged();
    publishSynthPrebufferConfig();

    // A buffer-size/audio-cache policy change invalidates already rendered PCM
    // exactly like an audio setting change requested by the user.
#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_ && isPlaying_) {
        wasmidi_snappy_seek(currentTime_);
        resetSynthSchedule(currentTime_);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
#endif
}

void MainWindow::setSynthVelocityFloor(int value)
{
    const int clamped = std::clamp(value, 0, 127);
    if (synthVelocityFloor_ == clamped)
        return;

    synthVelocityFloor_ = clamped;
    synthEffectiveVelocityFloor_ =
        std::max(synthEffectiveVelocityFloor_, synthVelocityFloor_);

    if (!isPlaying_)
        synthEffectiveVelocityFloor_ = synthVelocityFloor_;

    if (skippedVelocity_ != synthEffectiveVelocityFloor_) {
        skippedVelocity_ = synthEffectiveVelocityFloor_;
        emit skippedVelocityChanged();
    }

    emit synthConfigChanged();

#ifdef __EMSCRIPTEN__
    // The velocity floor changes the actual MIDI stream sent to the synth, so
    // already rendered PCM is invalid just like after a seek or DSP change.
    if (soundfontLoaded_ && isPlaying_) {
        wasmidi_snappy_seek(currentTime_);
        resetSynthSchedule(currentTime_);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
#endif
}

void MainWindow::publishSynthPrebufferConfig()
{
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_prebuffer(
        synthPrebufferSeconds_,
        duration_);
#endif
}

void MainWindow::setSynthRequestedSampleRate(int value)
{
    const int clamped =
        value <= 0
            ? 0
            : std::clamp(
                value,
                8000,
                384000);

    if (synthRequestedSampleRate_ == clamped)
        return;

    // AudioContext sample rate is chosen at context construction. Keep this
    // configurable before the backend starts; afterward the actual device rate
    // remains authoritative so pitch cannot drift.
    if (synthReady_)
        return;

    synthRequestedSampleRate_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthChannels(int value)
{
    const int clamped =
        value == 1 ? 1 : 2;

    if (synthChannels_ == clamped)
        return;

    synthChannels_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthBitsPerSample(int value)
{
    const int clamped =
        value == 16 ? 16 : 32;

    if (synthBitsPerSample_ == clamped)
        return;

    synthBitsPerSample_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthRealtimePriority(bool enabled)
{
    if (synthRealtimePriority_ == enabled)
        return;

    synthRealtimePriority_ = enabled;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthWorkers(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            256);

    if (synthWorkers_ == clamped)
        return;

    synthWorkers_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthNoteSharding(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            2);

    if (synthNoteSharding_ == clamped)
        return;

    synthNoteSharding_ = clamped;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthStealScoreCache(bool enabled)
{
    if (synthStealScoreCache_ == enabled)
        return;

    synthStealScoreCache_ = enabled;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthFastNoteOff(bool enabled)
{
    if (synthFastNoteOff_ == enabled)
        return;

    synthFastNoteOff_ = enabled;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthValidateState(bool enabled)
{
    if (synthValidateState_ == enabled)
        return;

    synthValidateState_ = enabled;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthSoftClip(bool enabled)
{
    if (synthSoftClip_ == enabled)
        return;

    synthSoftClip_ = enabled;
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthOverlapGain(bool enabled)
{
    if (synthOverlapGain_ == enabled)
        return;

    synthOverlapGain_ = enabled;
    emit synthConfigChanged();

#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_overlap_gain(
        enabled ? 1 : 0);

    if (soundfontLoaded_ && isPlaying_) {
        wasmidi_snappy_seek(currentTime_);
        resetSynthSchedule(currentTime_);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
#endif
}

void MainWindow::setChannelColor(
    int channel,
    const QColor& color)
{
    if (channel < 0 ||
        channel >= channelColors_.size() ||
        !color.isValid()) {
        return;
    }

    if (channelColors_[channel] == color)
        return;

    channelColors_[channel] = color;

    emit channelColorsChanged();
    updateNeuralVisuals();
}

void MainWindow::addVisualNote(std::size_t sourceIndex)
{
    if (sourceIndex >= document_.visualNotes.size())
        return;

    const auto& note = document_.visualNotes[sourceIndex];
    const uint8_t pitch = static_cast<uint8_t>((note.packedData >> 8) & 0x7f);
    const uint8_t color = document_.visualColorIndex(note, perTrackColors_) & 0x0f;

    addVisualCount(pitch, color, 1);

    // VisualNote is source/start ordered, so a seek rebuild naturally leaves
    // the newest still-active note as the representative keyboard color.
    visualPitchColor_[pitch] = static_cast<int8_t>(color);
}

void MainWindow::addVisualCount(uint8_t pitch, uint8_t color, uint32_t count)
{
    if (pitch >= 128 || color >= 16 || count == 0)
        return;

    const auto addClamped = [](uint32_t current, uint32_t amount) {
        const uint64_t sum = uint64_t(current) + uint64_t(amount);
        return static_cast<uint32_t>(
            std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max()));
    };

    visualPitchCount_[pitch] = addClamped(visualPitchCount_[pitch], count);
    visualPitchColorCounts_[pitch][color] =
        addClamped(visualPitchColorCounts_[pitch][color], count);
    visualColorVoices_[color] = addClamped(visualColorVoices_[color], count);

    visualActiveVoices_ = static_cast<int>(
        std::min<int64_t>(
            int64_t(std::numeric_limits<int>::max()),
            int64_t(visualActiveVoices_) + int64_t(count)));

    visualPitchMask_[pitch] = 1;
}

void MainWindow::removeVisualCount(uint8_t pitch, uint8_t color, uint32_t count)
{
    if (pitch >= 128 || color >= 16 || count == 0)
        return;

    const uint32_t removed =
        std::min(count, visualPitchColorCounts_[pitch][color]);

    visualPitchColorCounts_[pitch][color] -= removed;
    visualPitchCount_[pitch] -=
        std::min(removed, visualPitchCount_[pitch]);
    visualColorVoices_[color] -=
        std::min(removed, visualColorVoices_[color]);

    visualActiveVoices_ = static_cast<int>(
        std::max<int64_t>(
            0,
            int64_t(visualActiveVoices_) - int64_t(removed)));

    if (visualPitchCount_[pitch] == 0) {
        visualPitchMask_[pitch] = 0;
        visualPitchColor_[pitch] = -1;
        return;
    }

    if (visualPitchColor_[pitch] == static_cast<int8_t>(color) &&
        visualPitchColorCounts_[pitch][color] == 0) {
        // This is the same deterministic fallback used by the old heap path.
        // Exact held-state is represented by counts; the owner stream restores
        // the newest color whenever another start for this pitch is crossed.
        visualPitchColor_[pitch] = -1;
        for (int candidate = 15; candidate >= 0; --candidate) {
            if (visualPitchColorCounts_[pitch][candidate] != 0) {
                visualPitchColor_[pitch] = static_cast<int8_t>(candidate);
                break;
            }
        }
    }
}

void MainWindow::applyVisualKeyEvent(
    const wasmidi::VisualKeyEvent& event,
    bool add)
{
    const uint8_t pitch = document_.visualKeyPitch(event.packedData);
    const uint8_t color = document_.visualKeyColor(event.packedData, perTrackColors_) & 0x0f;

    if (add)
        addVisualCount(pitch, color, event.count);
    else
        removeVisualCount(pitch, color, event.count);
}

bool MainWindow::restoreVisualStateFromPage(double targetTick)
{
    if (keyboardVisualPages_.empty())
        return false;

    const KeyboardVisualPage* best = nullptr;
    uint64_t bestTick = 0;

    for (const auto& page : keyboardVisualPages_) {
        if (page.generation != keyboardVisualGeneration_ || page.spanTicks == 0)
            continue;

        const uint64_t tick = page.startTick();
        if (double(tick) > targetTick)
            continue;

        if (!best || tick >= bestTick) {
            best = &page;
            bestTick = tick;
        }
    }

    if (!best)
        return false;

    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);
    visualActiveVoices_ = 0;

    const auto& sourceCounts =
        perTrackColors_ ? best->trackCounts : best->globalCounts;

    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        uint64_t pitchTotal = 0;
        for (std::size_t color = 0; color < 16; ++color) {
            const uint32_t count = sourceCounts[pitch][color];
            visualPitchColorCounts_[pitch][color] = count;
            pitchTotal += count;
            visualColorVoices_[color] = static_cast<uint32_t>(
                std::min<uint64_t>(
                    uint64_t(std::numeric_limits<uint32_t>::max()),
                    uint64_t(visualColorVoices_[color]) + count));
        }

        visualPitchCount_[pitch] = static_cast<uint32_t>(
            std::min<uint64_t>(pitchTotal, std::numeric_limits<uint32_t>::max()));
        visualPitchMask_[pitch] = pitchTotal != 0 ? 1 : 0;

        if (pitchTotal == 0)
            continue;

        const uint32_t ownerWord = best->ownerColors[pitch];
        int owner = perTrackColors_
            ? int((ownerWord >> 8) & 0xffu)
            : int(ownerWord & 0xffu);

        if (owner < 0 || owner >= 16 ||
            visualPitchColorCounts_[pitch][std::size_t(owner)] == 0) {
            owner = -1;
            for (int candidate = 15; candidate >= 0; --candidate) {
                if (visualPitchColorCounts_[pitch][std::size_t(candidate)] != 0) {
                    owner = candidate;
                    break;
                }
            }
        }

        visualPitchColor_[pitch] = static_cast<int8_t>(owner);
        visualActiveVoices_ = static_cast<int>(
            std::min<int64_t>(
                int64_t(std::numeric_limits<int>::max()),
                int64_t(visualActiveVoices_) +
                    int64_t(std::min<uint64_t>(
                        pitchTotal,
                        uint64_t(std::numeric_limits<int>::max())))));
    }

    visualKeyStartCursor_ = std::min<std::size_t>(
        best->startCursor, document_.visualKeyStarts.size());
    visualKeyEndCursor_ = std::min<std::size_t>(
        best->endCursor, document_.visualKeyEnds.size());
    visualKeyOwnerCursor_ = std::min<std::size_t>(
        best->ownerCursor, document_.visualKeyOwners.size());
    visualTick_ = double(bestTick);
    visualStateValid_ = true;

    advanceVisualStateTo(targetTick);
    return true;
}

void MainWindow::advanceVisualStateTo(double targetTick)
{
    // Forward playback advances compact counted streams built by the MIDI
    // parser worker. A million identical starts at one crashpoint can now be
    // one add instead of one million heap pushes on the UI thread.
    while (visualKeyStartCursor_ < document_.visualKeyStarts.size() &&
           double(document_.visualKeyStarts[visualKeyStartCursor_].tick) <= targetTick) {
        applyVisualKeyEvent(document_.visualKeyStarts[visualKeyStartCursor_], true);
        ++visualKeyStartCursor_;
    }

    while (visualKeyOwnerCursor_ < document_.visualKeyOwners.size() &&
           double(document_.visualKeyOwners[visualKeyOwnerCursor_].tick) <= targetTick) {
        const auto& owner = document_.visualKeyOwners[visualKeyOwnerCursor_];
        const uint8_t pitch = document_.visualKeyPitch(owner.packedData);
        if (pitch < 128 && visualPitchCount_[pitch] != 0) {
            visualPitchColor_[pitch] = static_cast<int8_t>(
                document_.visualKeyColor(owner.packedData, perTrackColors_) & 0x0f);
        }
        ++visualKeyOwnerCursor_;
    }

    while (visualKeyEndCursor_ < document_.visualKeyEnds.size() &&
           double(document_.visualKeyEnds[visualKeyEndCursor_].tick) < targetTick) {
        applyVisualKeyEvent(document_.visualKeyEnds[visualKeyEndCursor_], false);
        ++visualKeyEndCursor_;
    }

    visualTick_ = targetTick;
}

void MainWindow::rebuildVisualStateAt(double targetTick)
{
    if (restoreVisualStateFromPage(targetTick))
        return;

    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);
    visualActiveVoices_ = 0;

    // Arbitrary seeks use the exact source stream plus its block max-end index.
    // This scans only blocks that can still contain a held note at targetTick.
    const std::size_t hi = document_.upperBoundVisualStart(targetTick);
    const std::size_t blockSize = wasmidi::MidiDocument::VisualSeekBlockSize;
    const std::size_t blockCount = (hi + blockSize - 1) / blockSize;

    for (std::size_t block = 0; block < blockCount; ++block) {
        if (block < document_.visualBlockMaxEnd.size() &&
            double(document_.visualBlockMaxEnd[block]) < targetTick) {
            continue;
        }

        const std::size_t begin = block * blockSize;
        const std::size_t end = std::min(hi, begin + blockSize);
        for (std::size_t i = begin; i < end; ++i) {
            const auto& note = document_.visualNotes[i];
            if (double(note.endTick) >= targetTick)
                addVisualNote(i);
        }
    }

    const auto startIt = std::upper_bound(
        document_.visualKeyStarts.begin(),
        document_.visualKeyStarts.end(),
        targetTick,
        [](double tick, const wasmidi::VisualKeyEvent& event) {
            return tick < double(event.tick);
        });
    visualKeyStartCursor_ = static_cast<std::size_t>(
        startIt - document_.visualKeyStarts.begin());

    // A note is active at its exact end tick, so only end events strictly
    // before targetTick have already been consumed.
    const auto endIt = std::lower_bound(
        document_.visualKeyEnds.begin(),
        document_.visualKeyEnds.end(),
        targetTick,
        [](const wasmidi::VisualKeyEvent& event, double tick) {
            return double(event.tick) < tick;
        });
    visualKeyEndCursor_ = static_cast<std::size_t>(
        endIt - document_.visualKeyEnds.begin());

    const auto ownerIt = std::upper_bound(
        document_.visualKeyOwners.begin(),
        document_.visualKeyOwners.end(),
        targetTick,
        [](double tick, const wasmidi::VisualKeyOwner& owner) {
            return tick < double(owner.tick);
        });
    visualKeyOwnerCursor_ = static_cast<std::size_t>(
        ownerIt - document_.visualKeyOwners.begin());

    visualTick_ = targetTick;
    visualStateValid_ = true;
}

void MainWindow::syncVisualState(double targetTick, bool forceRebuild)
{
    if (document_.visualNotes.empty()) {
        clearVisualState();
        return;
    }

    const auto oldMask = visualPitchMask_;
    const auto oldColors = visualPitchColor_;

    if (forceRebuild || !visualStateValid_ || targetTick < visualTick_) {
        rebuildVisualStateAt(targetTick);
    } else {
        advanceVisualStateTo(targetTick);
    }

    if (oldMask != visualPitchMask_ || oldColors != visualPitchColor_)
        emit activePitchesChanged();
}

void MainWindow::updateNeuralVisuals()
{
    float newHue = dominantHue_;

    if (!document_.visualNotes.empty()) {
        const double futureSeconds =
            std::min<double>(duration_, double(currentTime_) + 0.15);
        const double futureTick = document_.secondsToTick(futureSeconds);

        const std::size_t targetLo = document_.upperBoundVisualStart(
            document_.secondsToTick(currentTime_));
        const std::size_t targetHi = document_.upperBoundVisualStart(futureTick);

        const bool rebuild =
            !neuralWindowValid_ ||
            targetLo < neuralFutureLo_ ||
            targetHi < neuralFutureHi_ ||
            targetLo > targetHi;

        auto addNoteColor = [this](std::size_t index, int direction) {
            if (index >= document_.visualNotes.size())
                return;
            const uint8_t color =
                document_.visualColorIndex(document_.visualNotes[index], perTrackColors_) & 0x0f;
            if (direction > 0) {
                ++neuralFutureColors_[color];
            } else if (neuralFutureColors_[color] != 0) {
                --neuralFutureColors_[color];
            }
        };

        if (rebuild) {
            neuralFutureColors_.fill(0);
            for (std::size_t i = targetLo; i < targetHi; ++i)
                addNoteColor(i, +1);
        } else {
            while (neuralFutureLo_ < targetLo) {
                addNoteColor(neuralFutureLo_, -1);
                ++neuralFutureLo_;
            }
            while (neuralFutureHi_ < targetHi) {
                addNoteColor(neuralFutureHi_, +1);
                ++neuralFutureHi_;
            }
        }

        neuralFutureLo_ = targetLo;
        neuralFutureHi_ = targetHi;
        neuralWindowValid_ = true;

        uint64_t bestCount = 0;
        int bestColor = -1;
        for (int color = 0; color < 16; ++color) {
            const uint64_t combined =
                uint64_t(visualColorVoices_[color]) + neuralFutureColors_[color];
            if (combined > bestCount) {
                bestCount = combined;
                bestColor = color;
            }
        }

        if (bestColor >= 0 && bestColor < channelColors_.size())
            newHue = colorHue(channelColors_[bestColor]);
    }

    const float activity =
        peakNps_ > 0
            ? std::min(1.0f, float(nps_) / float(peakNps_))
            : 0.0f;
    const float newActivity = activity * 0.7f + neuralActivity_ * 0.3f;

    if (!qFuzzyCompare(newHue, dominantHue_) ||
        !qFuzzyCompare(newActivity, neuralActivity_)) {
        dominantHue_ = newHue;
        neuralActivity_ = newActivity;
        emit neuralVisualChanged();
    }
}

void MainWindow::updateLiveStats()
{
    if (!hasMidi() ||
        document_.tickGroups.empty()) {
        if (nps_ != 0) {
            nps_ = 0;
            emit npsChanged();
        }

        if (activeVoices_ != 0) {
            activeVoices_ = 0;
            emit activeVoicesChanged();
        }

        if (ccPerSecond_ != 0) {
            ccPerSecond_ = 0;
            emit ccPerSecondChanged();
        }

        return;
    }

    const double time =
        currentTime_;

    const uint32_t currentTick =
        static_cast<uint32_t>(
            std::min<double>(
                document_.maxTick,
                std::floor(
                    document_.secondsToTick(
                        time))));

    // MPWGL2 live NPS is exactly:
    // (upperBound(startArr,t) - lowerBound(startArr,t-0.25)) * 4.
    // visualNotes is the same stable start-ordered note stream, so perform the
    // identical binary searches in fractional tick space.
    const double currentVisualTick =
        document_.secondsToTick(time);

    const double npsStartVisualTick =
        document_.secondsToTick(
            std::max(
                0.0,
                time - 0.25));

    const std::size_t exactNpsLo =
        document_.lowerBoundVisualStart(
            npsStartVisualTick);

    const std::size_t exactNpsHi =
        document_.upperBoundVisualStart(
            currentVisualTick);

    const uint32_t ccStartTick =
        static_cast<uint32_t>(
            std::max(
                0.0,
                std::floor(
                    document_.secondsToTick(
                        std::max(
                            0.0,
                            time - 1.0)))));

    const std::size_t targetHi =
        document_.upperBoundGroup(
            currentTick);

    const std::size_t targetCcLo =
        document_.lowerBoundGroup(
            ccStartTick);

    const bool discontinuity =
        !liveWindowsValid_ ||
        time < liveLastTime_ ||
        time - liveLastTime_ > 0.5;

    if (discontinuity) {
        liveNpsCount_ = 0;
        liveCcCount_ = 0;

        for (std::size_t i = targetCcLo;
             i < targetHi;
             ++i) {
            liveCcCount_ +=
                document_.tickGroups[i]
                    .controlCount;
        }

        liveHi_ = targetHi;
        liveNpsLo_ = 0;
        liveCcLo_ = targetCcLo;
        liveWindowsValid_ = true;
    } else {
        while (liveHi_ < targetHi) {
            const auto& group =
                document_.tickGroups[liveHi_];

            liveCcCount_ +=
                group.controlCount;

            ++liveHi_;
        }
        while (liveCcLo_ < targetCcLo &&
               liveCcLo_ < liveHi_) {
            liveCcCount_ -=
                document_.tickGroups[
                    liveCcLo_]
                    .controlCount;

            ++liveCcLo_;
        }
    }

    liveLastTime_ =
        static_cast<float>(time);

    // Keyboard/polyphony state uses the fractional tick corresponding to the
    // exact playback time. Using floor(currentTick) kept NoteOff keys lit until
    // the next whole MIDI tick, which was visible at low PPQ / slow tempos.
    syncVisualState(
        currentVisualTick);

    const uint64_t exactNpsCount =
        exactNpsHi >= exactNpsLo
            ? uint64_t(
                exactNpsHi -
                exactNpsLo)
            : 0u;

    const int newNps =
        static_cast<int>(
            std::min<uint64_t>(
                exactNpsCount * 4u,
                uint64_t(
                    std::numeric_limits<int>::max())));

    const int newCc =
        static_cast<int>(
            std::min<uint64_t>(
                liveCcCount_,
                uint64_t(
                    std::numeric_limits<int>::max())));

    const int newActive =
        visualActiveVoices_;

    float newBpm = 120.0f;

    if (!tempoTimes_.empty()) {
        auto it =
            std::upper_bound(
                tempoTimes_.begin(),
                tempoTimes_.end(),
                currentTime_);

        std::size_t index = 0;

        if (it != tempoTimes_.begin()) {
            index =
                static_cast<std::size_t>(
                    (it -
                     tempoTimes_.begin()) -
                    1);
        }

        index =
            std::min(
                index,
                tempoBpms_.size() - 1);

        newBpm =
            tempoBpms_[index];
    }

    if (nps_ != newNps) {
        nps_ = newNps;
        emit npsChanged();
    }

    if (activeVoices_ != newActive) {
        activeVoices_ = newActive;
        emit activeVoicesChanged();
    }

    if (ccPerSecond_ != newCc) {
        ccPerSecond_ = newCc;
        emit ccPerSecondChanged();
    }

    if (!qFuzzyCompare(
            bpm_,
            newBpm)) {
        bpm_ = newBpm;
        emit bpmChanged();
    }

    updateNeuralVisuals();
}

void MainWindow::updateCurrentTime()
{
    if (!isPlaying_)
        return;

    // Advance the master timeline only after the horizontal visualizer has
    // actually completed another frame. This turns renderer slowdown into
    // transport slowdown instead of letting keyboard/audio run ahead.
    if (visualFrameSerial_ == playbackConsumedVisualFrameSerial_)
        return;

    playbackConsumedVisualFrameSerial_ = visualFrameSerial_;

    // The visual timeline is authoritative. Advance it from GUI-frame time,
    // never from SnappySynth's AudioWorklet clock. A long browser/UI stall is
    // deliberately clamped instead of being converted into a giant visual
    // jump; keyboard and roll therefore remain together and the synth is
    // resynchronized to this clock after the UI recovers.
    const qint64 elapsedMs =
        playbackClock_.isValid()
            ? playbackClock_.elapsed()
            : 0;

    qint64 deltaMs =
        std::max<qint64>(
            0,
            elapsedMs - playbackLastElapsedMs_);

    playbackLastElapsedMs_ = elapsedMs;

    constexpr qint64 MaxVisualStepMs = 50;
    deltaMs = std::min(deltaMs, MaxVisualStepMs);

    const float nextTime =
        currentTime_ +
        static_cast<float>(deltaMs) / 1000.0f;

    currentTime_ =
        std::clamp(
            nextTime,
            0.0f,
            duration_);

    if (currentTime_ >= duration_) {
        stop();
        return;
    }

#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_)
        wasmidi_snappy_sync_visual_clock(currentTime_);
#endif

    updateSynthSynchronization();

    emit currentTimeChanged();
    updateLiveStats();
}

void MainWindow::updateEffectiveVelocityFloor(double synthLagSeconds)
{
    int target = synthVelocityFloor_;

    // While audio is behind, progressively drop quiet NoteOns. The user's
    // configured floor is always the minimum. At >=350 ms lag the adaptive
    // floor reaches 127, leaving only maximum-velocity NoteOns until recovery.
    if (synthLagSeconds > 0.025) {
        const double pressure =
            std::clamp(
                (synthLagSeconds - 0.025) / 0.325,
                0.0,
                1.0);

        target =
            synthVelocityFloor_ +
            static_cast<int>(
                std::lround(
                    pressure *
                    double(127 - synthVelocityFloor_)));
    }

    target = std::clamp(target, synthVelocityFloor_, 127);

    if (target > synthEffectiveVelocityFloor_) {
        // Catch-up pressure takes effect immediately.
        synthEffectiveVelocityFloor_ = target;
    } else if (target < synthEffectiveVelocityFloor_) {
        // Recover gradually so an oscillating deadline does not chatter the
        // audible velocity cutoff on every 16 ms GUI tick.
        synthEffectiveVelocityFloor_ =
            std::max(
                target,
                synthEffectiveVelocityFloor_ - 2);
    }

    if (skippedVelocity_ != synthEffectiveVelocityFloor_) {
        skippedVelocity_ = synthEffectiveVelocityFloor_;
        emit skippedVelocityChanged();
    }
}

void MainWindow::updateSynthSynchronization()
{
#ifdef __EMSCRIPTEN__
    if (!soundfontLoaded_) {
        updateEffectiveVelocityFloor(0.0);
        return;
    }

    const double audioTime = wasmidi_snappy_audio_clock();
    if (audioTime < 0.0 || !std::isfinite(audioTime))
        return;

    const double lag =
        double(currentTime_) - audioTime;

    updateEffectiveVelocityFloor(lag);

    const bool starved =
        wasmidi_snappy_starved() != 0;

    // Audio follows the visual timeline, never the reverse. During starvation
    // the background renderer is allowed to refill without forcing the UI to
    // wait. As soon as the AudioWorklet becomes healthy again, rebuild at the
    // exact visual position so playback never resumes from an old lagged clock.
    const bool recoveredFromStarvation =
        synthWasStarved_ && !starved;
    synthWasStarved_ = starved;

    constexpr double HardResyncLagSeconds = 0.10;
    constexpr qint64 HardResyncCooldownMs = 100;

    const qint64 elapsedMs =
        playbackClock_.isValid()
            ? playbackClock_.elapsed()
            : 0;

    if (!starved &&
        (recoveredFromStarvation ||
         std::abs(lag) >= HardResyncLagSeconds) &&
        elapsedMs - synthLastHardResyncElapsedMs_ >=
            HardResyncCooldownMs) {
        synthLastHardResyncElapsedMs_ = elapsedMs;

        wasmidi_snappy_seek(currentTime_);
        resetSynthSchedule(currentTime_);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
        wasmidi_snappy_sync_visual_clock(currentTime_);
    }
#else
    updateEffectiveVelocityFloor(0.0);
#endif
}

uint32_t MainWindow::packSynthMessage(const wasmidi::CompactEvent& event) const
{
    const uint8_t command = event.status & 0xf0;
    if (command != 0x80 && command != 0x90 && command != 0xb0 &&
        command != 0xc0 && command != 0xe0) {
        return 0xffffffffu;
    }

    return uint32_t(event.status) |
           (uint32_t(event.data1) << 8) |
           (uint32_t(event.data2) << 16);
}

void MainWindow::resetSynthSchedule(float seconds)
{
    const double time = std::clamp<double>(seconds, 0.0, duration_);
    const uint32_t tick = static_cast<uint32_t>(
        std::max(0.0, std::floor(document_.secondsToTick(time))));

    synthGroupCursor_ = document_.lowerBoundGroup(tick);
    synthSysExCursor_ = static_cast<std::size_t>(
        std::lower_bound(
            document_.sysEx.begin(), document_.sysEx.end(), tick,
            [](const wasmidi::SysExEvent& event, uint32_t value) {
                return event.tick < value;
            }) - document_.sysEx.begin());
    synthScheduledUntil_ = time;
    synthMessages_.clear();
    synthTimes_.clear();

#ifdef __EMSCRIPTEN__
    if (!soundfontLoaded_)
        return;

    // Restore SnappySynthV2's GM/GS/XG SysEx state exactly as the original
    // driver does. SysEx count is normally tiny, so replaying the historical
    // state at seek is cheap and preserves drum-part/channel remaps/tuning.
    for (std::size_t si = 0; si < synthSysExCursor_; ++si) {
        const auto& sx = document_.sysEx[si];
        if (!sx.data.empty()) {
            wasmidi_snappy_schedule_sysex(
                sx.data.data(), static_cast<int>(sx.data.size()), time);
        }
    }

    // Restore the last controller/program/bend state at a seek. Groups with no
    // control data are skipped, so this scans the sparse control history rather
    // than materializing or replaying every NoteOn in a Black MIDI.
    std::array<std::array<int16_t, 128>, 16> cc{};
    for (auto& row : cc)
        row.fill(-1);
    std::array<int16_t, 16> program{};
    std::array<int16_t, 16> bend{};
    program.fill(-1);
    bend.fill(-1);

    for (std::size_t gi = 0; gi < synthGroupCursor_; ++gi) {
        const auto& group = document_.tickGroups[gi];
        if (group.controlCount == 0)
            continue;
        const std::size_t end = group.eventOffset + group.eventCount;
        for (std::size_t ei = group.eventOffset; ei < end; ++ei) {
            const auto& event = document_.events[ei];
            const uint8_t cmd = event.status & 0xf0;
            const uint8_t ch = event.status & 0x0f;
            if (cmd == 0xb0)
                cc[ch][event.data1 & 0x7f] = event.data2 & 0x7f;
            else if (cmd == 0xc0)
                program[ch] = event.data1 & 0x7f;
            else if (cmd == 0xe0)
                bend[ch] = int16_t((event.data1 & 0x7f) | ((event.data2 & 0x7f) << 7));
        }
    }

    auto appendState = [this, time](uint32_t message) {
        synthMessages_.push_back(message);
        synthTimes_.push_back(time);
    };

    for (int ch = 0; ch < 16; ++ch) {
        if (cc[ch][0] >= 0)
            appendState(uint32_t(0xb0 | ch) | (0u << 8) | (uint32_t(cc[ch][0]) << 16));
        if (cc[ch][32] >= 0)
            appendState(uint32_t(0xb0 | ch) | (32u << 8) | (uint32_t(cc[ch][32]) << 16));
        if (program[ch] >= 0)
            appendState(uint32_t(0xc0 | ch) | (uint32_t(program[ch]) << 8));
        for (int controller = 1; controller < 128; ++controller) {
            if (controller == 32 || cc[ch][controller] < 0)
                continue;
            appendState(uint32_t(0xb0 | ch) |
                        (uint32_t(controller) << 8) |
                        (uint32_t(cc[ch][controller]) << 16));
        }
        if (bend[ch] >= 0) {
            const uint16_t value = static_cast<uint16_t>(bend[ch]);
            appendState(uint32_t(0xe0 | ch) |
                        (uint32_t(value & 0x7f) << 8) |
                        (uint32_t((value >> 7) & 0x7f) << 16));
        }
    }

    // Restore notes that genuinely span the seek point. VisualNote carries the
    // original channel in bits 24..27; future original NoteOff events will close
    // these voices at their correct MIDI time. Notes that start exactly on the
    // seek tick are NOT restored here because synthGroupCursor_ will schedule
    // their original NoteOns, preventing duplicate voices at the boundary.
    const std::size_t hi =
        tick == 0
            ? 0
            : document_.lowerBoundVisualStart(double(tick));
    const std::size_t blockSize = wasmidi::MidiDocument::VisualSeekBlockSize;
    const std::size_t blockCount = (hi + blockSize - 1) / blockSize;
    for (std::size_t block = 0; block < blockCount; ++block) {
        if (block < document_.visualBlockMaxEnd.size() &&
            document_.visualBlockMaxEnd[block] < tick)
            continue;
        const std::size_t begin = block * blockSize;
        const std::size_t end = std::min(hi, begin + blockSize);
        for (std::size_t i = begin; i < end; ++i) {
            const auto& note = document_.visualNotes[i];
            if (note.endTick < tick)
                continue;
            const uint8_t velocity = note.packedData & 0x7f;
            if (velocity < synthEffectiveVelocityFloor_)
                continue;
            const uint8_t pitch = (note.packedData >> 8) & 0x7f;
            const uint8_t channel = (note.packedData >> 24) & 0x0f;
            appendState(uint32_t(0x90 | channel) |
                        (uint32_t(pitch) << 8) |
                        (uint32_t(velocity) << 16));
        }
    }

    if (!synthMessages_.empty()) {
        wasmidi_snappy_schedule(synthMessages_.data(), synthTimes_.data(),
                                static_cast<int>(synthMessages_.size()), time);
    } else {
        wasmidi_snappy_schedule(nullptr, nullptr, 0, time);
    }
    synthMessages_.clear();
    synthTimes_.clear();
#endif
}

void MainWindow::scheduleSynthAhead()
{
#ifdef __EMSCRIPTEN__
    if (!isPlaying_ || !soundfontLoaded_ || document_.tickGroups.empty())
        return;

    constexpr std::size_t BatchLimit = 65536;

    const double requestedAhead =
        synthPrebufferSeconds_ <= 0.0f
            ? std::max(0.0, double(duration_) - double(currentTime_))
            : double(synthPrebufferSeconds_);

    // Keep the worker's rolling PCM cache supplied through its full requested
    // horizon. A small guard gives the worker one additional render quantum so
    // it never reaches the safeUntil boundary exactly at the worklet deadline.
    const double target =
        std::min<double>(
            duration_,
            double(currentTime_) + requestedAhead + 0.10);
    if (target <= synthScheduledUntil_ + 0.015)
        return;

    const uint32_t targetTick = static_cast<uint32_t>(
        std::min<double>(document_.maxTick,
                         std::ceil(document_.secondsToTick(target))));
    const std::size_t endGroup = document_.upperBoundGroup(targetTick);

    auto flush = [this](double safeUntil) {
        if (synthMessages_.empty()) {
            wasmidi_snappy_schedule(nullptr, nullptr, 0, safeUntil);
            return;
        }
        wasmidi_snappy_schedule(synthMessages_.data(), synthTimes_.data(),
                                static_cast<int>(synthMessages_.size()), safeUntil);
        synthMessages_.clear();
        synthTimes_.clear();
    };

    synthMessages_.clear();
    synthTimes_.clear();
    synthMessages_.reserve(std::min<std::size_t>(BatchLimit, 8192));
    synthTimes_.reserve(std::min<std::size_t>(BatchLimit, 8192));

    for (; synthGroupCursor_ < endGroup; ++synthGroupCursor_) {
        const auto& group = document_.tickGroups[synthGroupCursor_];
        const double eventTime = document_.tickToSeconds(group.tick);
        const std::size_t end = group.eventOffset + group.eventCount;

        for (std::size_t i = group.eventOffset; i < end; ++i) {
            const auto& event = document_.events[i];
            uint32_t message = packSynthMessage(event);
            if (message == 0xffffffffu)
                continue;

            // Source SnappySynthV2 supports exact-overlap stack count in the
            // high byte. Pack only consecutive identical NoteOns so controller
            // ordering at the same tick can never be changed.
            if ((event.status & 0xf0) == 0x90 && event.data2 != 0) {
                std::size_t run = 1;
                while (i + run < end && run < 256) {
                    const auto& next = document_.events[i + run];
                    if (packSynthMessage(next) != message)
                        break;
                    ++run;
                }

                if (event.data2 < synthEffectiveVelocityFloor_) {
                    i += run - 1;
                    continue;
                }

                if (run > 1) {
                    message |= uint32_t(run - 1) << 24;
                    i += run - 1;
                }
            }

            synthMessages_.push_back(message);
            synthTimes_.push_back(eventTime);

            if (synthMessages_.size() >= BatchLimit)
                flush(synthScheduledUntil_);
        }
    }

    // Forward all original SysEx through the SnappySynthV2 SysEx dispatcher.
    while (synthSysExCursor_ < document_.sysEx.size() &&
           document_.sysEx[synthSysExCursor_].tick <= targetTick) {
        const auto& sx = document_.sysEx[synthSysExCursor_++];
        if (!sx.data.empty()) {
            wasmidi_snappy_schedule_sysex(
                sx.data.data(), static_cast<int>(sx.data.size()),
                document_.tickToSeconds(sx.tick));
        }
    }

    // Only advance safeUntil after every event through the target horizon has
    // been transferred; this prevents the audio worker from rendering past a
    // partially delivered ultra-dense tick.
    // Let the final audio block cross the exact MIDI duration by a tiny
    // bounded margin. Without this, a 128/256/512-frame AudioWorklet request
    // that straddles the last MIDI timestamp can be refused forever, leaving
    // the audio clock a few samples short of `duration_` and preventing the
    // normal end-of-song stop condition.
    double safeUntil = target;
    if (target >= double(duration_) - 1e-7) {
        const double rate = synthSampleRate_ > 0
            ? double(synthSampleRate_)
            : 48000.0;
        safeUntil += std::max(
            0.05,
            (double(synthBufferFrames_) / rate) * 2.0);
    }

    flush(safeUntil);
    synthScheduledUntil_ = target;
#endif
}

void MainWindow::dispatchScheduler()
{
    scheduleSynthAhead();
}

void MainWindow::pollSynthState()
{
#ifdef __EMSCRIPTEN__
    const bool ready = wasmidi_snappy_ready() != 0;
    const bool loaded = wasmidi_snappy_soundfont_loaded() != 0;
    const int sampleRate = wasmidi_snappy_sample_rate();
    const int active = wasmidi_snappy_active_voices();
    const int freeVoices = wasmidi_snappy_free_voices();
    const int steals = wasmidi_snappy_steals();
    const int layers = wasmidi_snappy_layers();
    const int regions = wasmidi_snappy_regions();
    const int workerCount = wasmidi_snappy_worker_count();
    const int underruns = wasmidi_snappy_underruns();

    char nameBuffer[512] = {};
    char statusBuffer[512] = {};
    wasmidi_snappy_copy_string(0, nameBuffer, int(sizeof(nameBuffer)));
    wasmidi_snappy_copy_string(1, statusBuffer, int(sizeof(statusBuffer)));
    const QString name = QString::fromUtf8(nameBuffer);
    const QString status = QString::fromUtf8(statusBuffer);

    const bool changed =
        ready != synthReady_ || loaded != soundfontLoaded_ ||
        sampleRate != synthSampleRate_ || active != synthActiveVoices_ ||
        freeVoices != synthFreeVoices_ || steals != synthSteals_ ||
        layers != synthLayers_ || regions != synthRegions_ ||
        workerCount != synthWorkerCount_ ||
        underruns != synthUnderruns_ || name != soundfontName_ || status != synthStatus_;

    synthReady_ = ready;
    soundfontLoaded_ = loaded;
    synthSampleRate_ = sampleRate;
    synthActiveVoices_ = active;
    synthFreeVoices_ = freeVoices;
    synthSteals_ = steals;
    synthLayers_ = layers;
    synthRegions_ = regions;
    synthWorkerCount_ = workerCount;
    synthUnderruns_ = underruns;
    soundfontName_ = name;
    synthStatus_ = status.isEmpty() ? QStringLiteral("SnappySynthV2 idle") : status;

    if (loaded && !synthWasSoundfontLoaded_) {
        synthPlaybackPrimed_ = false;
        if (isPlaying_) {
            wasmidi_snappy_play(currentTime_, 1);
            resetSynthSchedule(currentTime_);
            synthPlaybackPrimed_ = true;
            scheduleSynthAhead();
        }
    }
    if (!loaded)
        synthPlaybackPrimed_ = false;
    synthWasSoundfontLoaded_ = loaded;

    if (changed)
        emit synthStateChanged();
#endif
}
