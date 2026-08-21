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

    auto* schedulerTimer = new QTimer(this);
    schedulerTimer->setTimerType(Qt::PreciseTimer);

    connect(
        schedulerTimer,
        &QTimer::timeout,
        this,
        &MainWindow::dispatchScheduler);

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

void MainWindow::clearVisualState()
{
    visualStateCount_.fill(0);
    visualStateColor_.fill(0);
    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    visualColorVoices_.fill(0);

    visualActiveVoices_ = 0;
    visualTick_ = 0;
    visualGroupCursor_ = 0;
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

    // Exact one-second peak-NPS using a sliding TickGroup window.
    std::size_t left = 0;
    uint64_t rollingNotes = 0;

    for (std::size_t right = 0;
         right < document_.tickGroups.size();
         ++right) {
        const auto& group =
            document_.tickGroups[right];

        rollingNotes +=
            group.noteOnCount;

        const double rightSeconds =
            document_.tickToSeconds(
                group.tick);

        while (left <= right) {
            const double leftSeconds =
                document_.tickToSeconds(
                    document_.tickGroups[left].tick);

            if (leftSeconds >=
                rightSeconds - 1.0) {
                break;
            }

            rollingNotes -=
                document_.tickGroups[left]
                    .noteOnCount;

            ++left;
        }

        if (rollingNotes >
            static_cast<uint64_t>(
                peakNps_)) {
            peakNps_ =
                static_cast<int>(
                    std::min<uint64_t>(
                        rollingNotes,
                        uint64_t(
                            std::numeric_limits<int>::max())));

            peakNpsTime_ =
                static_cast<float>(
                    rightSeconds);
        }
    }

    // Exact peak polyphony from the compact event stream. No 2x Edge vector,
    // no global note sort and no sampling.
    std::array<uint32_t, 16 * 128>
        activeStates{};

    int64_t active = 0;
    int64_t peak = 0;

    for (const auto& group :
         document_.tickGroups) {
        const std::size_t begin =
            group.eventOffset;

        const std::size_t end =
            begin + group.eventCount;

        // Match the old dirs[b]-dirs[a] edge ordering: all NoteOns at this
        // exact tick contribute before any NoteOff at the same tick.
        for (std::size_t i = begin;
             i < end;
             ++i) {
            const auto& event =
                document_.events[i];

            if ((event.status & 0xf0) != 0x90 ||
                event.data2 == 0) {
                continue;
            }

            const std::size_t state =
                std::size_t(
                    event.status & 0x0f) *
                128u +
                std::size_t(
                    event.data1 & 0x7f);

            ++activeStates[state];
            ++active;
            peak = std::max(peak, active);
        }

        for (std::size_t i = begin;
             i < end;
             ++i) {
            const auto& event =
                document_.events[i];

            if ((event.status & 0xf0) != 0x80)
                continue;

            const std::size_t state =
                std::size_t(
                    event.status & 0x0f) *
                128u +
                std::size_t(
                    event.data1 & 0x7f);

            if (activeStates[state] != 0) {
                --activeStates[state];
                --active;
            }
        }
    }

    peakPolyphony_ =
        static_cast<int>(
            std::min<int64_t>(
                peak,
                std::numeric_limits<int>::max()));

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

    playbackAnchorSeconds_ =
        currentTime_;

    playbackClock_.restart();

    scheduler_.seek(
        currentTime_);

    scheduler_.start();

    invalidateLiveTrackers();
    visualStateValid_ = false;

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
        !qFuzzyCompare(
            bpm_,
            tempoBpms_.front())) {
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
        std::clamp(
            seconds,
            0.0f,
            duration_);

    currentTime_ = clamped;
    playbackAnchorSeconds_ = clamped;

    if (isPlaying_)
        playbackClock_.restart();

    scheduler_.seek(clamped);

    invalidateLiveTrackers();
    visualStateValid_ = false;

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
}

void MainWindow::setOutputMode(const QString& mode)
{
    QString normalized =
        mode.toLower();

    if (normalized != QStringLiteral("native") &&
        normalized != QStringLiteral("input") &&
        normalized != QStringLiteral("off") &&
        normalized != QStringLiteral("embedded")) {
        normalized =
            QStringLiteral("off");
    }

    if (outputMode_ == normalized)
        return;

    outputMode_ = normalized;
    emit outputModeChanged();
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

void MainWindow::processVisualEvent(
    const wasmidi::CompactEvent& event,
    uint32_t /*tick*/)
{
    const uint8_t command =
        event.status & 0xf0;

    if (command != 0x90 &&
        command != 0x80) {
        return;
    }

    const uint8_t channel =
        event.status & 0x0f;

    const uint8_t pitch =
        event.data1 & 0x7f;

    const std::size_t state =
        std::size_t(channel) *
        128u +
        std::size_t(pitch);

    const uint8_t color =
        document_.colorIndex(
            event,
            perTrackColors_);

    if (command == 0x90 &&
        event.data2 != 0) {
        ++visualStateCount_[state];
        visualStateColor_[state] = color;

        ++visualPitchCount_[pitch];
        visualPitchMask_[pitch] = 1;
        visualPitchColor_[pitch] =
            static_cast<int8_t>(color);

        ++visualColorVoices_[color];
        ++visualActiveVoices_;
        return;
    }

    if (visualStateCount_[state] == 0)
        return;

    const uint8_t soundingColor =
        visualStateColor_[state] & 0x0f;

    --visualStateCount_[state];

    if (visualPitchCount_[pitch] != 0)
        --visualPitchCount_[pitch];

    if (visualColorVoices_[soundingColor] != 0)
        --visualColorVoices_[soundingColor];

    visualActiveVoices_ =
        std::max(
            0,
            visualActiveVoices_ - 1);

    if (visualPitchCount_[pitch] == 0) {
        visualPitchMask_[pitch] = 0;
        visualPitchColor_[pitch] = -1;
        return;
    }

    if (visualPitchColor_[pitch] ==
        static_cast<int8_t>(soundingColor)) {
        // Pick another currently sounding channel for this pitch.
        for (int ch = 0; ch < 16; ++ch) {
            const std::size_t other =
                std::size_t(ch) * 128u +
                std::size_t(pitch);

            if (visualStateCount_[other] != 0) {
                visualPitchColor_[pitch] =
                    static_cast<int8_t>(
                        visualStateColor_[other]);
                break;
            }
        }
    }
}

void MainWindow::syncVisualState(
    uint32_t targetTick,
    bool forceRebuild)
{
    if (document_.tickGroups.empty()) {
        clearVisualState();
        return;
    }

    const auto oldMask =
        visualPitchMask_;

    const auto oldColors =
        visualPitchColor_;

    const bool backwards =
        visualStateValid_ &&
        targetTick < visualTick_;

    if (forceRebuild ||
        !visualStateValid_ ||
        backwards) {
        visualStateCount_.fill(0);
        visualStateColor_.fill(0);
        visualPitchCount_.fill(0);
        visualPitchMask_.fill(0);
        visualPitchColor_.fill(-1);
        visualColorVoices_.fill(0);
        visualActiveVoices_ = 0;

        // Bounded reconstruction on seeks: mirrors SharpMIDI's renderer
        // strategy and prevents a seek near the end of a 100M-event file from
        // replaying the whole MIDI on the UI thread.
        const double lookbackSeconds =
            std::max(
                30.0,
                double(noteSpeed_) * 4.0 +
                double(postBuffer_));

        const double startSeconds =
            std::max(
                0.0,
                double(currentTime_) -
                lookbackSeconds);

        const uint32_t startTick =
            static_cast<uint32_t>(
                std::max(
                    0.0,
                    std::floor(
                        document_.secondsToTick(
                            startSeconds))));

        visualGroupCursor_ =
            document_.lowerBoundGroup(
                startTick);

        visualTick_ = startTick;
        visualStateValid_ = true;
    }

    while (visualGroupCursor_ <
           document_.tickGroups.size()) {
        const auto& group =
            document_.tickGroups[
                visualGroupCursor_];

        if (group.tick > targetTick)
            break;

        const std::size_t begin =
            group.eventOffset;

        const std::size_t end =
            begin + group.eventCount;

        for (std::size_t i = begin;
             i < end;
             ++i) {
            processVisualEvent(
                document_.events[i],
                group.tick);
        }

        ++visualGroupCursor_;
    }

    visualTick_ = targetTick;

    if (oldMask != visualPitchMask_ ||
        oldColors != visualPitchColor_) {
        emit activePitchesChanged();
    }
}

void MainWindow::updateNeuralVisuals()
{
    float newHue = dominantHue_;

    std::array<uint64_t, 16> frequency{};

    for (int i = 0; i < 16; ++i)
        frequency[i] = visualColorVoices_[i];

    // Preserve MPWGL2's slight look-ahead for background color reaction.
    if (!document_.tickGroups.empty()) {
        const double futureSeconds =
            std::min<double>(
                duration_,
                double(currentTime_) +
                0.15);

        const uint32_t futureTick =
            static_cast<uint32_t>(
                std::min<double>(
                    document_.maxTick,
                    std::ceil(
                        document_.secondsToTick(
                            futureSeconds))));

        const std::size_t futureEnd =
            document_.upperBoundGroup(
                futureTick);

        for (std::size_t groupIndex =
                 visualGroupCursor_;
             groupIndex < futureEnd;
             ++groupIndex) {
            const auto& group =
                document_.tickGroups[groupIndex];

            const std::size_t begin =
                group.eventOffset;

            const std::size_t end =
                begin + group.eventCount;

            for (std::size_t i = begin;
                 i < end;
                 ++i) {
                const auto& event =
                    document_.events[i];

                if ((event.status & 0xf0) ==
                        0x90 &&
                    event.data2 != 0) {
                    ++frequency[
                        document_.colorIndex(
                            event,
                            perTrackColors_)];
                }
            }
        }
    }

    int bestColor = -1;
    uint64_t bestCount = 0;

    for (int color = 0;
         color < 16;
         ++color) {
        if (frequency[color] >
            bestCount) {
            bestCount =
                frequency[color];

            bestColor = color;
        }
    }

    if (bestColor >= 0 &&
        bestColor <
            channelColors_.size()) {
        newHue =
            colorHue(
                channelColors_[
                    bestColor]);
    }

    const float activity =
        peakNps_ > 0
            ? std::min(
                1.0f,
                float(nps_) /
                float(peakNps_))
            : 0.0f;

    const float newActivity =
        activity * 0.7f +
        neuralActivity_ * 0.3f;

    if (!qFuzzyCompare(
            newHue,
            dominantHue_) ||
        !qFuzzyCompare(
            newActivity,
            neuralActivity_)) {
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

    const uint32_t npsStartTick =
        static_cast<uint32_t>(
            std::max(
                0.0,
                std::floor(
                    document_.secondsToTick(
                        std::max(
                            0.0,
                            time - 0.25)))));

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

    const std::size_t targetNpsLo =
        document_.lowerBoundGroup(
            npsStartTick);

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

        for (std::size_t i = targetNpsLo;
             i < targetHi;
             ++i) {
            liveNpsCount_ +=
                document_.tickGroups[i]
                    .noteOnCount;
        }

        for (std::size_t i = targetCcLo;
             i < targetHi;
             ++i) {
            liveCcCount_ +=
                document_.tickGroups[i]
                    .controlCount;
        }

        liveHi_ = targetHi;
        liveNpsLo_ = targetNpsLo;
        liveCcLo_ = targetCcLo;
        liveWindowsValid_ = true;
    } else {
        while (liveHi_ < targetHi) {
            const auto& group =
                document_.tickGroups[liveHi_];

            liveNpsCount_ +=
                group.noteOnCount;

            liveCcCount_ +=
                group.controlCount;

            ++liveHi_;
        }

        while (liveNpsLo_ < targetNpsLo &&
               liveNpsLo_ < liveHi_) {
            liveNpsCount_ -=
                document_.tickGroups[
                    liveNpsLo_]
                    .noteOnCount;

            ++liveNpsLo_;
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

    syncVisualState(
        currentTick);

    const int newNps =
        static_cast<int>(
            std::min<uint64_t>(
                liveNpsCount_ * 4u,
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

    currentTime_ =
        playbackAnchorSeconds_ +
        static_cast<float>(
            playbackClock_.elapsed()) /
        1000.0f;

    if (currentTime_ >= duration_) {
        stop();
        return;
    }

    emit currentTimeChanged();
    updateLiveStats();
}

void MainWindow::dispatchScheduler()
{
    if (!isPlaying_ ||
        !hasMidi()) {
        return;
    }

    const auto& scheduled =
        scheduler_.getEventsForWindow(
            currentTime_,
            0.25f,
            0.05f);

    // Native hand-off point for the Web MIDI / embedded synth driver.
    (void)scheduled;
}
