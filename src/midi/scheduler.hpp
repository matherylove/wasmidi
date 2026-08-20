#pragma once

#include "midi_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
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

    // Literal port of MPWGL2's dispatch model:
    // - call every ~5 ms
    // - NoteOn/CC look-ahead: 250 ms
    // - resume/seek lookback: 50 ms
    // - NoteOff emitted when <= 8 ms away
    const std::vector<ScheduledEvent>& getEventsForWindow(
        float now,
        float horizon = 0.25f,
        float lookback = 0.05f);

private:
    struct SoundingNote {
        float endSec = 0.0f;
        uint8_t channel = 0;
        uint8_t note = 0;
        uint16_t track = 0;
    };

    const MidiDocument* document_ = nullptr;
    float sampleRate_ = 44100.0f;
    float currentTime_ = 0.0f;
    bool playing_ = false;

    std::size_t noteCursor_ = 0;
    std::size_t controlCursor_ = 0;

    std::unordered_map<uint16_t, SoundingNote> soundingNotes_;
    std::vector<ScheduledEvent> pendingEvents_;
};

} // namespace wasmidi
