#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "midi_parser.h"
#include <cstdint>
#include <vector>

namespace wasmidi {

class Scheduler {
public:
    Scheduler();
    ~Scheduler();
    
    void setMidiFile(const MidiFile* file);
    void setSampleRate(float sr);
    
    void start();
    void stop();
    void pause();
    void resume();
    
    void seek(float time);
    float getPosition() const { return currentTime; }
    bool isPlaying() const { return playing; }
    
    // Get next events to schedule (call every frame)
    struct ScheduledEvent {
        float time;
        uint8_t type;  // 0x90=noteOn, 0x80=noteOff, 0xB0=CC, 0xE0=pitchBend
        uint8_t channel;
        uint8_t d1;
        uint8_t d2;
    };
    
    const std::vector<ScheduledEvent>& getEventsForFrame(float horizon);
    
private:
    const MidiFile* midiFile;
    float sampleRate;
    float currentTime;
    float startTime;
    bool playing;
    
    uint32_t noteCursor;
    uint32_t ccCursor;
    
    std::vector<ScheduledEvent> pendingEvents;
};

} // namespace wasmidi

#endif // SCHEDULER_H