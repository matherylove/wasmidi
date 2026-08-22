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

// Exact MPWGL2 visual note representation.
//
// Notes are stored in NoteOn/start order (stable across equal start ticks).
// This is important: MPWGL2 paints its start-sorted note buffer sequentially,
// so later-starting notes overwrite earlier notes where they overlap.
//
// packedData:
//   bits  0..7  velocity
//   bits  8..15 pitch
//   bits 16..19 global/channel color slot
//   bits 20..23 per-track color slot
//   bits 24..27 original MIDI channel (used only to rebuild held notes on seek)
struct VisualNote {
    uint32_t startTick = 0;
    uint32_t endTick = 0;
    uint32_t packedData = 0;
};
static_assert(sizeof(VisualNote) == 12, "VisualNote must stay 12 bytes");

// Playback-time keyboard index. A single event represents COUNT notes with the
// same tick/pitch/global-color/per-track-color signature, so a million-note
// crashpoint can advance the keyboard with a handful of counted operations.
// packedData: bits 0..7 pitch, 8..11 global color, 12..15 per-track color.
struct VisualKeyEvent {
    uint32_t tick = 0;
    uint32_t count = 0;
    uint32_t packedData = 0;
};
static_assert(sizeof(VisualKeyEvent) == 12, "VisualKeyEvent must stay 12 bytes");

// Last-started color owner for one pitch at a tick. Counts live in the event
// tables above; this tiny stream preserves the keyboard's newest-note color.
struct VisualKeyOwner {
    uint32_t tick = 0;
    uint32_t packedData = 0;
};
static_assert(sizeof(VisualKeyOwner) == 8, "VisualKeyOwner must stay 8 bytes");

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

    // Browser-only huge-MIDI mode. When true, this object contains metadata
    // only; channel events/visual notes remain in the persistent Memory64
    // mapped-source Worker and are supplied as bounded pages on demand.
    bool remoteIndexed = false;
    uint8_t _remotePad[3]{};

    uint32_t maxTick = 0;
    float durationSeconds = 0.0f;

    uint64_t noteCount = 0;
    uint64_t controlEventCount = 0;

    uint8_t minPitch = 127;
    uint8_t maxPitch = 0;
    bool hasPitch = false;

    // Worker-precomputed UI statistics. Keeping the expensive peak-polyphony
    // sort here prevents the final 98% loading stage from blocking Qt.
    uint32_t derivedPeakNps = 0;
    float derivedPeakNpsTime = 0.0f;
    uint32_t derivedPeakPolyphony = 0;
    bool derivedStatsReady = false;
    std::vector<uint32_t> derivedNpsTimeline;

    // Single authoritative playback/control stream, already ordered by tick.
    std::vector<CompactEvent> events;
    std::vector<TickGroup> tickGroups;

    // Dedicated immutable visual stream. Unlike the previous runtime
    // channel+pitch merger, every NoteOn remains an independent note exactly
    // as in MPWGL2. It is already sorted by startTick, so rendering never sorts.
    std::vector<VisualNote> visualNotes;

    // Tiny exact-seek accelerator for keyboard/sustained-note reconstruction.
    // Each entry stores the maximum endTick of a 4096-note start-ordered block.
    // A seek can skip entire historical blocks whose notes all ended already.
    static constexpr std::size_t VisualSeekBlockSize = 4096;
    std::vector<uint32_t> visualBlockMaxEnd;

    // Compressed keyboard timeline built in the parser Worker. Starts are
    // active at tick <= currentTick; ends are removed only when endTick <
    // currentTick, matching the roll's inclusive VisualNote interval.
    std::vector<VisualKeyEvent> visualKeyStarts;
    std::vector<VisualKeyEvent> visualKeyEnds;
    std::vector<VisualKeyOwner> visualKeyOwners;

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

    // Binary search in the start-ordered visual stream. Double tick values are
    // intentional: secondsToTick() is fractional between real MIDI ticks and
    // this preserves MPWGL2's exact [time-window] boundary semantics.
    std::size_t lowerBoundVisualStart(double tick) const;
    std::size_t upperBoundVisualStart(double tick) const;

    uint8_t colorIndex(const CompactEvent& event, bool perTrack) const {
        return perTrack
            ? static_cast<uint8_t>((event.color >> 4) & 0x0f)
            : static_cast<uint8_t>(event.color & 0x0f);
    }

    uint8_t visualColorIndex(const VisualNote& note, bool perTrack) const {
        return perTrack
            ? static_cast<uint8_t>((note.packedData >> 20) & 0x0f)
            : static_cast<uint8_t>((note.packedData >> 16) & 0x0f);
    }

    uint8_t visualKeyPitch(uint32_t packedData) const {
        return static_cast<uint8_t>(packedData & 0x7f);
    }

    uint8_t visualKeyColor(uint32_t packedData, bool perTrack) const {
        return perTrack
            ? static_cast<uint8_t>((packedData >> 12) & 0x0f)
            : static_cast<uint8_t>((packedData >> 8) & 0x0f);
    }
};

using MidiParseProgress =
    void (*)(void* user, int percent, const char* stage);

// Random-access byte source used by the browser Worker. The callback must copy
// exactly BYTECOUNT bytes starting at OFFSET into DST and return true. Keeping
// the source outside WebAssembly lets giant MIDIs be parsed from File/Blob
// windows without first allocating the entire raw file inside the wasm heap.
using MidiReadAt =
    bool (*)(void* user, uint64_t offset, uint8_t* dst, std::size_t byteCount);

class MidiParser {
public:
    bool parse(
        const uint8_t* data,
        std::size_t size,
        MidiDocument& output,
        MidiParseProgress progress = nullptr,
        void* progressUser = nullptr);

    // Same parser, backed by a random-access source instead of one contiguous
    // allocation. This is the production browser path for very large MIDIs.
    bool parseReadAt(
        uint64_t size,
        MidiReadAt readAt,
        void* readUser,
        MidiDocument& output,
        MidiParseProgress progress = nullptr,
        void* progressUser = nullptr);
    const char* error() const { return errorMessage_; }

private:
    const char* errorMessage_ = "Unknown error";
};

} // namespace wasmidi
