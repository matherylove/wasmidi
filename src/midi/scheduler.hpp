#pragma once

#include "midi_parser.hpp"
#include <vector>
#include <cstdint>

namespace wasmidi {

class MidiScheduler {
public:
    MidiScheduler();
    ~MidiScheduler();
    
    void setDocument(const MidiDocument* doc);
    void setSampleRate(float sampleRate);
    
    void start();
    void stop();
    void pause();
    void resume();
    
    void seek(float seconds);
    float getPosition() const { return currentTime_; }
    bool isPlaying() const { return playing_; }
    
    struct ScheduledEvent {
        float time;
        uint8_t type;
        uint8_t channel;
        uint8_t data1;
        uint8_t data2;
    };
    
    const std::vector<ScheduledEvent>& getEventsForFrame(float horizon);
    
private:
    const MidiDocument* document_ = nullptr;
    float sampleRate_ = 44100.0f;
    float currentTime_ = 0.0f;
    bool playing_ = false;
    
    std::size_t noteCursor_ = 0;
    std::size_t ccCursor_ = 0;
    
    std::vector<ScheduledEvent> pendingEvents_;
};

}