#include "mainwindow.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTimer>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cmath>


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

        // Allow selecting the same file again.
        input.value = '';
    };

    // Because this is invoked directly from the QML click handler, the
    // browser keeps the user-activation token and opens the native OS picker.
    input.click();
});
#endif

MainWindow::MainWindow(QObject *parent)
    : QObject(parent)
{
    g_browserMainWindow = this;
    static const QColor defaults[16] = {
        QColor("#818cf8"), QColor("#a78bfa"), QColor("#c084fc"), QColor("#e879f9"),
        QColor("#f472b6"), QColor("#fb7185"), QColor("#fb923c"), QColor("#facc15"),
        QColor("#a3e635"), QColor("#4ade80"), QColor("#34d399"), QColor("#2dd4bf"),
        QColor("#22d3ee"), QColor("#38bdf8"), QColor("#60a5fa"), QColor("#8b5cf6")
    };
    channelColors_.reserve(16);
    for (const auto& color : defaults)
        channelColors_.push_back(color);

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::updateCurrentTime);
    timer->start(16);
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
    peakPolyphony_ = 0;
    pitchRange_ = QStringLiteral("—");
    skippedVelocity_ = 0;

    noteStarts_.clear();
    noteEnds_.clear();
    npsBuckets_.clear();
    ccBuckets_.clear();
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
        uint8_t minPitch = 127;
        uint8_t maxPitch = 0;
        for (const auto& note : document_.notes) {
            minPitch = std::min(minPitch, note.pitch);
            maxPitch = std::max(maxPitch, note.pitch);
        }
        pitchRange_ = QStringLiteral("%1–%2").arg(minPitch).arg(maxPitch);
    } else {
        pitchRange_ = QStringLiteral("—");
    }

    if (!document_.tempoMap.empty() && document_.tempoMap.front().microsecondsPerBeat != 0)
        bpm_ = 60000000.0f / static_cast<float>(document_.tempoMap.front().microsecondsPerBeat);
    else
        bpm_ = 120.0f;

    emit durationChanged();
    emit noteCountChanged();
    emit trackCountChanged();
    emit midiFormatChanged();
    emit ppqChanged();
    emit tempoChangeCountChanged();
    emit controlEventCountChanged();
    emit activeChannelCountChanged();
    emit pitchRangeChanged();
    emit bpmChanged();
}

void MainWindow::rebuildDerivedStats()
{
    noteStarts_.clear();
    noteEnds_.clear();
    npsBuckets_.clear();
    ccBuckets_.clear();
    for (auto& v : pitchStarts_) v.clear();
    for (auto& v : pitchEnds_) v.clear();
    peakNps_ = 0;
    peakPolyphony_ = 0;

    noteStarts_.reserve(document_.notes.size());
    noteEnds_.reserve(document_.notes.size());

    const auto bucketCount = static_cast<std::size_t>(std::max(1.0f, std::ceil(duration_) + 1.0f));
    npsBuckets_.assign(bucketCount, 0);
    ccBuckets_.assign(bucketCount, 0);

    struct Edge { float time; int delta; };
    std::vector<Edge> edges;
    edges.reserve(document_.notes.size() * 2);

    for (const auto& note : document_.notes) {
        noteStarts_.push_back(note.startTime);
        noteEnds_.push_back(note.endTime);
        pitchStarts_[note.pitch].push_back(note.startTime);
        pitchEnds_[note.pitch].push_back(note.endTime);
        const auto bucket = static_cast<std::size_t>(std::max(0.0f, std::floor(note.startTime)));
        if (bucket < npsBuckets_.size())
            peakNps_ = std::max(peakNps_, ++npsBuckets_[bucket]);
        edges.push_back({note.startTime, +1});
        edges.push_back({note.endTime, -1});
    }

    for (const auto& event : document_.controls) {
        const auto bucket = static_cast<std::size_t>(std::max(0.0f, std::floor(event.time)));
        if (bucket < ccBuckets_.size())
            ++ccBuckets_[bucket];
    }

    std::sort(noteStarts_.begin(), noteStarts_.end());
    std::sort(noteEnds_.begin(), noteEnds_.end());
    for (auto& v : pitchStarts_) std::sort(v.begin(), v.end());
    for (auto& v : pitchEnds_) std::sort(v.begin(), v.end());
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.time != b.time) return a.time < b.time;
        return a.delta < b.delta; // note-off before note-on at the same timestamp
    });

    int active = 0;
    for (const auto& edge : edges) {
        active += edge.delta;
        peakPolyphony_ = std::max(peakPolyphony_, active);
    }

    emit peakNpsChanged();
    emit peakPolyphonyChanged();
    updateLiveStats();
}

void MainWindow::play()
{
    if (document_.notes.empty())
        return;
    if (currentTime_ >= duration_)
        seek(0.0f);

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
    if (currentTime_ != 0.0f) {
        currentTime_ = 0.0f;
        emit currentTimeChanged();
    }
    updateLiveStats();
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

    const auto sec = static_cast<std::size_t>(std::max(0.0f, std::floor(currentTime_)));
    const int newNps = sec < npsBuckets_.size() ? npsBuckets_[sec] : 0;
    const int newCc = sec < ccBuckets_.size() ? ccBuckets_[sec] : 0;

    const auto started = std::upper_bound(noteStarts_.begin(), noteStarts_.end(), currentTime_) - noteStarts_.begin();
    const auto ended = std::lower_bound(noteEnds_.begin(), noteEnds_.end(), currentTime_) - noteEnds_.begin();
    const int newActive = static_cast<int>(started - ended);

    if (nps_ != newNps) {
        nps_ = newNps;
        emit npsChanged();
    }
    if (ccPerSecond_ != newCc) {
        ccPerSecond_ = newCc;
        emit ccPerSecondChanged();
    }
    if (activeVoices_ != newActive) {
        activeVoices_ = newActive;
        emit activeVoicesChanged();
    }
}

void MainWindow::updateCurrentTime()
{
    if (!isPlaying_)
        return;

    currentTime_ = playbackAnchorSeconds_ + static_cast<float>(playbackClock_.elapsed()) / 1000.0f;
    if (currentTime_ >= duration_) {
        currentTime_ = duration_;
        scheduler_.pause();
        setPlaying(false);
    }

    emit currentTimeChanged();
    updateLiveStats();
}

std::array<uint8_t, 128> MainWindow::activePitchMask() const
{
    std::array<uint8_t, 128> mask{};
    for (std::size_t pitch = 0; pitch < mask.size(); ++pitch) {
        const auto& starts = pitchStarts_[pitch];
        const auto& ends = pitchEnds_[pitch];
        const auto started = std::upper_bound(starts.begin(), starts.end(), currentTime_) - starts.begin();
        const auto ended = std::lower_bound(ends.begin(), ends.end(), currentTime_) - ends.begin();
        mask[pitch] = started > ended ? 1 : 0;
    }
    return mask;
}
