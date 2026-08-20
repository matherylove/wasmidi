#include "mainwindow.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTimer>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {
MainWindow* g_browserMainWindow = nullptr;
}

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_file_selected(const unsigned char* data,
                                   int dataSize,
                                   const char* fileName,
                                   int fileNameSize)
{
    if (!g_browserMainWindow || !data || dataSize <= 0)
        return;

    const QByteArray midiBytes(reinterpret_cast<const char*>(data), dataSize);
    const QString name = fileName && fileNameSize > 0
        ? QString::fromUtf8(fileName, fileNameSize)
        : QStringLiteral("browser.mid");

    g_browserMainWindow->loadMidiFileNamed(midiBytes, name);
}

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
        const file = input.files && input.files.length ? input.files[0] : null;
        if (!file) {
            input.value = '';
            return;
        }

        try {
            const buffer = await file.arrayBuffer();
            const bytes = new Uint8Array(buffer);
            const nameBytes = new TextEncoder().encode(file.name || 'browser.mid');

            const dataPtr = _malloc(Math.max(1, bytes.length));
            const namePtr = _malloc(Math.max(1, nameBytes.length));

            if (bytes.length)
                HEAPU8.set(bytes, dataPtr);
            if (nameBytes.length)
                HEAPU8.set(nameBytes, namePtr);

            _wasmidi_browser_file_selected(
                dataPtr,
                bytes.length,
                namePtr,
                nameBytes.length
            );

            _free(dataPtr);
            _free(namePtr);
        } catch (error) {
            console.error('[WASMIDI] Could not read selected MIDI file:', error);
        }

        input.value = '';
    };

    input.click();
});
#endif

MainWindow::MainWindow(QObject *parent)
    : QObject(parent)
{
    g_browserMainWindow = this;

    // Same 16-slot palette used by MPWGL2.html.
    static const QColor defaults[16] = {
        QColor("#818cf8"), QColor("#a78bfa"), QColor("#c4b5fd"), QColor("#fb923c"),
        QColor("#4ade80"), QColor("#38bdf8"), QColor("#f472b6"), QColor("#facc15"),
        QColor("#f87171"), QColor("#34d399"), QColor("#60a5fa"), QColor("#e879f9"),
        QColor("#fb7185"), QColor("#a3e635"), QColor("#22d3ee"), QColor("#fbbf24")
    };

    channelColors_.reserve(16);
    for (const auto& color : defaults)
        channelColors_.push_back(color);

    // MPWGL2 drives visual/player time from requestAnimationFrame. 16 ms is
    // the Qt/WASM equivalent: the wall clock remains authoritative and this
    // timer only publishes the latest position to QML/GL.
    auto* frameTimer = new QTimer(this);
    frameTimer->setTimerType(Qt::PreciseTimer);
    connect(frameTimer, &QTimer::timeout, this, &MainWindow::updateCurrentTime);
    frameTimer->start(16);

    // The legacy MIDI scheduler runs every 5 ms with a 250 ms look-ahead.
    auto* schedulerTimer = new QTimer(this);
    schedulerTimer->setTimerType(Qt::PreciseTimer);
    connect(schedulerTimer, &QTimer::timeout, this, &MainWindow::dispatchScheduler);
    schedulerTimer->start(5);
}

MainWindow::~MainWindow()
{
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
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit loadFailed(QStringLiteral("Could not open %1").arg(localPath));
        return false;
    }

    return loadMidiFileNamed(file.readAll(), QFileInfo(localPath).fileName());
}

bool MainWindow::loadMidiFile(const QByteArray& data)
{
    return loadMidiFileNamed(data, fileName_);
}

bool MainWindow::loadMidiFileNamed(const QByteArray& data, const QString& fileName)
{
    wasmidi::MidiDocument parsed;
    if (!parser_.parse(reinterpret_cast<const uint8_t*>(data.constData()),
                       static_cast<std::size_t>(data.size()), parsed)) {
        emit loadFailed(QString::fromUtf8(parser_.error()));
        return false;
    }

    stop();
    document_ = std::move(parsed);
    scheduler_.setDocument(&document_);

    if (!fileName.isEmpty()) {
        fileName_ = fileName;
        emit fileNameChanged();
    }

    publishDocumentMetadata();
    rebuildDerivedStats();

    ++documentRevision_;
    emit documentRevisionChanged();
    emit fileLoaded();
    return true;
}

void MainWindow::openMidiPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_open_midi_picker();
#else
    emit loadFailed(QStringLiteral("The browser MIDI picker is available in the WebAssembly build."));
#endif
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

    noteStarts_.clear();
    noteEnds_.clear();
    controlTimes_.clear();
    tempoTimes_.clear();
    tempoBpms_.clear();
    for (auto& v : pitchStarts_) v.clear();
    for (auto& v : pitchEnds_) v.clear();

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
    emit documentRevisionChanged();
}

void MainWindow::publishDocumentMetadata()
{
    duration_ = document_.durationSeconds;
    noteCount_ = static_cast<int>(document_.notes.size());
    trackCount_ = document_.trackCount;
    midiFormat_ = document_.format;
    ppq_ = document_.ticksPerBeat;
    tempoChangeCount_ = static_cast<int>(document_.tempoMap.size());
    controlEventCount_ = static_cast<int>(document_.controls.size());

    uint32_t channelMask = 0;
    for (const auto mask : document_.activeChannelMasks)
        channelMask |= mask;

    activeChannelCount_ = 0;
    for (int channel = 0; channel < 16; ++channel)
        activeChannelCount_ += (channelMask & (1u << channel)) ? 1 : 0;

    if (!document_.notes.empty()) {
        int minPitch = 127;
        int maxPitch = 0;
        for (const auto& note : document_.notes) {
            minPitch = std::min(minPitch, static_cast<int>(note.pitch));
            maxPitch = std::max(maxPitch, static_cast<int>(note.pitch));
        }
        pitchRange_ = QStringLiteral("%1–%2").arg(minPitch).arg(maxPitch);
    } else {
        pitchRange_ = QStringLiteral("—");
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
    noteStarts_.clear();
    noteEnds_.clear();
    controlTimes_.clear();
    tempoTimes_.clear();
    tempoBpms_.clear();
    npsTimeline_.clear();
    for (auto& v : pitchStarts_) v.clear();
    for (auto& v : pitchEnds_) v.clear();

    peakNps_ = 0;
    peakNpsTime_ = 0.0f;
    peakPolyphony_ = 0;

    noteStarts_.reserve(document_.notes.size());
    noteEnds_.reserve(document_.notes.size());
    controlTimes_.reserve(document_.controls.size());

    for (const auto& note : document_.notes) {
        noteStarts_.push_back(note.startTime);
        noteEnds_.push_back(note.endTime);
        pitchStarts_[note.pitch].push_back(note.startTime);
        pitchEnds_[note.pitch].push_back(note.endTime);
    }

    for (const auto& event : document_.controls)
        controlTimes_.push_back(event.time);

    std::sort(noteStarts_.begin(), noteStarts_.end());
    std::sort(noteEnds_.begin(), noteEnds_.end());
    std::sort(controlTimes_.begin(), controlTimes_.end());
    for (auto& v : pitchStarts_) std::sort(v.begin(), v.end());
    for (auto& v : pitchEnds_) std::sort(v.begin(), v.end());

    // Build the same seconds-domain tempo index used by MPWGL2.
    if (!document_.tempoMap.empty()) {
        double seconds = 0.0;
        uint32_t previousTick = 0;
        uint32_t previousUsPerBeat = 500000;
        const double ppq = std::max(1, static_cast<int>(document_.ticksPerBeat));

        tempoTimes_.reserve(document_.tempoMap.size());
        tempoBpms_.reserve(document_.tempoMap.size());

        for (const auto& tempo : document_.tempoMap) {
            seconds += (static_cast<double>(tempo.tick - previousTick) / ppq) *
                       (static_cast<double>(previousUsPerBeat) / 1000000.0);
            tempoTimes_.push_back(static_cast<float>(seconds));
            tempoBpms_.push_back(tempo.microsecondsPerBeat != 0
                ? 60000000.0f / static_cast<float>(tempo.microsecondsPerBeat)
                : 120.0f);
            previousTick = tempo.tick;
            previousUsPerBeat = tempo.microsecondsPerBeat != 0
                ? tempo.microsecondsPerBeat
                : previousUsPerBeat;
        }
    }

    if (tempoBpms_.empty()) {
        tempoTimes_.push_back(0.0f);
        tempoBpms_.push_back(120.0f);
    }
    bpm_ = tempoBpms_.front();

    // Legacy computePeaks(): one-second window sampled every 100 ms.
    if (!noteStarts_.empty()) {
        for (float t = 0.0f; t <= duration_ + 0.0001f; t += 0.1f) {
            const auto lo = std::lower_bound(noteStarts_.begin(), noteStarts_.end(), t - 1.0f);
            const auto hi = std::upper_bound(noteStarts_.begin(), noteStarts_.end(), t);
            const int count = static_cast<int>(hi - lo);
            if (count > peakNps_) {
                peakNps_ = count;
                peakNpsTime_ = t;
            }
        }
    }

    // MPWGL2 limits/NPS timeline: half-second buckets across the whole file.
    if (duration_ > 0.0f) {
        const float step = 0.5f;
        const int buckets = static_cast<int>(std::ceil(duration_ / step)) + 1;
        npsTimeline_.reserve(buckets);
        for (int bucket = 0; bucket < buckets; ++bucket) {
            const float t = (bucket + 1) * step;
            const auto lo = std::lower_bound(noteStarts_.begin(), noteStarts_.end(), t - step);
            const auto hi = std::upper_bound(noteStarts_.begin(), noteStarts_.end(), t);
            npsTimeline_.push_back(static_cast<int>(hi - lo));
        }
    }

    // Legacy peak-polyphony calculation samples at most ~400k notes on huge
    // Black MIDI files so loading the UI cannot be blocked indefinitely.
    struct Edge { float time; int delta; };
    constexpr std::size_t MaxPeakNotes = 400000;
    const std::size_t stride = document_.notes.size() > MaxPeakNotes
        ? static_cast<std::size_t>(std::ceil(
            static_cast<double>(document_.notes.size()) / MaxPeakNotes))
        : 1;

    std::vector<Edge> edges;
    edges.reserve((document_.notes.size() / stride + 1) * 2);
    for (std::size_t i = 0; i < document_.notes.size(); i += stride) {
        const auto& note = document_.notes[i];
        edges.push_back({note.startTime, +1});
        edges.push_back({note.endTime, -1});
    }

    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.time != b.time)
            return a.time < b.time;
        // MPWGL2's dirs[b]-dirs[a]: note-on before note-off at equal time.
        return a.delta > b.delta;
    });

    int active = 0;
    for (const auto& edge : edges) {
        active += edge.delta;
        peakPolyphony_ = std::max(peakPolyphony_, active);
    }

    emit bpmChanged();
    emit peakNpsChanged();
    emit peakPolyphonyChanged();
    emit timelineChanged();
    updateLiveStats();
}

void MainWindow::play()
{
    if (document_.notes.empty())
        return;

    if (currentTime_ >= duration_)
        currentTime_ = 0.0f;

    // Equivalent to: startedAt = performance.now() - currentTime * 1000.
    playbackAnchorSeconds_ = currentTime_;
    playbackClock_.restart();
    scheduler_.seek(currentTime_);
    scheduler_.start();
    setPlaying(true);
}

void MainWindow::pause()
{
    if (!isPlaying_)
        return;

    updateCurrentTime();
    scheduler_.pause();
    setPlaying(false);
}

void MainWindow::stop()
{
    scheduler_.stop();
    setPlaying(false);
    playbackAnchorSeconds_ = 0.0f;
    playbackClock_.invalidate();

    if (currentTime_ != 0.0f) {
        currentTime_ = 0.0f;
        emit currentTimeChanged();
    }

    // MPWGL2 explicitly resets live note/NPS counters on Stop.
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

    if (!tempoBpms_.empty() && !qFuzzyCompare(bpm_, tempoBpms_.front())) {
        bpm_ = tempoBpms_.front();
        emit bpmChanged();
    }
}

void MainWindow::seek(float seconds)
{
    const float clamped = std::clamp(seconds, 0.0f, duration_);
    currentTime_ = clamped;
    playbackAnchorSeconds_ = clamped;

    if (isPlaying_)
        playbackClock_.restart();

    scheduler_.seek(clamped);
    emit currentTimeChanged();
    updateLiveStats();
}

void MainWindow::setNoteSpeed(float speed)
{
    const float clamped = std::clamp(speed, 0.1f, 60.0f);
    if (qFuzzyCompare(noteSpeed_, clamped))
        return;
    noteSpeed_ = clamped;
    emit noteSpeedChanged();
}

void MainWindow::setPostBuffer(float buffer)
{
    const float clamped = std::clamp(buffer, 0.0f, 10.0f);
    const bool autoWasEnabled = postBufferAuto_;
    postBufferAuto_ = false;
    if (autoWasEnabled)
        emit postBufferAutoChanged();
    if (qFuzzyCompare(postBuffer_, clamped))
        return;
    postBuffer_ = clamped;
    emit postBufferChanged();
}

void MainWindow::setPostBufferAuto()
{
    const bool changed = !postBufferAuto_;
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
    emit perTrackColorsChanged();
}

void MainWindow::setVolume(int value)
{
    const int clamped = std::clamp(value, 0, 100);
    if (volume_ == clamped)
        return;
    volume_ = clamped;
    emit volumeChanged();
}

void MainWindow::setOutputMode(const QString& mode)
{
    QString normalized = mode.toLower();
    if (normalized != QStringLiteral("native") &&
        normalized != QStringLiteral("input") &&
        normalized != QStringLiteral("off") &&
        normalized != QStringLiteral("embedded")) {
        normalized = QStringLiteral("off");
    }
    if (outputMode_ == normalized)
        return;
    outputMode_ = normalized;
    emit outputModeChanged();
}

void MainWindow::setChannelColor(int channel, const QColor& color)
{
    if (channel < 0 || channel >= channelColors_.size() || !color.isValid())
        return;
    if (channelColors_[channel] == color)
        return;
    channelColors_[channel] = color;
    emit channelColorsChanged();
}

void MainWindow::updateLiveStats()
{
    if (document_.notes.empty()) {
        if (nps_ != 0) { nps_ = 0; emit npsChanged(); }
        if (activeVoices_ != 0) { activeVoices_ = 0; emit activeVoicesChanged(); }
        if (ccPerSecond_ != 0) { ccPerSecond_ = 0; emit ccPerSecondChanged(); }
        return;
    }

    const float t = currentTime_;

    // Exact MPWGL2 live NPS formula: count starts in the last 250 ms and
    // multiply by four. This is why the legacy counter reacts immediately.
    const auto npsLo = std::lower_bound(noteStarts_.begin(), noteStarts_.end(), t - 0.25f);
    const auto npsHi = std::upper_bound(noteStarts_.begin(), noteStarts_.end(), t);
    const int newNps = static_cast<int>(std::lround((npsHi - npsLo) * 4.0));

    // Same active-polyphony formula as MPWGL2:
    // upperBound(startArr,t) - lowerBound(endArr,t).
    //
    // IMPORTANT: do not subtract iterators from noteStarts_ and noteEnds_
    // directly. They belong to different vectors; doing so is undefined
    // behavior and produced huge ACTIVE values in the WASM build.
    const auto startedIt =
        std::upper_bound(noteStarts_.begin(), noteStarts_.end(), t);
    const auto endedIt =
        std::lower_bound(noteEnds_.begin(), noteEnds_.end(), t);

    const std::ptrdiff_t startedCount =
        startedIt - noteStarts_.begin();
    const std::ptrdiff_t endedCount =
        endedIt - noteEnds_.begin();

    const int newActive = std::max(
        0,
        static_cast<int>(startedCount - endedCount));

    // CC/s is a moving one-second window, not a coarse per-second bucket.
    const auto ccLo = std::lower_bound(controlTimes_.begin(), controlTimes_.end(), t - 1.0f);
    const auto ccHi = std::upper_bound(controlTimes_.begin(), controlTimes_.end(), t);
    const int newCc = static_cast<int>(ccHi - ccLo);

    float newBpm = 120.0f;
    if (!tempoTimes_.empty()) {
        auto it = std::upper_bound(tempoTimes_.begin(), tempoTimes_.end(), t);
        std::size_t index = 0;
        if (it != tempoTimes_.begin())
            index = static_cast<std::size_t>((it - tempoTimes_.begin()) - 1);
        index = std::min(index, tempoBpms_.size() - 1);
        newBpm = tempoBpms_[index];
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
    if (!qFuzzyCompare(bpm_, newBpm)) {
        bpm_ = newBpm;
        emit bpmChanged();
    }
}

void MainWindow::updateCurrentTime()
{
    if (!isPlaying_)
        return;

    currentTime_ = playbackAnchorSeconds_ +
        static_cast<float>(playbackClock_.elapsed()) / 1000.0f;

    if (currentTime_ >= duration_) {
        // MPWGL2 calls stopPlayback() at EOF, which returns the transport to 0.
        stop();
        return;
    }

    emit currentTimeChanged();
    updateLiveStats();
}

void MainWindow::dispatchScheduler()
{
    if (!isPlaying_ || document_.notes.empty())
        return;

    // Preserve the exact legacy cadence and look-ahead. The returned events
    // are the native hand-off point for Web MIDI / SnappySynth in the next
    // audio-driver port; importantly, scheduler time no longer advances by
    // 250 ms on every 5 ms callback.
    const auto& scheduled = scheduler_.getEventsForWindow(currentTime_, 0.25f, 0.05f);
    (void)scheduled;
}

std::array<uint8_t, 128> MainWindow::activePitchMask() const
{
    std::array<uint8_t, 128> mask{};

    for (std::size_t pitch = 0; pitch < mask.size(); ++pitch) {
        const auto& starts = pitchStarts_[pitch];
        const auto& ends = pitchEnds_[pitch];
        const auto started = std::upper_bound(starts.begin(), starts.end(), currentTime_ + 0.05f) - starts.begin();
        const auto ended = std::lower_bound(ends.begin(), ends.end(), currentTime_) - ends.begin();
        mask[pitch] = started > ended ? 1 : 0;
    }

    return mask;
}
