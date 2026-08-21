#include "mainwindow.hpp"

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

// Stream the browser File directly into one WASM allocation instead of
// File.arrayBuffer() + Uint8Array + HEAP copy. This removes the second full
// file-sized JS allocation during loading and yields between stream chunks.
EM_JS(void, wasmidi_browser_open_midi_picker, (), {
    let input = document.getElementById('wasmidi-midi-file-input');

    if (!input) {
        input = document.createElement('input');
        input.id = 'wasmidi-midi-file-input';
        input.type = 'file';
        input.accept = '.mid,.midi,audio/midi,audio/x-midi';
        input.style.position = 'fixed';
        input.style.left = '-10000px';
        input.style.top = '-10000px';
        input.style.width = '1px';
        input.style.height = '1px';
        input.style.opacity = '0';
        document.body.appendChild(input);
    }

    input.onchange = async () => {
        const file =
            input.files && input.files.length
                ? input.files[0]
                : null;

        if (!file) {
            input.value = null;
            return;
        }

        let dataPtr = 0;
        let namePtr = 0;

        try {
            if (file.size > 0x7fffffff)
                throw new Error('MIDI is too large for this wasm32 build.');

            dataPtr = _malloc(Math.max(1, file.size));

            if (!dataPtr)
                throw new Error('Could not allocate WASM memory for MIDI.');

            let offset = 0;

            if (file.stream) {
                const reader = file.stream().getReader();

                while (true) {
                    const result = await reader.read();

                    if (result.done)
                        break;

                    const chunk = result.value;

                    HEAPU8.set(
                        chunk,
                        dataPtr + offset);

                    offset += chunk.byteLength;
                }
            } else {
                // Compatibility fallback for older browsers.
                const bytes =
                    new Uint8Array(
                        await file.arrayBuffer());

                HEAPU8.set(bytes, dataPtr);
                offset = bytes.length;
            }

            const nameBytes =
                new TextEncoder().encode(
                    file.name || 'browser.mid');

            namePtr =
                _malloc(
                    Math.max(1, nameBytes.length));

            if (nameBytes.length)
                HEAPU8.set(nameBytes, namePtr);

            _wasmidi_browser_file_selected(
                dataPtr,
                offset,
                namePtr,
                nameBytes.length);
        } catch (error) {
            console.error(
                '[WASMIDI] Could not load selected MIDI:',
                error);
        } finally {
            if (namePtr)
                _free(namePtr);

            if (dataPtr)
                _free(dataPtr);

            input.value = null;
        }
    };

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

EM_JS(int, wasmidi_snappy_underruns, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.underruns) | 0) : 0;
});

EM_JS(double, wasmidi_snappy_audio_clock, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.getAudioClock ? Number(b.getAudioClock()) : -1.0;
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

EM_JS(void, wasmidi_snappy_open_soundfont, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.openSoundfont)
        b.openSoundfont();
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

EM_JS(void, wasmidi_snappy_set_volume, (int percent), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setVolume)
        b.setVolume(percent);
});

EM_JS(void, wasmidi_snappy_configure, (int maxVoices, int blockFrames), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.configure)
        b.configure(maxVoices, blockFrames);
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

    updateLiveStats();
    emit fileLoaded();

    return true;
}

void MainWindow::openMidiPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_open_midi_picker();
#else
    emit loadFailed(
        QStringLiteral(
            "The browser MIDI picker is available in the WebAssembly build."));
#endif
}

void MainWindow::openSoundfontPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_open_soundfont();
#else
    synthStatus_ = QStringLiteral("SnappySynthV2 SF2 picker is available in the WebAssembly build.");
    emit synthStateChanged();
#endif
}


void MainWindow::clearVisualState()
{
    visualEndHeap_.clear();
    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);

    visualActiveVoices_ = 0;
    visualTick_ = 0.0;
    visualStartCursor_ = 0;
    visualStateValid_ = false;
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
    skippedVelocity_ = 0;

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

void MainWindow::play()
{
    if (!hasMidi())
        return;

    if (currentTime_ >= duration_)
        currentTime_ = 0.0f;

    playbackAnchorSeconds_ = currentTime_;
    playbackClock_.restart();

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

    if (isPlaying_)
        playbackClock_.restart();

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
#endif
}


void MainWindow::setSynthMaxVoices(int value)
{
    const int clamped = std::clamp(value, 128, 262144);
    if (synthMaxVoices_ == clamped)
        return;
    synthMaxVoices_ = clamped;
    synthPlaybackPrimed_ = false;
    emit synthConfigChanged();
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_configure(synthMaxVoices_, synthBufferFrames_);
#endif
}

void MainWindow::setSynthBufferFrames(int value)
{
    int clamped = std::clamp(value, 128, 2048);
    // Keep the worker/render block on a power of two, which is the efficient
    // path for both the source voice engine and the AudioWorklet queue.
    int pow2 = 128;
    while (pow2 < clamped && pow2 < 2048)
        pow2 <<= 1;
    clamped = pow2;
    if (synthBufferFrames_ == clamped)
        return;
    synthBufferFrames_ = clamped;
    synthPlaybackPrimed_ = false;
    emit synthConfigChanged();
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_configure(synthMaxVoices_, synthBufferFrames_);
#endif
}

void MainWindow::setSynthOverlapGain(bool enabled)
{
    if (synthOverlapGain_ == enabled)
        return;
    synthOverlapGain_ = enabled;
    emit synthConfigChanged();
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_overlap_gain(enabled ? 1 : 0);
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

    ++visualPitchCount_[pitch];
    ++visualPitchColorCounts_[pitch][color];
    ++visualColorVoices_[color];
    ++visualActiveVoices_;

    visualPitchMask_[pitch] = 1;
    // VisualNote is start-ordered, so the newest active note owns the key color.
    visualPitchColor_[pitch] = static_cast<int8_t>(color);

    visualEndHeap_.push_back({note.endTick, static_cast<uint32_t>(sourceIndex), pitch, color});
    std::push_heap(
        visualEndHeap_.begin(), visualEndHeap_.end(),
        [](const VisualActiveEnd& left, const VisualActiveEnd& right) {
            if (left.endTick != right.endTick)
                return left.endTick > right.endTick;
            return left.sourceIndex > right.sourceIndex;
        });
}

void MainWindow::removeVisualNote(uint8_t pitch, uint8_t color)
{
    if (visualPitchCount_[pitch] != 0)
        --visualPitchCount_[pitch];
    if (visualPitchColorCounts_[pitch][color] != 0)
        --visualPitchColorCounts_[pitch][color];
    if (visualColorVoices_[color] != 0)
        --visualColorVoices_[color];
    visualActiveVoices_ = std::max(0, visualActiveVoices_ - 1);

    if (visualPitchCount_[pitch] == 0) {
        visualPitchMask_[pitch] = 0;
        visualPitchColor_[pitch] = -1;
        return;
    }

    if (visualPitchColor_[pitch] == static_cast<int8_t>(color) &&
        visualPitchColorCounts_[pitch][color] == 0) {
        // The original key visual ultimately only needs one representative
        // active color. Prefer the highest currently populated palette slot;
        // held-state correctness is independent of this tie-break.
        for (int candidate = 15; candidate >= 0; --candidate) {
            if (visualPitchColorCounts_[pitch][candidate] != 0) {
                visualPitchColor_[pitch] = static_cast<int8_t>(candidate);
                break;
            }
        }
    }
}

void MainWindow::rebuildVisualStateAt(double targetTick)
{
    visualEndHeap_.clear();
    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);
    visualActiveVoices_ = 0;

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

    visualStartCursor_ = hi;
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
        const auto heapCompare =
            [](const VisualActiveEnd& left, const VisualActiveEnd& right) {
                if (left.endTick != right.endTick)
                    return left.endTick > right.endTick;
                return left.sourceIndex > right.sourceIndex;
            };

        // A note remains held at its exact MIDI end tick, matching the visual
        // interval test endTick >= currentTick. Remove it on the first later tick.
        while (!visualEndHeap_.empty() &&
               double(visualEndHeap_.front().endTick) < targetTick) {
            std::pop_heap(visualEndHeap_.begin(), visualEndHeap_.end(), heapCompare);
            const VisualActiveEnd ended = visualEndHeap_.back();
            visualEndHeap_.pop_back();
            removeVisualNote(ended.pitch, ended.color);
        }

        while (visualStartCursor_ < document_.visualNotes.size() &&
               double(document_.visualNotes[visualStartCursor_].startTick) <= targetTick) {
            const auto& note = document_.visualNotes[visualStartCursor_];
            if (double(note.endTick) >= targetTick)
                addVisualNote(visualStartCursor_);
            ++visualStartCursor_;
        }

        visualTick_ = targetTick;
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

        const std::size_t targetLo = visualStartCursor_;
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

    float nextTime =
        playbackAnchorSeconds_ +
        static_cast<float>(playbackClock_.elapsed()) / 1000.0f;

#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_) {
        const double audioTime = wasmidi_snappy_audio_clock();
        if (audioTime >= 0.0 && std::isfinite(audioTime))
            nextTime = static_cast<float>(audioTime);
    }
#endif

    currentTime_ = std::clamp(nextTime, 0.0f, duration_);

    if (currentTime_ >= duration_) {
        stop();
        return;
    }

    emit currentTimeChanged();
    updateLiveStats();
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
    synthScheduledUntil_ = time;
    synthMessages_.clear();
    synthTimes_.clear();

#ifdef __EMSCRIPTEN__
    if (!soundfontLoaded_)
        return;

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

    constexpr double LookAheadSeconds = 0.40;
    constexpr std::size_t BatchLimit = 65536;

    const double target = std::min<double>(duration_, double(currentTime_) + LookAheadSeconds);
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
        underruns != synthUnderruns_ || name != soundfontName_ || status != synthStatus_;

    synthReady_ = ready;
    soundfontLoaded_ = loaded;
    synthSampleRate_ = sampleRate;
    synthActiveVoices_ = active;
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
