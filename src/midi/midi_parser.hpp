#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wasmidi {

// Four-byte authoritative channel-event representation.
// color: low nibble = global/channel palette slot,
//        high nibble = per-track palette slot.
struct CompactEvent {
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint8_t color = 0;
};
static_assert(sizeof(CompactEvent) == 4, "CompactEvent must stay packed to 4 bytes");

// Sparse timing index. There is one entry only for ticks that contain
// channel events, rather than allocating maxTick+1 entries.
struct TickGroup {
    uint32_t tick = 0;
    uint32_t eventOffset = 0;
    uint32_t eventCount = 0;
    uint32_t noteOnCount = 0;
    uint32_t controlCount = 0;
};

struct TempoChange {
    uint32_t tick = 0;
    uint32_t microsecondsPerBeat = 500000;
};

struct SysExEvent {
    uint32_t tick = 0;
    std::vector<uint8_t> data;
};

struct MidiDocument {
    uint16_t format = 0;
    uint16_t trackCount = 0;
    uint16_t ticksPerBeat = 480;
    uint16_t _pad = 0;

    uint32_t maxTick = 0;
    float durationSeconds = 0.0f;

    uint64_t noteCount = 0;
    uint64_t controlEventCount = 0;

    uint8_t minPitch = 127;
    uint8_t maxPitch = 0;
    bool hasPitch = false;

    // Single authoritative event stream, already ordered by tick.
    std::vector<CompactEvent> events;
    std::vector<TickGroup> tickGroups;

    std::vector<TempoChange> tempoMap;
    // Seconds corresponding to tempoMap[i].tick. Tiny compared with MIDI data
    // and removes repeated O(tempo-count) conversions.
    std::vector<double> tempoSeconds;

    std::vector<SysExEvent> sysEx;
    std::vector<uint32_t> activeChannelMasks;

    double tickToSeconds(uint32_t tick) const;
    double secondsToTick(double seconds) const;

    // First group with tick >= value.
    std::size_t lowerBoundGroup(uint32_t tick) const;
    // First group with tick > value.
    std::size_t upperBoundGroup(uint32_t tick) const;

    uint8_t colorIndex(const CompactEvent& event, bool perTrack) const {
        return perTrack
            ? static_cast<uint8_t>((event.color >> 4) & 0x0f)
            : static_cast<uint8_t>(event.color & 0x0f);
    }
};

class MidiParser {
public:
    bool parse(const uint8_t* data, std::size_t size, MidiDocument& output);
    const char* error() const { return errorMessage_; }

private:
    const char* errorMessage_ = "Unknown error";
};

} // namespace wasmidi
