#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QColor>
#include <QElapsedTimer>
#include <QUrl>
#include <QVariantList>
#include <QVector>

#include <vector>
#include <array>

#include "midi/midi_parser.hpp"
#include "midi/scheduler.hpp"

class MainWindow : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(PlayerController)

    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool hasMidi READ hasMidi NOTIFY noteCountChanged)
    Q_PROPERTY(float currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(float duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileNameChanged)
    Q_PROPERTY(int noteCount READ noteCount NOTIFY noteCountChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY trackCountChanged)
    Q_PROPERTY(int activeVoices READ activeVoices NOTIFY activeVoicesChanged)
    Q_PROPERTY(int activeChannelCount READ activeChannelCount NOTIFY activeChannelCountChanged)
    Q_PROPERTY(int nps READ nps NOTIFY npsChanged)
    Q_PROPERTY(int ccPerSecond READ ccPerSecond NOTIFY ccPerSecondChanged)
    Q_PROPERTY(float bpm READ bpm NOTIFY bpmChanged)
    Q_PROPERTY(float noteSpeed READ noteSpeed WRITE setNoteSpeed NOTIFY noteSpeedChanged)
    Q_PROPERTY(float postBuffer READ postBuffer WRITE setPostBuffer NOTIFY postBufferChanged)
    Q_PROPERTY(bool postBufferAuto READ postBufferAuto NOTIFY postBufferAutoChanged)
    Q_PROPERTY(bool perTrackColors READ perTrackColors WRITE setPerTrackColors NOTIFY perTrackColorsChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(int midiFormat READ midiFormat NOTIFY midiFormatChanged)
    Q_PROPERTY(int ppq READ ppq NOTIFY ppqChanged)
    Q_PROPERTY(int tempoChangeCount READ tempoChangeCount NOTIFY tempoChangeCountChanged)
    Q_PROPERTY(int controlEventCount READ controlEventCount NOTIFY controlEventCountChanged)
    Q_PROPERTY(int peakNps READ peakNps NOTIFY peakNpsChanged)
    Q_PROPERTY(int peakPolyphony READ peakPolyphony NOTIFY peakPolyphonyChanged)
    Q_PROPERTY(QString pitchRange READ pitchRange NOTIFY pitchRangeChanged)
    Q_PROPERTY(int skippedVelocity READ skippedVelocity NOTIFY skippedVelocityChanged)
    Q_PROPERTY(quint64 documentRevision READ documentRevision NOTIFY documentRevisionChanged)
    Q_PROPERTY(QString outputMode READ outputMode WRITE setOutputMode NOTIFY outputModeChanged)
    Q_PROPERTY(QVariantList channelColorList READ channelColorList NOTIFY channelColorsChanged)

public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow() override;

    bool isPlaying() const { return isPlaying_; }
    bool hasMidi() const { return noteCount_ > 0; }
    float currentTime() const { return currentTime_; }
    float duration() const { return duration_; }
    QString fileName() const { return fileName_; }
    int noteCount() const { return noteCount_; }
    int trackCount() const { return trackCount_; }
    int activeVoices() const { return activeVoices_; }
    int activeChannelCount() const { return activeChannelCount_; }
    int nps() const { return nps_; }
    int ccPerSecond() const { return ccPerSecond_; }
    float bpm() const { return bpm_; }
    float noteSpeed() const { return noteSpeed_; }
    float postBuffer() const { return postBuffer_; }
    bool postBufferAuto() const { return postBufferAuto_; }
    bool perTrackColors() const { return perTrackColors_; }
    int volume() const { return volume_; }
    int midiFormat() const { return midiFormat_; }
    int ppq() const { return ppq_; }
    int tempoChangeCount() const { return tempoChangeCount_; }
    int controlEventCount() const { return controlEventCount_; }
    int peakNps() const { return peakNps_; }
    int peakPolyphony() const { return peakPolyphony_; }
    QString pitchRange() const { return pitchRange_; }
    int skippedVelocity() const { return skippedVelocity_; }
    quint64 documentRevision() const { return documentRevision_; }
    QString outputMode() const { return outputMode_; }
    QVariantList channelColorList() const;

    const wasmidi::MidiDocument& document() const { return document_; }
    const QVector<QColor>& channelColors() const { return channelColors_; }
    std::array<uint8_t, 128> activePitchMask() const;

    Q_INVOKABLE bool loadMidiFile(const QByteArray& data);
    Q_INVOKABLE bool loadMidiUrl(const QUrl& url);
    Q_INVOKABLE void clearFile();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(float seconds);
    Q_INVOKABLE void setPostBufferAuto();
    Q_INVOKABLE void setChannelColor(int channel, const QColor& color);

    void setNoteSpeed(float speed);
    void setPostBuffer(float buffer);
    void setPerTrackColors(bool enable);
    void setVolume(int value);
    void setOutputMode(const QString& mode);

signals:
    void playingChanged();
    void currentTimeChanged();
    void durationChanged();
    void fileNameChanged();
    void noteCountChanged();
    void trackCountChanged();
    void activeVoicesChanged();
    void activeChannelCountChanged();
    void npsChanged();
    void ccPerSecondChanged();
    void bpmChanged();
    void noteSpeedChanged();
    void postBufferChanged();
    void postBufferAutoChanged();
    void perTrackColorsChanged();
    void volumeChanged();
    void midiFormatChanged();
    void ppqChanged();
    void tempoChangeCountChanged();
    void controlEventCountChanged();
    void peakNpsChanged();
    void peakPolyphonyChanged();
    void pitchRangeChanged();
    void skippedVelocityChanged();
    void documentRevisionChanged();
    void outputModeChanged();
    void channelColorsChanged();
    void fileLoaded();
    void loadFailed(QString message);

private:
    void setPlaying(bool playing);
    void rebuildDerivedStats();
    void updateLiveStats();
    void updateCurrentTime();
    void publishDocumentMetadata();

    wasmidi::MidiParser parser_;
    wasmidi::MidiScheduler scheduler_;
    wasmidi::MidiDocument document_;

    bool isPlaying_ = false;
    float currentTime_ = 0.0f;
    float duration_ = 0.0f;
    QString fileName_;
    int noteCount_ = 0;
    int trackCount_ = 0;
    int activeVoices_ = 0;
    int activeChannelCount_ = 0;
    int nps_ = 0;
    int ccPerSecond_ = 0;
    float bpm_ = 120.0f;
    float noteSpeed_ = 1.0f;
    float postBuffer_ = 0.0f;
    bool postBufferAuto_ = true;
    bool perTrackColors_ = false;
    int volume_ = 80;
    int midiFormat_ = 0;
    int ppq_ = 480;
    int tempoChangeCount_ = 0;
    int controlEventCount_ = 0;
    int peakNps_ = 0;
    int peakPolyphony_ = 0;
    QString pitchRange_ = QStringLiteral("—");
    int skippedVelocity_ = 0;
    quint64 documentRevision_ = 0;
    QString outputMode_ = QStringLiteral("native");

    QVector<QColor> channelColors_;
    QElapsedTimer playbackClock_;
    float playbackAnchorSeconds_ = 0.0f;

    std::vector<float> noteStarts_;
    std::vector<float> noteEnds_;
    std::vector<int> npsBuckets_;
    std::vector<int> ccBuckets_;
    std::array<std::vector<float>, 128> pitchStarts_;
    std::array<std::vector<float>, 128> pitchEnds_;
};
