#ifndef MIDI_PARSER_H
#define MIDI_PARSER_H

#include <cstdint>
#include <vector>
#include <string>

namespace wasmidi {

struct NoteEvent {
    float startTime;
    float endTime;
    uint8_t note;
    uint8_t channel;
    uint8_t velocity;
    uint16_t trackIdx;
};

struct CCEvent {
    float time;
    uint8_t type;
    uint8_t channel;
    uint8_t d1;
    uint8_t d2;
};

struct TempoChange {
    uint32_t tick;
    uint32_t uspb;
};

struct MidiFile {
    std::vector<NoteEvent> notes;
    std::vector<CCEvent> ccEvents;
    std::vector<TempoChange> tempoMap;
    uint32_t format;
    uint32_t numTracks;
    uint32_t ppq;
    float duration;
    std::vector<uint32_t> activeChannelMasks;
};

// Parse MIDI file from memory buffer
// Returns 0 on success, negative on error
int midi_parse(const uint8_t* data, uint32_t dataLen, MidiFile& out);

// Free allocated memory
void midi_parse_free(MidiFile& file);

} // namespace wasmidi

#endif // MIDI_PARSER_H