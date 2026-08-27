#pragma once

#include <cstdint>
#include <vector>

namespace wasmidi {

struct Note {
    float startSeconds = 0.0f;
    float endSeconds = 0.0f;
    uint8_t pitch = 0;
    uint8_t channel = 0;
    uint8_t velocity = 0;
    uint16_t track = 0;
};

struct ControlEvent {
    float timeSeconds = 0.0f;
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

    std::vector<Note> notes;
    std::vector<ControlEvent> controls;
    std::vector<TempoChange> tempoMap;
    std::vector<uint32_t> activeChannelMasks;
};

}