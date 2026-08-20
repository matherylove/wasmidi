#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wasmidi {

struct NoteEvent {
    float startTime = 0.0f;
    float endTime = 0.0f;
    uint8_t pitch = 0;
    uint8_t channel = 0;
    uint8_t velocity = 0;
    uint16_t track = 0;
};

struct ControlEvent {
    float time = 0.0f;
    uint8_t type = 0;
    uint8_t channel = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

struct TempoChange {
    uint32_t tick = 0;
    uint32_t microsecondsPerBeat = 500000;
};

struct MidiDocument {
    uint16_t format = 0;
    uint16_t trackCount = 0;
    uint16_t ticksPerBeat = 480;
    float durationSeconds = 0.0f;

    std::vector<NoteEvent> notes;
    std::vector<ControlEvent> controls;
    std::vector<TempoChange> tempoMap;
    std::vector<uint32_t> activeChannelMasks;
};

class MidiParser {
public:
    bool parse(const uint8_t* data, std::size_t size, MidiDocument& output);
    const char* error() const;

private:
    const char* errorMessage_ = "Unknown error";

    double tickToSeconds(uint32_t tick,
                         const std::vector<TempoChange>& tempoMap,
                         uint16_t ppq) const;
};

} // namespace wasmidi
