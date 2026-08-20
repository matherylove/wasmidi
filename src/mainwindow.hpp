#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QProperty>
#include <QVector>
#include <QColor>

#include "midi/midi_parser.hpp"
#include "midi/scheduler.hpp"

class MainWindow : public QObject {
    Q_OBJECT
    QML_ELEMENT
    
    Q_PROPERTY(bool isPlaying READ isPlaying WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(float currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(float duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString fileName READ fileName WRITE setFileName NOTIFY fileNameChanged)
    Q_PROPERTY(int noteCount READ noteCount NOTIFY noteCountChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY trackCountChanged)
    Q_PROPERTY(int activeVoices READ activeVoices NOTIFY activeVoicesChanged)
    Q_PROPERTY(int nps READ nps NOTIFY npsChanged)
    Q_PROPERTY(float bpm READ bpm NOTIFY bpmChanged)
    Q_PROPERTY(float noteSpeed READ noteSpeed WRITE setNoteSpeed NOTIFY noteSpeedChanged)
    Q_PROPERTY(float postBuffer READ postBuffer WRITE setPostBuffer NOTIFY postBufferChanged)
    Q_PROPERTY(bool perTrackColors READ perTrackColors WRITE setPerTrackColors NOTIFY perTrackColorsChanged)
    
public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow();
    
    bool isPlaying() const { return isPlaying_; }
    float currentTime() const { return currentTime_; }
    float duration() const { return duration_; }
    QString fileName() const { return fileName_; }
    int noteCount() const { return noteCount_; }
    int trackCount() const { return trackCount_; }
    int activeVoices() const { return activeVoices_; }
    int nps() const { return nps_; }
    float bpm() const { return bpm_; }
    float noteSpeed() const { return noteSpeed_; }
    float postBuffer() const { return postBuffer_; }
    bool perTrackColors() const { return perTrackColors_; }
    
    Q_INVOKABLE void loadMidiFile(const QByteArray& data);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(float seconds);
    Q_INVOKABLE void setVolume(int value);
    Q_INVOKABLE void setChannelColor(int channel, const QColor& color);
    
    void setPlaying(bool playing);
    void setFileName(const QString& name);
    void setNoteSpeed(float speed);
    void setPostBuffer(float buffer);
    void setPerTrackColors(bool enable);
    
signals:
    void playingChanged();
    void currentTimeChanged();
    void durationChanged();
    void fileNameChanged();
    void noteCountChanged();
    void trackCountChanged();
    void activeVoicesChanged();
    void npsChanged();
    void bpmChanged();
    void noteSpeedChanged();
    void postBufferChanged();
    void perTrackColorsChanged();
    void fileLoaded();
    
private:
    void updateStats();
    void updateCurrentTime();
    
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
    int nps_ = 0;
    float bpm_ = 120.0f;
    float noteSpeed_ = 10.0f;
    float postBuffer_ = 0.0f;
    bool perTrackColors_ = false;
    
    QVector<QColor> channelColors_;
};