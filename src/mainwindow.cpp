#include "mainwindow.hpp"

#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

MainWindow::MainWindow(QObject *parent)
    : QObject(parent)
{
    channelColors_.resize(16);
    channelColors_.fill(QColor(129, 140, 248));
    
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::updateCurrentTime);
    timer->start(16);
}

MainWindow::~MainWindow() = default;

bool MainWindow::isPlaying() const { return isPlaying_; }
float MainWindow::currentTime() const { return currentTime_; }
float MainWindow::duration() const { return duration_; }
QString MainWindow::fileName() const { return fileName_; }
int MainWindow::noteCount() const { return noteCount_; }
int MainWindow::trackCount() const { return trackCount_; }
int MainWindow::activeVoices() const { return activeVoices_; }
int MainWindow::nps() const { return nps_; }
float MainWindow::bpm() const { return bpm_; }
float MainWindow::noteSpeed() const { return noteSpeed_; }
float MainWindow::postBuffer() const { return postBuffer_; }
bool MainWindow::perTrackColors() const { return perTrackColors_; }

void MainWindow::setPlaying(bool playing) {
    if (isPlaying_ != playing) {
        isPlaying_ = playing;
        emit playingChanged();
    }
}

void MainWindow::setFileName(const QString& name) {
    if (fileName_ != name) {
        fileName_ = name;
        emit fileNameChanged();
    }
}

void MainWindow::setNoteSpeed(float speed) {
    if (noteSpeed_ != speed) {
        noteSpeed_ = speed;
        emit noteSpeedChanged();
    }
}

void MainWindow::setPostBuffer(float buffer) {
    if (postBuffer_ != buffer) {
        postBuffer_ = buffer;
        emit postBufferChanged();
    }
}

void MainWindow::setPerTrackColors(bool enable) {
    if (perTrackColors_ != enable) {
        perTrackColors_ = enable;
        emit perTrackColorsChanged();
    }
}

void MainWindow::loadMidiFile(const QByteArray& data) {
    if (parser_.parse(reinterpret_cast<const uint8_t*>(data.constData()), 
                      data.size(), document_)) {
        setFileName(QFileInfo(fileName_).fileName());
        setNoteCount(document_.notes.size());
        setTrackCount(document_.trackCount);
        setDuration(document_.durationSeconds);
        
        scheduler_.setDocument(&document_);
        
        emit fileLoaded();
    }
}

void MainWindow::play() {
    setPlaying(true);
    scheduler_.start();
}

void MainWindow::pause() {
    setPlaying(false);
    scheduler_.pause();
}

void MainWindow::stop() {
    setPlaying(false);
    currentTime_ = 0.0f;
    emit currentTimeChanged();
    scheduler_.stop();
}

void MainWindow::seek(float seconds) {
    currentTime_ = seconds;
    emit currentTimeChanged();
    scheduler_.seek(seconds);
}

void MainWindow::setVolume(int value) {
    // Implementar control de volumen
}

void MainWindow::setChannelColor(int channel, const QColor& color) {
    if (channel >= 0 && channel < 16) {
        channelColors_[channel] = color;
    }
}

void MainWindow::updateStats() {
    if (!document_.notes.empty()) {
        float windowStart = currentTime_;
        float windowEnd = currentTime_ + 1.0f;
        
        int count = 0;
        for (const auto& note : document_.notes) {
            if (note.startTime >= windowStart && note.startTime <= windowEnd) {
                ++count;
            }
        }
        
        if (nps_ != count) {
            nps_ = count;
            emit npsChanged();
        }
        
        int active = 0;
        for (const auto& note : document_.notes) {
            if (note.startTime <= currentTime_ && note.endTime >= currentTime_) {
                ++active;
            }
        }
        
        if (activeVoices_ != active) {
            activeVoices_ = active;
            emit activeVoicesChanged();
        }
    }
}

void MainWindow::updateCurrentTime() {
    if (isPlaying_) {
        currentTime_ += 0.016f;
        emit currentTimeChanged();
        updateStats();
    }
}