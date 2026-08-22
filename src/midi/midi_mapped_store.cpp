#include "midi_mapped_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

namespace wasmidi {
namespace {

// SharpMIDI maps the source file and parses directly from the mapping. Browser
// File/Blob cannot be memory-mapped into WASM, so this is the equivalent: a
// small shared LRU of fixed source windows. File size never determines heap
// size. Thirty-two 1 MiB windows cap source-cache residency at 32 MiB.
constexpr std::size_t SourcePageBytes = 1u << 20;
constexpr std::size_t SourcePageCount = 32;
constexpr uint64_t TrackCheckpointEventStride = 1u << 16; // 65,536 channel events
constexpr uint64_t TrackCheckpointByteStride = 4u << 20;  // or every 4 MiB source
constexpr std::size_t VisualCheckpointCount = 128;
constexpr std::size_t VisualStateCount = 16u * 128u;

class MappedReader {
public:
    void reset(uint64_t size, MidiReadAt readAt, void* user)
    {
        size_ = size;
        readAt_ = readAt;
        user_ = user;
        failed_ = false;
        stamp_ = 1;
        for (auto& page : pages_) {
            page.start = 0;
            page.length = 0;
            page.stamp = 0;
            page.valid = false;
            page.bytes.clear();
        }
    }

    uint64_t size() const { return size_; }
    bool failed() const { return failed_; }

    bool read(uint64_t offset, void* destination, std::size_t bytes)
    {
        if (bytes == 0)
            return true;
        if (!destination || !readAt_ || offset > size_ ||
            uint64_t(bytes) > size_ - offset) {
            failed_ = true;
            return false;
        }

        auto* out = static_cast<uint8_t*>(destination);
        while (bytes != 0) {
            const uint64_t pageStart = offset & ~uint64_t(SourcePageBytes - 1u);
            Page* page = find(pageStart);
            if (!page)
                page = load(pageStart);
            if (!page)
                return false;

            const std::size_t within = static_cast<std::size_t>(offset - pageStart);
            if (within >= page->length) {
                failed_ = true;
                return false;
            }
            const std::size_t chunk = std::min(bytes, page->length - within);
            std::memcpy(out, page->bytes.data() + within, chunk);
            out += chunk;
            offset += chunk;
            bytes -= chunk;
        }
        return true;
    }

    bool byte(uint64_t offset, uint8_t& value)
    {
        return read(offset, &value, 1);
    }

private:
    struct Page {
        uint64_t start = 0;
        std::size_t length = 0;
        uint64_t stamp = 0;
        bool valid = false;
        std::vector<uint8_t> bytes;
    };

    Page* find(uint64_t start)
    {
        for (auto& page : pages_) {
            if (page.valid && page.start == start) {
                page.stamp = ++stamp_;
                return &page;
            }
        }
        return nullptr;
    }

    Page* load(uint64_t start)
    {
        Page* victim = &pages_.front();
        for (auto& page : pages_) {
            if (!page.valid) {
                victim = &page;
                break;
            }
            if (page.stamp < victim->stamp)
                victim = &page;
        }

        const uint64_t remaining = size_ - start;
        const std::size_t length = static_cast<std::size_t>(
            std::min<uint64_t>(SourcePageBytes, remaining));
        if (victim->bytes.size() != SourcePageBytes)
            victim->bytes.resize(SourcePageBytes);
        if (length != 0 && !readAt_(user_, start, victim->bytes.data(), length)) {
            failed_ = true;
            victim->valid = false;
            return nullptr;
        }
        victim->start = start;
        victim->length = length;
        victim->stamp = ++stamp_;
        victim->valid = true;
        return victim;
    }

    uint64_t size_ = 0;
    MidiReadAt readAt_ = nullptr;
    void* user_ = nullptr;
    bool failed_ = false;
    uint64_t stamp_ = 1;
    std::array<Page, SourcePageCount> pages_{};
};

class Cursor {
public:
    Cursor(MappedReader& reader, uint64_t begin, uint64_t end)
        : reader_(&reader), position_(begin), end_(end)
    {
    }

    uint64_t position() const { return position_; }
    uint64_t remaining() const { return position_ <= end_ ? end_ - position_ : 0; }

    bool readByte(uint8_t& value)
    {
        if (!reader_ || position_ >= end_ || !reader_->byte(position_, value))
            return false;
        ++position_;
        return true;
    }

    bool read(void* destination, std::size_t bytes)
    {
        if (!reader_ || uint64_t(bytes) > remaining() ||
            !reader_->read(position_, destination, bytes))
            return false;
        position_ += bytes;
        return true;
    }

    bool skip(uint64_t bytes)
    {
        if (bytes > remaining())
            return false;
        position_ += bytes;
        return true;
    }

private:
    MappedReader* reader_ = nullptr;
    uint64_t position_ = 0;
    uint64_t end_ = 0;
};

uint16_t read16(Cursor& c, bool& ok)
{
    uint8_t b[2]{};
    if (!ok || !c.read(b, 2)) {
        ok = false;
        return 0;
    }
    return uint16_t((uint16_t(b[0]) << 8) | uint16_t(b[1]));
}

uint32_t read32(Cursor& c, bool& ok)
{
    uint8_t b[4]{};
    if (!ok || !c.read(b, 4)) {
        ok = false;
        return 0;
    }
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
           (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}

uint32_t readVar(Cursor& c, bool& ok)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t byte = 0;
        if (!ok || !c.readByte(byte)) {
            ok = false;
            return 0;
        }
        value = (value << 7) | uint32_t(byte & 0x7f);
        if ((byte & 0x80) == 0)
            return value;
    }
    ok = false;
    return 0;
}

// Running-status aware status read with the first data byte returned separately.
bool readStatusAndFirstData(
    Cursor& c,
    uint8_t& running,
    uint8_t& status,
    bool& hasFirstData,
    uint8_t& firstData)
{
    uint8_t next = 0;
    if (!c.readByte(next))
        return false;
    if (next < 0x80) {
        if (!running)
            return false;
        status = running;
        hasFirstData = true;
        firstData = next;
        return true;
    }

    status = next;
    hasFirstData = false;
    firstData = 0;
    if (status < 0xf0)
        running = status;
    else if (status < 0xf8 || status == 0xff)
        running = 0;
    return true;
}

int systemBytes(uint8_t status)
{
    switch (status) {
    case 0xf1: return 1;
    case 0xf2: return 2;
    case 0xf3: return 1;
    default: return 0;
    }
}

struct TrackCheckpoint {
    uint64_t sourceOffset = 0;
    uint64_t channelEventIndex = 0;
    uint32_t tickBefore = 0;
    uint32_t firstTick = 0;
    uint8_t runningBefore = 0;
};

struct TrackIndex {
    uint64_t offset = 0;
    uint32_t length = 0;
    uint64_t channelEvents = 0;
    uint32_t maxTick = 0;
    std::vector<TrackCheckpoint> checkpoints;
};

struct SysExRef {
    uint32_t tick = 0;
    uint64_t payloadOffset = 0;
    uint32_t length = 0;
    uint8_t status = 0xf0;
};

struct VisualState {
    std::array<uint64_t, VisualStateCount> count{};
    std::array<uint8_t, VisualStateCount> style{};
    std::array<uint8_t, VisualStateCount> velocity{};
    std::array<uint32_t, VisualStateCount> startTick{};
};

struct VisualCheckpoint {
    uint32_t tick = 0;
    VisualState state;
};

void buildTempoIndex(MidiDocument& output)
{
    std::stable_sort(
        output.tempoMap.begin(), output.tempoMap.end(),
        [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });

    std::vector<TempoChange> deduped;
    deduped.reserve(output.tempoMap.size());
    for (const auto& tempo : output.tempoMap) {
        if (!deduped.empty() && deduped.back().tick == tempo.tick)
            deduped.back() = tempo;
        else
            deduped.push_back(tempo);
    }
    if (deduped.empty() || deduped.front().tick != 0)
        deduped.insert(deduped.begin(), {0, 500000});
    output.tempoMap.swap(deduped);
    output.tempoSeconds.resize(output.tempoMap.size());

    double seconds = 0.0;
    uint32_t previousTick = 0;
    uint32_t previousUs = 500000;
    const double ppq = std::max<uint16_t>(1, output.ticksPerBeat);
    for (std::size_t i = 0; i < output.tempoMap.size(); ++i) {
        const auto& tempo = output.tempoMap[i];
        seconds += double(tempo.tick - previousTick) / ppq *
                   (double(previousUs) / 1000000.0);
        output.tempoSeconds[i] = seconds;
        previousTick = tempo.tick;
        if (tempo.microsecondsPerBeat)
            previousUs = tempo.microsecondsPerBeat;
    }
}

} // namespace

struct MidiMappedStore::Impl {
    MappedReader reader;
    uint64_t sourceSize = 0;
    std::vector<TrackIndex> tracks;
    std::vector<SysExRef> sysex;
    std::array<uint8_t, 16> globalColors{};
    std::vector<std::array<uint8_t, 16>> trackColors;
    std::vector<VisualCheckpoint> checkpoints;

    struct Decoder {
        Impl* owner = nullptr;
        uint32_t trackIndex = 0;
        std::unique_ptr<Cursor> cursor;
        uint32_t tick = 0;
        uint8_t running = 0;
        bool ok = true;
        bool currentValid = false;
        EventWord current{};

        bool readNextChannel(EventWord& out)
        {
            if (!cursor)
                return false;

            while (cursor->remaining() && ok) {
                const uint32_t delta = readVar(*cursor, ok);
                if (!ok || delta > std::numeric_limits<uint32_t>::max() - tick) {
                    ok = false;
                    return false;
                }
                tick += delta;

                uint8_t status = 0;
                uint8_t firstData = 0;
                bool hasFirstData = false;
                if (!readStatusAndFirstData(
                        *cursor, running, status, hasFirstData, firstData)) {
                    ok = false;
                    return false;
                }

                if (status == 0xff) {
                    uint8_t meta = 0;
                    if (hasFirstData) { ok = false; return false; }
                    if (!cursor->readByte(meta)) { ok = false; return false; }
                    const uint32_t len = readVar(*cursor, ok);
                    if (!ok || !cursor->skip(len)) { ok = false; return false; }
                    if (meta == 0x2f)
                        return false;
                    continue;
                }

                if (status == 0xf0 || status == 0xf7) {
                    if (hasFirstData) { ok = false; return false; }
                    const uint32_t len = readVar(*cursor, ok);
                    if (!ok || !cursor->skip(len)) { ok = false; return false; }
                    continue;
                }

                if (status >= 0xf0) {
                    if (hasFirstData) { ok = false; return false; }
                    if (!cursor->skip(static_cast<uint64_t>(systemBytes(status)))) {
                        ok = false;
                        return false;
                    }
                    continue;
                }

                const uint8_t command = status & 0xf0;
                const uint8_t channel = status & 0x0f;
                const int dataBytes = (command == 0xc0 || command == 0xd0) ? 1 : 2;
                uint8_t data1 = 0, data2 = 0;
                if (hasFirstData) {
                    data1 = firstData;
                } else if (!cursor->readByte(data1)) {
                    ok = false;
                    return false;
                }
                if (dataBytes == 2 && !cursor->readByte(data2)) {
                    ok = false;
                    return false;
                }

                if (command == 0x90 && data2 == 0) {
                    status = static_cast<uint8_t>(0x80 | channel);
                    data2 = 64;
                }

                const uint8_t color = static_cast<uint8_t>(
                    (owner->globalColors[channel] & 0x0f) |
                    ((owner->trackColors[trackIndex][channel] & 0x0f) << 4));
                out.tick = tick;
                out.packed = uint32_t(status) |
                             (uint32_t(data1) << 8) |
                             (uint32_t(data2) << 16) |
                             (uint32_t(color) << 24);
                return true;
            }
            return false;
        }

        bool reset(Impl* o, uint32_t track, uint32_t startTick)
        {
            owner = o;
            trackIndex = track;
            ok = true;
            currentValid = false;

            const TrackIndex& ti = owner->tracks[track];
            if (ti.checkpoints.empty())
                return true;

            // Choose the latest source checkpoint whose first channel event is
            // strictly before STARTTICK. Strictness is essential at a billion-
            // note crashpoint: seeking to tick T must not skip earlier events at
            // the same T merely because several sparse checkpoints share it.
            auto it = std::lower_bound(
                ti.checkpoints.begin(), ti.checkpoints.end(), startTick,
                [](const TrackCheckpoint& cp, uint32_t value) {
                    return cp.firstTick < value;
                });
            if (it == ti.checkpoints.begin()) {
                it = ti.checkpoints.begin();
            } else {
                --it;
            }

            cursor = std::make_unique<Cursor>(
                owner->reader,
                it->sourceOffset,
                ti.offset + uint64_t(ti.length));
            tick = it->tickBefore;
            running = it->runningBefore;

            EventWord ev;
            while (readNextChannel(ev)) {
                if (ev.tick >= startTick) {
                    current = ev;
                    currentValid = true;
                    return true;
                }
            }
            return ok;
        }

        bool advance()
        {
            EventWord ev;
            if (readNextChannel(ev)) {
                current = ev;
                currentValid = true;
                return true;
            }
            // End-of-track / end-of-cursor is a normal condition.  Only
            // propagate false when the decoder actually marked itself bad.
            // Iterator::next() uses this return value to distinguish EOF from
            // malformed MIDI data.
            currentValid = false;
            return ok;
        }
    };

    struct HeapNode {
        uint32_t tick = 0;
        uint32_t track = 0;
    };
    struct HeapGreater {
        bool operator()(const HeapNode& a, const HeapNode& b) const
        {
            if (a.tick != b.tick)
                return a.tick > b.tick;
            return a.track > b.track;
        }
    };

    struct Iterator {
        Impl* owner = nullptr;
        uint32_t endTick = std::numeric_limits<uint32_t>::max();
        std::vector<Decoder> decoders;
        std::priority_queue<HeapNode, std::vector<HeapNode>, HeapGreater> heap;
        bool ok = true;

        void reset(Impl* o, uint32_t start, uint32_t end)
        {
            owner = o;
            endTick = end;
            ok = true;
            heap = {};
            decoders.clear();
            decoders.resize(owner->tracks.size());

            for (uint32_t t = 0; t < owner->tracks.size(); ++t) {
                if (!decoders[t].reset(owner, t, start)) {
                    ok = false;
                    return;
                }
                if (decoders[t].currentValid && decoders[t].current.tick <= endTick)
                    heap.push({decoders[t].current.tick, t});
            }
        }

        bool peek(EventWord& out) const
        {
            if (heap.empty())
                return false;
            const auto n = heap.top();
            out = decoders[n.track].current;
            return true;
        }

        bool next(EventWord& out)
        {
            if (heap.empty())
                return false;
            const auto n = heap.top();
            heap.pop();
            Decoder& decoder = decoders[n.track];
            out = decoder.current;
            if (!decoder.advance()) {
                ok = false;
                return true; // deliver the current valid event, fail afterwards
            }
            if (decoder.currentValid && decoder.current.tick <= endTick)
                heap.push({decoder.current.tick, n.track});
            return true;
        }

        bool failed() const
        {
            if (!ok)
                return true;
            for (const auto& decoder : decoders) {
                if (!decoder.ok)
                    return true;
            }
            return false;
        }
    };

    Iterator eventCursor;
    bool eventCursorValid = false;

    bool scanTrack(
        uint32_t trackIndex,
        MidiDocument& metadata,
        uint64_t& totalEvents,
        uint64_t& totalNotes,
        uint64_t& totalControls)
    {
        TrackIndex& track = tracks[trackIndex];
        track.checkpoints.clear();
        track.checkpoints.reserve(
            static_cast<std::size_t>(track.length / TrackCheckpointByteStride) + 2u);
        // Initial checkpoint includes leading meta/SysEx and is always safe.
        track.checkpoints.push_back({track.offset, 0, 0, 0, 0});

        Cursor cursor(reader, track.offset, track.offset + uint64_t(track.length));
        uint32_t tick = 0;
        uint8_t running = 0;
        bool ok = true;
        uint64_t channelEventIndex = 0;

        while (cursor.remaining() && ok) {
            const uint64_t eventStart = cursor.position();
            const uint32_t tickBefore = tick;
            const uint8_t runningBefore = running;
            const uint32_t delta = readVar(cursor, ok);
            if (!ok || delta > std::numeric_limits<uint32_t>::max() - tick)
                return false;
            tick += delta;

            uint8_t status = 0;
            uint8_t firstData = 0;
            bool hasFirstData = false;
            if (!readStatusAndFirstData(
                    cursor, running, status, hasFirstData, firstData))
                return false;

            if (status == 0xff) {
                if (hasFirstData)
                    return false;
                uint8_t meta = 0;
                if (!cursor.readByte(meta))
                    return false;
                const uint32_t len = readVar(cursor, ok);
                if (!ok || uint64_t(len) > cursor.remaining())
                    return false;
                if (meta == 0x51 && len >= 3) {
                    uint8_t b[3]{};
                    if (!cursor.read(b, 3))
                        return false;
                    metadata.tempoMap.push_back({
                        tick,
                        (uint32_t(b[0]) << 16) |
                        (uint32_t(b[1]) << 8) |
                        uint32_t(b[2])
                    });
                    if (!cursor.skip(uint64_t(len) - 3u))
                        return false;
                } else if (!cursor.skip(len)) {
                    return false;
                }
                if (meta == 0x2f)
                    break;
                continue;
            }

            if (status == 0xf0 || status == 0xf7) {
                if (hasFirstData)
                    return false;
                const uint32_t len = readVar(cursor, ok);
                if (!ok || uint64_t(len) > cursor.remaining())
                    return false;
                sysex.push_back({tick, cursor.position(), len, status});
                if (!cursor.skip(len))
                    return false;
                continue;
            }

            if (status >= 0xf0) {
                if (hasFirstData)
                    return false;
                if (!cursor.skip(static_cast<uint64_t>(systemBytes(status))))
                    return false;
                continue;
            }

            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            const int dataBytes = (command == 0xc0 || command == 0xd0) ? 1 : 2;
            uint8_t d1 = 0, d2 = 0;
            if (hasFirstData)
                d1 = firstData;
            else if (!cursor.readByte(d1))
                return false;
            if (dataBytes == 2 && !cursor.readByte(d2))
                return false;

            // Record a sparse source checkpoint BEFORE this channel event. It
            // costs ~32 bytes per 65k channel events instead of retaining a
            // TickGroup/CompactEvent per event/tick.
            const TrackCheckpoint& lastCp = track.checkpoints.back();
            if (channelEventIndex != 0 &&
                ((channelEventIndex % TrackCheckpointEventStride) == 0 ||
                 eventStart - lastCp.sourceOffset >= TrackCheckpointByteStride)) {
                track.checkpoints.push_back({
                    eventStart,
                    channelEventIndex,
                    tickBefore,
                    tick,
                    runningBefore
                });
            } else if (channelEventIndex == 0) {
                // Refine the initial checkpoint to the actual first channel
                // event so repeated random access does not rescan huge leading
                // metadata. Its firstTick remains the event's absolute tick.
                track.checkpoints.front().firstTick = tick;
            }

            const bool noteOn = command == 0x90 && d2 != 0;
            const bool noteOff = command == 0x80 || (command == 0x90 && d2 == 0);
            const bool control = command == 0xb0 || command == 0xc0 ||
                                 command == 0xd0 || command == 0xe0;

            ++channelEventIndex;
            ++track.channelEvents;
            ++totalEvents;
            metadata.activeChannelMasks[trackIndex] |= (1u << channel);
            if (noteOn) {
                ++totalNotes;
                metadata.hasPitch = true;
                metadata.minPitch = std::min(metadata.minPitch, d1);
                metadata.maxPitch = std::max(metadata.maxPitch, d1);
            }
            if (control)
                ++totalControls;
            (void)noteOff;
        }

        track.maxTick = tick;
        return ok;
    }

    void buildColorTables(const MidiDocument& metadata)
    {
        globalColors.fill(0);
        std::array<int8_t, 16> assigned{};
        assigned.fill(-1);
        int next = 0;
        for (uint32_t mask : metadata.activeChannelMasks) {
            for (int ch = 0; ch < 16; ++ch) {
                if ((mask & (1u << ch)) && assigned[ch] < 0)
                    assigned[ch] = static_cast<int8_t>(next++ & 15);
            }
        }
        for (int ch = 0; ch < 16; ++ch)
            globalColors[ch] = assigned[ch] >= 0 ? uint8_t(assigned[ch]) : uint8_t(ch);

        trackColors.resize(metadata.trackCount);
        next = 0;
        for (std::size_t t = 0; t < trackColors.size(); ++t) {
            for (int ch = 0; ch < 16; ++ch)
                trackColors[t][ch] = uint8_t(ch);
            const uint32_t mask = metadata.activeChannelMasks[t];
            for (int ch = 0; ch < 16; ++ch) {
                if (mask & (1u << ch))
                    trackColors[t][ch] = uint8_t(next++ & 15);
            }
        }
    }

    static void applyVisualEvent(
        VisualState& state,
        const EventWord& event,
        std::vector<VisualNote>* output,
        std::array<int64_t, VisualStateCount>* outputIndex)
    {
        const uint8_t status = uint8_t(event.packed & 0xffu);
        const uint8_t command = status & 0xf0;
        if (command != 0x90 && command != 0x80)
            return;

        const uint8_t channel = status & 0x0f;
        const uint8_t pitch = uint8_t((event.packed >> 8) & 0x7fu);
        const uint8_t velocity = uint8_t((event.packed >> 16) & 0x7fu);
        const uint8_t color = uint8_t((event.packed >> 24) & 0xffu);
        const std::size_t idx = std::size_t(channel) * 128u + pitch;
        const bool noteOn = command == 0x90 && velocity != 0;

        auto closeCurrent = [&](uint32_t endTick) {
            if (!output || !outputIndex)
                return;
            const int64_t oi = (*outputIndex)[idx];
            if (oi < 0 || static_cast<std::size_t>(oi) >= output->size())
                return;
            VisualNote& note = (*output)[static_cast<std::size_t>(oi)];
            if (note.endTick != 0)
                return;
            // Zero means "still open" in the remote shader. Give a same-tick
            // NoteOff one tick of visible width instead of colliding with that
            // sentinel.
            note.endTick = endTick > note.startTick
                ? endTick
                : (note.startTick == std::numeric_limits<uint32_t>::max()
                    ? note.startTick
                    : note.startTick + 1u);
            (*outputIndex)[idx] = -1;
        };

        if (noteOn) {
            if (state.count[idx] != 0 && state.style[idx] != color) {
                closeCurrent(event.tick);
                state.count[idx] = 0;
            }
            if (state.count[idx] == 0) {
                state.style[idx] = color;
                state.velocity[idx] = velocity;
                state.startTick[idx] = event.tick;
                if (output && outputIndex) {
                    (*outputIndex)[idx] = static_cast<int64_t>(output->size());
                    output->push_back({
                        event.tick,
                        0,
                        uint32_t(velocity) |
                        (uint32_t(pitch) << 8) |
                        (uint32_t(color & 0x0f) << 16) |
                        (uint32_t((color >> 4) & 0x0f) << 20) |
                        (uint32_t(channel) << 24)
                    });
                }
            }
            ++state.count[idx];
            return;
        }

        if (state.count[idx] != 0 && state.style[idx] == color) {
            --state.count[idx];
            if (state.count[idx] == 0)
                closeCurrent(event.tick);
        }
    }

    const VisualCheckpoint& checkpointFor(uint32_t tick) const
    {
        auto it = std::upper_bound(
            checkpoints.begin(), checkpoints.end(), tick,
            [](uint32_t value, const VisualCheckpoint& cp) {
                return value < cp.tick;
            });
        if (it == checkpoints.begin())
            return checkpoints.front();
        return *(it - 1);
    }

    bool buildCheckpoints(
        MidiDocument& metadata,
        MidiParseProgress progress,
        void* user,
        uint64_t totalEvents)
    {
        checkpoints.clear();
        checkpoints.reserve(VisualCheckpointCount + 2);
        VisualState visualState;
        std::array<uint64_t, VisualStateCount> rawCounts{};
        uint64_t rawActive = 0;
        uint64_t peakActive = 0;

        const uint32_t span = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(
                (uint64_t(metadata.maxTick) + VisualCheckpointCount - 1u) /
                VisualCheckpointCount));
        uint32_t nextCheckpoint = 0;

        if (metadata.durationSeconds > 0.0f) {
            constexpr double Step = 0.5;
            const double wanted = std::ceil(double(metadata.durationSeconds) / Step) + 1.0;
            // Keep pathological multi-year timing from turning a tiny MIDI into
            // an enormous stats allocation. This is UI metadata only.
            constexpr std::size_t MaxTimelineBuckets = 1u << 20;
            const std::size_t buckets = static_cast<std::size_t>(
                std::clamp<double>(wanted, 1.0, double(MaxTimelineBuckets)));
            metadata.derivedNpsTimeline.assign(buckets, 0);
        }

        Iterator it;
        it.reset(this, 0, metadata.maxTick);
        if (it.failed())
            return false;

        EventWord event;
        uint64_t processed = 0;
        auto saveBefore = [&](uint32_t eventTick) {
            while (nextCheckpoint <= metadata.maxTick &&
                   nextCheckpoint <= eventTick &&
                   checkpoints.size() <= VisualCheckpointCount + 1) {
                checkpoints.push_back({nextCheckpoint, visualState});
                if (metadata.maxTick - nextCheckpoint < span) {
                    if (nextCheckpoint == metadata.maxTick)
                        break;
                    nextCheckpoint = metadata.maxTick;
                } else {
                    nextCheckpoint += span;
                }
            }
        };

        while (it.next(event)) {
            // A checkpoint at T means state strictly before events at T.
            if (event.tick >= nextCheckpoint)
                saveBefore(event.tick);

            const uint8_t status = uint8_t(event.packed & 0xffu);
            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            const uint8_t pitch = uint8_t((event.packed >> 8) & 0x7fu);
            const uint8_t velocity = uint8_t((event.packed >> 16) & 0x7fu);
            const std::size_t rawIndex = std::size_t(channel) * 128u + pitch;

            if (command == 0x90 && velocity != 0) {
                ++rawCounts[rawIndex];
                ++rawActive;
                peakActive = std::max(peakActive, rawActive);

                if (!metadata.derivedNpsTimeline.empty()) {
                    const double sec = metadata.tickToSeconds(event.tick);
                    constexpr double Step = 0.5;
                    std::size_t bucket = static_cast<std::size_t>(
                        std::max(0.0, std::floor(sec / Step)));
                    bucket = std::min(bucket, metadata.derivedNpsTimeline.size() - 1);
                    uint32_t& value = metadata.derivedNpsTimeline[bucket];
                    if (value != std::numeric_limits<uint32_t>::max())
                        ++value;
                }
            } else if (command == 0x80) {
                if (rawCounts[rawIndex] != 0) {
                    --rawCounts[rawIndex];
                    --rawActive;
                }
            }

            applyVisualEvent(visualState, event, nullptr, nullptr);
            ++processed;
            if (progress && (processed & ((1u << 20) - 1u)) == 0) {
                const int p = 70 + int(std::min<uint64_t>(23, processed * 23 /
                    std::max<uint64_t>(1, totalEvents)));
                progress(user, p, "Building SharpMIDI-style mapped checkpoints");
            }
        }
        if (it.failed())
            return false;

        if (checkpoints.empty() || checkpoints.front().tick != 0)
            checkpoints.insert(checkpoints.begin(), {0, VisualState{}});
        if (checkpoints.back().tick != metadata.maxTick)
            checkpoints.push_back({metadata.maxTick, visualState});

        metadata.derivedPeakPolyphony = static_cast<uint32_t>(
            std::min<uint64_t>(peakActive, std::numeric_limits<uint32_t>::max()));
        metadata.derivedPeakNps = 0;
        metadata.derivedPeakNpsTime = 0.0f;
        for (std::size_t b = 0; b < metadata.derivedNpsTimeline.size(); ++b) {
            const uint64_t perSecond = uint64_t(metadata.derivedNpsTimeline[b]) * 2u;
            const uint32_t value = static_cast<uint32_t>(
                std::min<uint64_t>(perSecond, std::numeric_limits<uint32_t>::max()));
            if (value > metadata.derivedPeakNps) {
                metadata.derivedPeakNps = value;
                metadata.derivedPeakNpsTime = static_cast<float>(double(b) * 0.5);
            }
        }
        return true;
    }
};

MidiMappedStore::MidiMappedStore()
    : impl_(new Impl)
{
}

MidiMappedStore::~MidiMappedStore()
{
    delete impl_;
}

void MidiMappedStore::clear()
{
    valid_ = false;
    error_.clear();
    metadata_ = MidiDocument{};
    if (!impl_)
        impl_ = new Impl;
    impl_->sourceSize = 0;
    impl_->tracks.clear();
    impl_->sysex.clear();
    impl_->trackColors.clear();
    impl_->checkpoints.clear();
    impl_->eventCursor = {};
    impl_->eventCursorValid = false;
}

bool MidiMappedStore::index(
    uint64_t size,
    MidiReadAt readAt,
    void* readUser,
    MidiDocument& metadata,
    MidiParseProgress progress,
    void* progressUser)
{
    clear();
    auto report = [&](int p, const char* stage) {
        if (progress)
            progress(progressUser, p, stage);
    };

    try {
        if (!readAt || size < 14) {
            error_ = "MIDI data is too short";
            return false;
        }

        impl_->sourceSize = size;
        impl_->reader.reset(size, readAt, readUser);
        MidiDocument out;
        out.remoteIndexed = true;
        out.minPitch = 127;
        out.maxPitch = 0;
        report(1, "Mapping MIDI header without copying the file");

        Cursor cursor(impl_->reader, 0, size);
        uint8_t id[4]{};
        bool ok = true;
        if (!cursor.read(id, 4) || std::memcmp(id, "MThd", 4) != 0) {
            error_ = "Missing MThd header";
            return false;
        }
        const uint32_t headerLength = read32(cursor, ok);
        if (!ok || headerLength < 6 || uint64_t(headerLength) > cursor.remaining()) {
            error_ = "Invalid MIDI header";
            return false;
        }
        const uint64_t headerEnd = cursor.position() + headerLength;
        out.format = read16(cursor, ok);
        out.trackCount = read16(cursor, ok);
        out.ticksPerBeat = read16(cursor, ok);
        if (!ok || out.format > 1) {
            error_ = "Only MIDI Format 0 and 1 are supported";
            return false;
        }
        if (out.ticksPerBeat == 0 || (out.ticksPerBeat & 0x8000)) {
            error_ = "SMPTE timing is not supported";
            return false;
        }
        if (cursor.position() > headerEnd || !cursor.skip(headerEnd - cursor.position())) {
            error_ = "Invalid MIDI header";
            return false;
        }

        impl_->tracks.resize(out.trackCount);
        for (uint16_t t = 0; t < out.trackCount; ++t) {
            if (!cursor.read(id, 4) || std::memcmp(id, "MTrk", 4) != 0) {
                error_ = "Invalid or truncated MTrk chunk";
                return false;
            }
            const uint32_t len = read32(cursor, ok);
            if (!ok || uint64_t(len) > cursor.remaining()) {
                error_ = "Truncated MIDI track";
                return false;
            }
            impl_->tracks[t].offset = cursor.position();
            impl_->tracks[t].length = len;
            if (!cursor.skip(len)) {
                error_ = "Truncated MIDI track";
                return false;
            }
        }

        out.activeChannelMasks.assign(out.trackCount, 0);
        out.tempoMap.reserve(64);
        out.tempoMap.push_back({0, 500000});

        uint64_t totalEvents = 0;
        uint64_t totalNotes = 0;
        uint64_t totalControls = 0;
        report(5, "SharpMIDI pass 1: indexing tracks");
        for (uint32_t t = 0; t < impl_->tracks.size(); ++t) {
            if (!impl_->scanTrack(t, out, totalEvents, totalNotes, totalControls)) {
                error_ = impl_->reader.failed()
                    ? "Could not read mapped MIDI source"
                    : "Malformed MIDI event data";
                return false;
            }
            out.maxTick = std::max(out.maxTick, impl_->tracks[t].maxTick);
            const int p = 5 + int(55.0 * double(t + 1) /
                double(std::max<std::size_t>(1, impl_->tracks.size())));
            report(p, "SharpMIDI pass 1: indexing tracks");
        }

        out.noteCount = totalNotes;
        out.controlEventCount = totalControls;
        buildTempoIndex(out);
        out.durationSeconds = static_cast<float>(out.tickToSeconds(out.maxTick));
        impl_->buildColorTables(out);

        report(65, "SharpMIDI pass 2: building bounded playback checkpoints");
        if (!impl_->buildCheckpoints(out, progress, progressUser, totalEvents)) {
            error_ = impl_->reader.failed()
                ? "Could not reread mapped MIDI source"
                : "Could not build mapped playback checkpoints";
            return false;
        }
        out.derivedStatsReady = true;

        std::stable_sort(
            impl_->sysex.begin(), impl_->sysex.end(),
            [](const SysExRef& a, const SysExRef& b) { return a.tick < b.tick; });

        // Qt receives metadata only. Unlike old WASMIDI, none of these arrays
        // scales with source-note/event count in steady state.
        out.events.clear();
        out.tickGroups.clear();
        out.visualNotes.clear();
        out.visualBlockMaxEnd.clear();
        out.visualKeyStarts.clear();
        out.visualKeyEnds.clear();
        out.visualKeyOwners.clear();
        out.sysEx.clear();

        metadata_ = out;
        metadata = out;
        valid_ = true;
        report(100, "Mapped MIDI ready");
        return true;
    } catch (const std::bad_alloc&) {
        error_ = "Mapped MIDI sparse index exhausted browser memory";
        return false;
    } catch (...) {
        error_ = "Mapped MIDI index failed";
        return false;
    }
}

bool MidiMappedStore::buildVisualPage(
    uint32_t pageStart,
    uint32_t pageEnd,
    std::vector<VisualNote>& output)
{
    output.clear();
    if (!valid_ || impl_->checkpoints.empty())
        return false;
    if (pageEnd < pageStart)
        std::swap(pageStart, pageEnd);
    pageStart = std::min(pageStart, metadata_.maxTick);
    pageEnd = std::min(pageEnd, metadata_.maxTick);

    const auto& cp = impl_->checkpointFor(pageStart);
    VisualState state = cp.state;
    Impl::Iterator it;
    it.reset(impl_, cp.tick, pageEnd);
    if (it.failed())
        return false;

    EventWord pending{};
    bool hasPending = false;
    EventWord ev{};
    while (it.peek(ev) && ev.tick < pageStart) {
        it.next(ev);
        Impl::applyVisualEvent(state, ev, nullptr, nullptr);
    }
    if (it.failed())
        return false;
    if (it.peek(pending) && pending.tick <= pageEnd)
        hasPending = true;

    std::array<int64_t, VisualStateCount> outputIndex{};
    outputIndex.fill(-1);
    for (std::size_t idx = 0; idx < VisualStateCount; ++idx) {
        if (!state.count[idx])
            continue;
        const uint8_t channel = uint8_t(idx / 128u);
        const uint8_t pitch = uint8_t(idx % 128u);
        const uint8_t color = state.style[idx];
        outputIndex[idx] = static_cast<int64_t>(output.size());
        output.push_back({
            state.startTick[idx],
            0,
            uint32_t(state.velocity[idx]) |
            (uint32_t(pitch) << 8) |
            (uint32_t(color & 0x0f) << 16) |
            (uint32_t((color >> 4) & 0x0f) << 20) |
            (uint32_t(channel) << 24)
        });
    }

    while (hasPending || it.peek(ev)) {
        if (hasPending) {
            it.next(ev);
            hasPending = false;
        } else {
            it.next(ev);
        }
        if (ev.tick > pageEnd)
            break;
        Impl::applyVisualEvent(state, ev, &output, &outputIndex);
    }
    if (it.failed())
        return false;

    // Open notes intentionally retain endTick=0. The remote renderer extends
    // that sentinel to uViewEnd exactly like SharpMIDI-raylib.
    return true;
}

bool MidiMappedStore::buildKeySnapshot(
    uint32_t tick,
    KeySnapshot& output)
{
    output = KeySnapshot{};
    if (!valid_ || impl_->checkpoints.empty())
        return false;
    tick = std::min(tick, metadata_.maxTick);

    const auto& cp = impl_->checkpointFor(tick);
    VisualState state = cp.state;
    Impl::Iterator it;
    it.reset(impl_, cp.tick, tick);
    if (it.failed())
        return false;

    EventWord ev;
    while (it.peek(ev)) {
        if (ev.tick > tick)
            break;
        it.next(ev);
        const uint8_t command = uint8_t(ev.packed & 0xf0u);
        // Inclusive end interval: a NoteOff at T turns the key off immediately
        // after T, matching the horizontal bar's [start,end] semantics.
        if (ev.tick == tick && command == 0x80)
            continue;
        Impl::applyVisualEvent(state, ev, nullptr, nullptr);
    }
    if (it.failed())
        return false;

    for (std::size_t idx = 0; idx < VisualStateCount; ++idx) {
        if (!state.count[idx])
            continue;
        const uint8_t pitch = uint8_t(idx % 128u);
        const uint8_t color = state.style[idx];
        const uint64_t sum = uint64_t(output.counts[pitch]) + state.count[idx];
        output.counts[pitch] = static_cast<uint32_t>(
            std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max()));
        output.globalColors[pitch] = color & 0x0f;
        output.trackColors[pitch] = (color >> 4) & 0x0f;
    }
    return true;
}

void MidiMappedStore::resetEventCursor(uint32_t startTick)
{
    if (!valid_) {
        impl_->eventCursorValid = false;
        return;
    }
    impl_->eventCursor.reset(
        impl_, std::min(startTick, metadata_.maxTick), metadata_.maxTick);
    impl_->eventCursorValid = !impl_->eventCursor.failed();
}

bool MidiMappedStore::buildEventBatch(
    uint32_t endTick,
    std::size_t maxEvents,
    std::vector<EventWord>& output,
    bool& complete)
{
    output.clear();
    complete = false;
    if (!valid_ || !impl_->eventCursorValid)
        return false;
    maxEvents = std::max<std::size_t>(1, maxEvents);
    endTick = std::min(endTick, metadata_.maxTick);

    EventWord ev;
    while (output.size() < maxEvents) {
        if (!impl_->eventCursor.peek(ev)) {
            if (impl_->eventCursor.failed())
                return false;
            complete = true;
            return true;
        }
        if (ev.tick > endTick) {
            // Do not consume the first event outside the requested horizon.
            complete = true;
            return true;
        }
        if (!impl_->eventCursor.next(ev)) {
            if (impl_->eventCursor.failed())
                return false;
            complete = true;
            return true;
        }
        output.push_back(ev);
    }

    // The batch filled. If the next event is still within the horizon the
    // caller must ask for another bounded batch before advancing safeUntil.
    if (!impl_->eventCursor.peek(ev) || ev.tick > endTick)
        complete = !impl_->eventCursor.failed();
    return !impl_->eventCursor.failed();
}

} // namespace wasmidi
