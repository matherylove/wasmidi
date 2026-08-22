#pragma once

#include "midi_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wasmidi {

// Browser equivalent of SharpMIDI's MemoryMappedFile + compact BigArray model.
//
// The original MIDI File/Blob remains the authoritative backing store. Loading
// scans every byte twice but never materializes the raw file, the channel-event
// stream, or one VisualNote per NoteOn inside the WASM heap. The retained index
// is proportional to track/tick structure rather than total channel-event
// count. Playback/render pages are decoded from the mapped source on demand.
class MidiMappedStore {
public:
    struct EventWord {
        uint32_t tick = 0;
        // bytes: status, data1, data2, color(global low nibble / track high)
        uint32_t packed = 0;
    };
    static_assert(sizeof(EventWord) == 8);

    struct KeySnapshot {
        std::array<uint32_t, 128> counts{};
        std::array<uint8_t, 128> globalColors{};
        std::array<uint8_t, 128> trackColors{};
    };

    MidiMappedStore();
    ~MidiMappedStore();

    MidiMappedStore(const MidiMappedStore&) = delete;
    MidiMappedStore& operator=(const MidiMappedStore&) = delete;

    void clear();

    bool index(
        uint64_t size,
        MidiReadAt readAt,
        void* readUser,
        MidiDocument& metadata,
        MidiParseProgress progress = nullptr,
        void* progressUser = nullptr);

    bool valid() const { return valid_; }
    const char* error() const { return error_.c_str(); }
    const MidiDocument& metadata() const { return metadata_; }

    // One screen/page of SharpMIDI-style merged RenderNotes. Long notes that
    // began before PAGESTART are emitted as carry notes and open notes have
    // endTick=0 so the shader extends them to the current page edge.
    bool buildVisualPage(
        uint32_t pageStart,
        uint32_t pageEnd,
        std::vector<VisualNote>& output);

    // Exact-at-tick keyboard state under the same count/color merge rules used
    // by the visual page builder. NoteOff at T remains active at T, matching the
    // horizontal renderer's inclusive [start,end] lifetime.
    bool buildKeySnapshot(
        uint32_t tick,
        KeySnapshot& output);

    // Persistent forward playback cursor. Events remain exact/unmerged; only
    // the output batch is bounded. A caller can repeatedly request batches until
    // complete=true before advancing SnappySynth's safeUntil.
    void resetEventCursor(uint32_t startTick);
    bool buildEventBatch(
        uint32_t endTick,
        std::size_t maxEvents,
        std::vector<EventWord>& output,
        bool& complete);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool valid_ = false;
    std::string error_;
    MidiDocument metadata_;
};

} // namespace wasmidi
