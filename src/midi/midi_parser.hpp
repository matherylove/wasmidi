#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace wasmidi {

struct NoteEvent {
    float startTime;
    float endTime;
    uint8_t pitch;
    uint8_t channel;
    uint8_t velocity;
    uint16_t track;
};

struct ControlEvent {
    float time;
    uint8_t type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
};

struct TempoChange {
    uint32_t tick;
    uint32_t microsecondsPerBeat;
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
    
    bool parseHeader(
        const uint8_t*& cursor,
        const uint8_t* end,
        MidiDocument& output
    );
    
    bool parseTracks(
        const uint8_t*& cursor,
        const uint8_t* end,
        MidiDocument& output
    );
    
    bool readTrack(
        const uint8_t* data,
        std::size_t size,
        std::uint16_t trackIndex,
        MidiDocument& output
    );
    
    double tickToSeconds(
        uint32_t tick,
        const std::vector<TempoChange>& tempoMap,
        uint16_t ppq
    ) const;
};

}