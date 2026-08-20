#pragma once

#include "midi_parser.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

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
        float time = 0.0f;
        uint8_t type = 0;
        uint8_t channel = 0;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
        uint16_t track = 0;
    };

    const std::vector<ScheduledEvent>& getEventsForFrame(float horizon);

private:
    void rebuildEventStream();

    const MidiDocument* document_ = nullptr;
    float sampleRate_ = 44100.0f;
    float currentTime_ = 0.0f;
    bool playing_ = false;
    std::size_t eventCursor_ = 0;
    std::vector<ScheduledEvent> events_;
    std::vector<ScheduledEvent> pendingEvents_;
};

} // namespace wasmidi
