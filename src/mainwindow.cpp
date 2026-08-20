#include "mainwindow.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <algorithm>
#include <cmath>

MainWindow::MainWindow(QObject *parent)
    : QObject(parent)
{
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

MainWindow::~MainWindow() = default;

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

    fileName_ = QFileInfo(localPath).fileName();
    emit fileNameChanged();
    return loadMidiFile(file.readAll());
}

bool MainWindow::loadMidiFile(const QByteArray& data)
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

    publishDocumentMetadata();
    rebuildDerivedStats();

    ++documentRevision_;
    emit documentRevisionChanged();
    emit fileLoaded();
    return true;
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
    emit bpmChanged();
}

void MainWindow::rebuildDerivedStats()
{
    noteStarts_.clear();
    noteEnds_.clear();
    npsBuckets_.clear();
    for (auto& v : pitchStarts_) v.clear();
    for (auto& v : pitchEnds_) v.clear();
    peakNps_ = 0;
    peakPolyphony_ = 0;

    noteStarts_.reserve(document_.notes.size());
    noteEnds_.reserve(document_.notes.size());

    const auto bucketCount = static_cast<std::size_t>(std::max(1.0f, std::ceil(duration_) + 1.0f));
    npsBuckets_.assign(bucketCount, 0);

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
    if (qFuzzyCompare(postBuffer_, clamped))
        return;
    postBuffer_ = clamped;
    emit postBufferChanged();
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
    const QString normalized = mode == QStringLiteral("native") ? QStringLiteral("native") : QStringLiteral("embedded");
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
        return;
    }

    const auto sec = static_cast<std::size_t>(std::max(0.0f, std::floor(currentTime_)));
    const int newNps = sec < npsBuckets_.size() ? npsBuckets_[sec] : 0;

    const auto started = std::upper_bound(noteStarts_.begin(), noteStarts_.end(), currentTime_) - noteStarts_.begin();
    const auto ended = std::lower_bound(noteEnds_.begin(), noteEnds_.end(), currentTime_) - noteEnds_.begin();
    const int newActive = static_cast<int>(started - ended);

    if (nps_ != newNps) {
        nps_ = newNps;
        emit npsChanged();
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
