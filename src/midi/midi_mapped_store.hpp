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
// The original MIDI File/Blob is read during two SharpMIDI-style loading passes:
// a cheap track/count index and one exact compact-event materialization pass.
// After loading, audio, renderer sweeps, keyboard snapshots and seeks operate on
// the resident Memory64 event arrays and never return to Blob/FileReader. The
// raw file itself is not retained in the WASM heap and one VisualNote per source
// NoteOn is still avoided.
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

    // Per-frame mapped playback state. This replaces the old load-time graph
    // preprocessing and the checkpoint-per-frame keyboard path. The worker
    // advances one resident-event cursor and reports exactly the state needed
    // by the UI for this frame.
    struct LiveSnapshot {
        KeySnapshot keys{};
        uint32_t activeVoices = 0;
        uint32_t nps = 0;
        uint32_t ccPerSecond = 0;
    };

    // WebAssembly transport for the SharpMIDI-raylib renderer port. Appends are
    // emitted in absolute ring order; closes patch EndTick on an already-open
    // RenderNote. The browser renderer never builds screen pages.
    struct RenderClose {
        uint32_t noteId = 0;
        uint32_t endTick = 0;
    };
    static_assert(sizeof(RenderClose) == 8);

    // Compact SysEx transport used only by the mapped synth cursor. Payloads
    // stay resident in the Memory64 parser module; a batch references one
    // contiguous byte arena so JavaScript can transfer the whole group once.
    struct SysExBatchEvent {
        uint32_t tick = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };
    static_assert(sizeof(SysExBatchEvent) == 12);

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

    // One screen/page of MPWGL2-compatible independent RenderNotes. Long notes that
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

    // Incremental realtime counterpart to buildKeySnapshot(). npsStartTick is
    // the fractional MIDI tick corresponding to now-0.25s; ccStartTick is the
    // tick corresponding to now-1s. NPS therefore matches the MPWGL2 formula
    // (notes in the last quarter-second * 4) without an all-song stats pass.
    bool buildLiveSnapshot(
        double tick,
        double npsStartTick,
        double ccStartTick,
        LiveSnapshot& output,
        bool forceReset = false);

    // Persistent forward playback cursor. Ordering is exact. Consecutive,
    // byte-identical NoteOns at the same tick may be losslessly folded into
    // SnappySynthV2's native high-byte overlap count (up to 256 voices per
    // output word); controllers/NoteOffs and differently ordered events are
    // never merged. Each batch also reports the first unconsumed tick, allowing
    // the caller to render only the source prefix that is already complete.
    void resetEventCursor(uint32_t startTick);
    bool buildEventBatch(
        uint32_t endTick,
        std::size_t maxEvents,
        std::vector<EventWord>& output,
        std::vector<SysExBatchEvent>& sysExEvents,
        std::vector<uint8_t>& sysExBytes,
        bool& complete,
        uint32_t& nextTick,
        bool& hasNextTick);

    // Replay only stateful SysEx that occurred before STARTTICK when seeking.
    // SysEx counts are normally tiny; retaining payloads in Memory64 avoids any
    // runtime Blob/FileReader access while keeping Qt's wasm32 heap flat.
    bool buildHistoricalSysEx(
        uint32_t startTick,
        std::vector<SysExBatchEvent>& sysExEvents,
        std::vector<uint8_t>& sysExBytes);

    // Compact channel-selector restore for mapped seeks.  SnappySynth latches
    // CC0/CC32 only when Program Change is processed; emitting both the applied
    // bank and any later pending Bank Select values recreates that exact latch
    // without replaying millions of historical NoteOns.
    bool buildHistoricalSelectorState(
        uint32_t startTick,
        std::vector<EventWord>& output);

    // Faithful SharpMIDI-raylib visual sweep. This is an independent cursor
    // from SnappySynth playback. It reproduces GLNoteRenderer.ProcessOneEvent:
    // one active counter per channel/key, track/color-owner replacement, open
    // notes with EndTick=0, and absolute ring IDs for later NoteOff patching.
    void resetRenderCursor(uint32_t startTick, bool perTrackColors);
    bool buildRenderSweep(
        uint32_t endTick,
        std::size_t maxSourceEvents,
        std::vector<VisualNote>& appends,
        std::vector<RenderClose>& closes,
        uint32_t& appendBase,
        bool& complete,
        uint32_t& nextTick,
        bool& hasNextTick);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool valid_ = false;
    std::string error_;
    MidiDocument metadata_;
};

} // namespace wasmidi
