#include "midi_parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace wasmidi {
namespace {

struct MidiByteSource {
    uint64_t size = 0;
    const uint8_t* contiguous = nullptr;
    MidiReadAt readAt = nullptr;
    void* readUser = nullptr;
    bool readFailed = false;

    bool read(uint64_t offset, uint8_t* dst, std::size_t byteCount)
    {
        if (byteCount == 0)
            return true;

        if (!dst ||
            offset > size ||
            uint64_t(byteCount) > size - offset) {
            readFailed = true;
            return false;
        }

        if (contiguous) {
            std::memcpy(dst, contiguous + static_cast<std::size_t>(offset), byteCount);
            return true;
        }

        if (!readAt || !readAt(readUser, offset, dst, byteCount)) {
            readFailed = true;
            return false;
        }

        return true;
    }
};

struct TrackRef {
    uint64_t offset = 0;
    uint32_t size = 0;
};

struct TrackTickCount {
    uint32_t tick = 0;
    uint32_t eventCount = 0;
    uint32_t noteOnCount = 0;
    uint32_t controlCount = 0;
    uint32_t globalGroupIndex = 0;
};

struct TrackScan {
    std::vector<TrackTickCount> groups;
    uint32_t maxTick = 0;
};

// A single reusable 4 MiB read window is enough to keep FileReaderSync calls
// coarse while avoiding any allocation proportional to the MIDI file size.
// Contiguous/native parsing bypasses this cache entirely.
class SourceCursor {
public:
    static constexpr std::size_t CacheBytes = 4u * 1024u * 1024u;

    SourceCursor(MidiByteSource& source, uint64_t begin, uint64_t end)
        : source_(source), position_(begin), end_(end)
    {
    }

    uint64_t position() const { return position_; }
    uint64_t remaining() const {
        return position_ <= end_ ? end_ - position_ : 0;
    }

    bool readByte(uint8_t& value)
    {
        return read(&value, 1);
    }

    bool peekByte(uint8_t& value)
    {
        if (position_ >= end_)
            return false;

        if (source_.contiguous) {
            value = source_.contiguous[static_cast<std::size_t>(position_)];
            return true;
        }

        if (!ensureCache())
            return false;

        value = cache_[static_cast<std::size_t>(position_ - cacheStart_)];
        return true;
    }

    bool read(uint8_t* dst, std::size_t byteCount)
    {
        if (byteCount == 0)
            return true;

        if (!dst || uint64_t(byteCount) > remaining())
            return false;

        if (source_.contiguous) {
            std::memcpy(
                dst,
                source_.contiguous + static_cast<std::size_t>(position_),
                byteCount);
            position_ += byteCount;
            return true;
        }

        std::size_t written = 0;
        while (written < byteCount) {
            if (!ensureCache())
                return false;

            const std::size_t cacheOffset =
                static_cast<std::size_t>(position_ - cacheStart_);
            const std::size_t available = cacheSize_ - cacheOffset;
            const std::size_t take =
                std::min(available, byteCount - written);

            std::memcpy(dst + written, cache_.data() + cacheOffset, take);
            written += take;
            position_ += take;
        }

        return true;
    }

    bool skip(uint64_t byteCount)
    {
        if (byteCount > remaining())
            return false;
        position_ += byteCount;
        return true;
    }

private:
    bool ensureCache()
    {
        if (position_ >= end_)
            return false;

        if (cacheSize_ != 0 &&
            position_ >= cacheStart_ &&
            position_ < cacheStart_ + cacheSize_) {
            return true;
        }

        cacheStart_ = position_;
        cacheSize_ = static_cast<std::size_t>(
            std::min<uint64_t>(CacheBytes, end_ - position_));

        if (cache_.size() < cacheSize_)
            cache_.resize(cacheSize_);

        return source_.read(cacheStart_, cache_.data(), cacheSize_);
    }

    MidiByteSource& source_;
    uint64_t position_ = 0;
    uint64_t end_ = 0;
    std::vector<uint8_t> cache_;
    uint64_t cacheStart_ = 0;
    std::size_t cacheSize_ = 0;
};

uint16_t read16(SourceCursor& cursor, bool& ok)
{
    uint8_t bytes[2]{};
    if (!ok || !cursor.read(bytes, sizeof(bytes))) {
        ok = false;
        return 0;
    }

    return
        (uint16_t(bytes[0]) << 8) |
        uint16_t(bytes[1]);
}

uint32_t read32(SourceCursor& cursor, bool& ok)
{
    uint8_t bytes[4]{};
    if (!ok || !cursor.read(bytes, sizeof(bytes))) {
        ok = false;
        return 0;
    }

    return
        (uint32_t(bytes[0]) << 24) |
        (uint32_t(bytes[1]) << 16) |
        (uint32_t(bytes[2]) << 8) |
        uint32_t(bytes[3]);
}

uint32_t readVarLen(SourceCursor& cursor, bool& ok)
{
    uint32_t value = 0;

    for (int i = 0; i < 4; ++i) {
        uint8_t byte = 0;
        if (!ok || !cursor.readByte(byte)) {
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

// SMF running status applies to channel messages. System common/SysEx/meta
// cancels it; realtime bytes are tolerated without replacing it.
bool readStatus(SourceCursor& cursor,
                uint8_t& running,
                uint8_t& status)
{
    if (!cursor.peekByte(status))
        return false;

    if (status < 0x80) {
        if (running == 0)
            return false;
        status = running;
        return true;
    }

    uint8_t consumed = 0;
    if (!cursor.readByte(consumed))
        return false;

    status = consumed;

    if (status < 0xf0) {
        running = status;
    } else if (status < 0xf8 || status == 0xff) {
        running = 0;
    }

    return true;
}

int systemDataBytes(uint8_t status)
{
    switch (status) {
    case 0xf1: return 1;
    case 0xf2: return 2;
    case 0xf3: return 1;
    default:   return 0;
    }
}

void appendTrackCount(TrackScan& scan,
                      uint32_t tick,
                      bool noteOn,
                      bool control)
{
    if (scan.groups.empty() ||
        scan.groups.back().tick != tick) {
        scan.groups.push_back({
            tick, 0, 0, 0, 0
        });
    }

    TrackTickCount& group = scan.groups.back();
    ++group.eventCount;

    if (noteOn)
        ++group.noteOnCount;

    if (control)
        ++group.controlCount;
}

bool scanTrack(MidiByteSource& source,
               const TrackRef& track,
               std::size_t trackIndex,
               TrackScan& scan,
               MidiDocument& output,
               uint64_t& totalEvents,
               uint64_t& totalNotes,
               uint64_t& totalControls)
{
    SourceCursor cursor(
        source,
        track.offset,
        track.offset + uint64_t(track.size));

    uint32_t tick = 0;
    uint8_t running = 0;
    bool ok = true;

    while (cursor.remaining() != 0 && ok) {
        tick += readVarLen(cursor, ok);

        uint8_t status = 0;
        if (!ok || !readStatus(cursor, running, status))
            return false;

        if (status == 0xff) {
            uint8_t meta = 0;
            if (!cursor.readByte(meta))
                return false;

            const uint32_t length = readVarLen(cursor, ok);
            if (!ok || uint64_t(length) > cursor.remaining())
                return false;

            if (meta == 0x51 && length >= 3) {
                uint8_t tempoBytes[3]{};
                if (!cursor.read(tempoBytes, sizeof(tempoBytes)))
                    return false;

                const uint32_t us =
                    (uint32_t(tempoBytes[0]) << 16) |
                    (uint32_t(tempoBytes[1]) << 8) |
                    uint32_t(tempoBytes[2]);

                output.tempoMap.push_back({
                    tick,
                    us
                });

                if (!cursor.skip(uint64_t(length) - 3u))
                    return false;
            } else if (!cursor.skip(length)) {
                return false;
            }

            if (meta == 0x2f)
                break;

            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            const uint32_t length = readVarLen(cursor, ok);

            if (!ok || !cursor.skip(length))
                return false;

            continue;
        }

        if (status >= 0xf0) {
            const int bytes = systemDataBytes(status);

            if (!cursor.skip(static_cast<uint64_t>(bytes)))
                return false;

            continue;
        }

        const uint8_t command = status & 0xf0;
        const uint8_t channel = status & 0x0f;
        const int dataBytes =
            (command == 0xc0 || command == 0xd0) ? 1 : 2;

        uint8_t data[2]{};
        if (!cursor.read(data, static_cast<std::size_t>(dataBytes)))
            return false;

        const uint8_t data1 = data[0];
        const uint8_t data2 = dataBytes == 2 ? data[1] : 0;

        const bool noteOn =
            command == 0x90 && data2 != 0;

        const bool control =
            command == 0xb0 ||
            command == 0xc0 ||
            command == 0xd0 ||
            command == 0xe0;

        appendTrackCount(
            scan,
            tick,
            noteOn,
            control);

        ++totalEvents;

        if (noteOn) {
            ++totalNotes;
            output.activeChannelMasks[trackIndex] |=
                (1u << channel);

            output.hasPitch = true;
            output.minPitch =
                std::min(output.minPitch, data1);
            output.maxPitch =
                std::max(output.maxPitch, data1);
        } else if (command == 0x80 ||
                   (command == 0x90 && data2 == 0)) {
            output.activeChannelMasks[trackIndex] |=
                (1u << channel);
        }

        if (control)
            ++totalControls;
    }

    if (!ok)
        return false;

    scan.maxTick = tick;
    return true;
}

void buildTempoIndex(MidiDocument& output)
{
    std::stable_sort(
        output.tempoMap.begin(),
        output.tempoMap.end(),
        [](const TempoChange& a, const TempoChange& b) {
            return a.tick < b.tick;
        });

    std::vector<TempoChange> deduped;
    deduped.reserve(output.tempoMap.size());

    for (const auto& tempo : output.tempoMap) {
        if (!deduped.empty() &&
            deduped.back().tick == tempo.tick) {
            deduped.back() = tempo;
        } else {
            deduped.push_back(tempo);
        }
    }

    if (deduped.empty() || deduped.front().tick != 0)
        deduped.insert(deduped.begin(), {0, 500000});

    output.tempoMap.swap(deduped);
    output.tempoSeconds.resize(output.tempoMap.size());

    double seconds = 0.0;
    uint32_t previousTick = 0;
    uint32_t previousUs = 500000;
    const double ppq = std::max<uint16_t>(1, output.ticksPerBeat);

    for (std::size_t i = 0;
         i < output.tempoMap.size();
         ++i) {
        const auto& tempo = output.tempoMap[i];

        seconds +=
            double(tempo.tick - previousTick) /
            ppq *
            (double(previousUs) / 1'000'000.0);

        output.tempoSeconds[i] = seconds;
        previousTick = tempo.tick;

        if (tempo.microsecondsPerBeat != 0)
            previousUs = tempo.microsecondsPerBeat;
    }
}

void buildColorTables(
    const MidiDocument& output,
    std::array<uint8_t, 16>& globalColors,
    std::vector<std::array<uint8_t, 16>>& trackColors)
{
    globalColors.fill(0);

    std::array<int8_t, 16> assigned;
    assigned.fill(-1);

    int color = 0;

    for (uint32_t mask : output.activeChannelMasks) {
        for (int channel = 0; channel < 16; ++channel) {
            if ((mask & (1u << channel)) != 0 &&
                assigned[channel] < 0) {
                assigned[channel] =
                    static_cast<int8_t>(color & 15);
                ++color;
            }
        }
    }

    for (int channel = 0; channel < 16; ++channel) {
        globalColors[channel] =
            assigned[channel] >= 0
                ? static_cast<uint8_t>(assigned[channel])
                : static_cast<uint8_t>(channel);
    }

    trackColors.resize(output.trackCount);

    for (auto& colors : trackColors) {
        for (int channel = 0; channel < 16; ++channel)
            colors[channel] = static_cast<uint8_t>(channel);
    }

    color = 0;

    for (std::size_t track = 0;
         track < output.activeChannelMasks.size();
         ++track) {
        const uint32_t mask = output.activeChannelMasks[track];

        for (int channel = 0; channel < 16; ++channel) {
            if ((mask & (1u << channel)) != 0) {
                trackColors[track][channel] =
                    static_cast<uint8_t>(color & 15);
                ++color;
            }
        }
    }
}

struct HeapNode {
    uint32_t tick = 0;
    uint32_t track = 0;
    uint32_t localIndex = 0;
};

struct HeapGreater {
    bool operator()(const HeapNode& a, const HeapNode& b) const {
        if (a.tick != b.tick)
            return a.tick > b.tick;
        return a.track > b.track;
    }
};

bool buildGlobalTickIndex(
    std::vector<TrackScan>& scans,
    MidiDocument& output,
    uint64_t totalEvents)
{
    if (totalEvents >
        std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    std::priority_queue<
        HeapNode,
        std::vector<HeapNode>,
        HeapGreater> heap;

    for (std::size_t track = 0;
         track < scans.size();
         ++track) {
        if (!scans[track].groups.empty()) {
            heap.push({
                scans[track].groups[0].tick,
                static_cast<uint32_t>(track),
                0
            });
        }
    }

    uint32_t eventOffset = 0;

    while (!heap.empty()) {
        const uint32_t tick = heap.top().tick;
        const uint32_t globalIndex =
            static_cast<uint32_t>(output.tickGroups.size());

        uint64_t eventCount = 0;
        uint64_t noteOnCount = 0;
        uint64_t controlCount = 0;

        while (!heap.empty() &&
               heap.top().tick == tick) {
            const HeapNode node = heap.top();
            heap.pop();

            TrackTickCount& source =
                scans[node.track].groups[node.localIndex];

            source.globalGroupIndex = globalIndex;

            eventCount += source.eventCount;
            noteOnCount += source.noteOnCount;
            controlCount += source.controlCount;

            const uint32_t next =
                node.localIndex + 1;

            if (next < scans[node.track].groups.size()) {
                heap.push({
                    scans[node.track].groups[next].tick,
                    node.track,
                    next
                });
            }
        }

        if (eventCount >
                std::numeric_limits<uint32_t>::max() ||
            uint64_t(eventOffset) + eventCount >
                std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        output.tickGroups.push_back({
            tick,
            eventOffset,
            static_cast<uint32_t>(eventCount),
            static_cast<uint32_t>(
                std::min<uint64_t>(
                    noteOnCount,
                    std::numeric_limits<uint32_t>::max())),
            static_cast<uint32_t>(
                std::min<uint64_t>(
                    controlCount,
                    std::numeric_limits<uint32_t>::max()))
        });

        eventOffset +=
            static_cast<uint32_t>(eventCount);
    }

    return eventOffset == totalEvents;
}

bool parseTrackEvents(
    MidiByteSource& source,
    const TrackRef& track,
    std::size_t trackIndex,
    const TrackScan& scan,
    const std::array<uint8_t, 16>& globalColors,
    const std::vector<std::array<uint8_t, 16>>& trackColors,
    std::vector<uint32_t>& writeCursors,
    std::vector<uint16_t>& eventTracks,
    MidiDocument& output)
{
    SourceCursor cursor(
        source,
        track.offset,
        track.offset + uint64_t(track.size));

    uint32_t tick = 0;
    uint8_t running = 0;
    bool ok = true;
    std::size_t localGroup = 0;

    while (cursor.remaining() != 0 && ok) {
        tick += readVarLen(cursor, ok);

        uint8_t status = 0;

        if (!ok || !readStatus(cursor, running, status))
            return false;

        if (status == 0xff) {
            uint8_t meta = 0;
            if (!cursor.readByte(meta))
                return false;

            const uint32_t length = readVarLen(cursor, ok);

            if (!ok || !cursor.skip(length))
                return false;

            if (meta == 0x2f)
                break;

            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            const uint32_t length = readVarLen(cursor, ok);

            if (!ok || uint64_t(length) > cursor.remaining())
                return false;

            SysExEvent event;
            event.tick = tick;
            event.data.resize(static_cast<std::size_t>(length) + 1);
            event.data[0] = status;

            if (length != 0 &&
                !cursor.read(event.data.data() + 1, length)) {
                return false;
            }

            output.sysEx.push_back(std::move(event));
            continue;
        }

        if (status >= 0xf0) {
            const int bytes = systemDataBytes(status);

            if (!cursor.skip(static_cast<uint64_t>(bytes)))
                return false;

            continue;
        }

        const uint8_t command = status & 0xf0;
        const uint8_t channel = status & 0x0f;
        const int dataBytes =
            (command == 0xc0 || command == 0xd0) ? 1 : 2;

        uint8_t data[2]{};
        if (!cursor.read(data, static_cast<std::size_t>(dataBytes)))
            return false;

        uint8_t data1 = data[0];
        uint8_t data2 = dataBytes == 2 ? data[1] : 0;

        // MIDI convention: NoteOn velocity zero is NoteOff.
        if (command == 0x90 && data2 == 0) {
            status = static_cast<uint8_t>(0x80 | channel);
            data2 = 64;
        }

        while (localGroup < scan.groups.size() &&
               scan.groups[localGroup].tick < tick) {
            ++localGroup;
        }

        if (localGroup >= scan.groups.size() ||
            scan.groups[localGroup].tick != tick) {
            return false;
        }

        const uint32_t groupIndex =
            scan.groups[localGroup].globalGroupIndex;

        if (groupIndex >= writeCursors.size())
            return false;

        const uint32_t position =
            writeCursors[groupIndex]++;

        if (position >= output.events.size())
            return false;

        const uint8_t globalColor =
            globalColors[channel] & 0x0f;

        const uint8_t trackColor =
            trackColors[trackIndex][channel] & 0x0f;

        output.events[position] = {
            status,
            data1,
            data2,
            static_cast<uint8_t>(
                globalColor |
                (trackColor << 4))
        };

        eventTracks[position] =
            static_cast<uint16_t>(
                trackIndex);
    }

    return ok;
}

constexpr uint32_t VisualNoIndex = 0xffffffffu;
constexpr uint32_t VisualHashSize = 65536u;
constexpr uint32_t VisualHashMask = VisualHashSize - 1u;

struct VisualPendingKey {
    uint32_t key = 0;
    uint32_t head = VisualNoIndex;
    uint32_t tail = VisualNoIndex;
    uint32_t nextHash = VisualNoIndex;
};

uint32_t visualKeyHash(uint32_t key)
{
    // Multiplicative hash; the fixed table keeps load-time allocation stable.
    return (key * 2654435761u) & VisualHashMask;
}

uint32_t findOrCreateVisualPendingKey(
    uint32_t key,
    std::array<uint32_t, VisualHashSize>& buckets,
    std::vector<VisualPendingKey>& keys)
{
    const uint32_t bucket =
        visualKeyHash(key);

    uint32_t index =
        buckets[bucket];

    while (index != VisualNoIndex) {
        if (keys[index].key == key)
            return index;

        index =
            keys[index].nextHash;
    }

    const uint32_t newIndex =
        static_cast<uint32_t>(
            keys.size());

    VisualPendingKey pending;
    pending.key = key;
    pending.nextHash =
        buckets[bucket];

    keys.push_back(pending);
    buckets[bucket] = newIndex;

    return newIndex;
}

uint32_t minimumEndTick(
    const MidiDocument& output,
    uint32_t startTick)
{
    const double minEndSeconds =
        output.tickToSeconds(startTick) +
        0.015;

    const double endTick =
        std::ceil(
            output.secondsToTick(
                minEndSeconds));

    return static_cast<uint32_t>(
        std::clamp<double>(
            endTick,
            double(startTick + 1u),
            double(
                std::numeric_limits<uint32_t>::max())));
}

void buildVisualNotes(
    MidiDocument& output,
    const std::vector<uint16_t>& eventTracks,
    const std::vector<TrackScan>& scans)
{
    output.visualNotes.clear();
    output.visualNotes.reserve(
        static_cast<std::size_t>(
            output.noteCount));

    // Build the compressed keyboard NoteOff timeline while notes are paired.
    // The previous implementation first materialized one uint64_t sort key per
    // note (8 * noteCount bytes) and only then compressed it. Large black MIDIs
    // could therefore need another multi-gigabyte allocation after visualNotes
    // had already been built. Here we aggregate closures at each source tick,
    // so memory scales with the final compressed keyboard timeline instead of
    // with the raw note count.
    output.visualKeyEnds.clear();
    output.visualKeyEnds.reserve(
        std::min<std::size_t>(
            static_cast<std::size_t>(output.noteCount),
            std::size_t(1) << 20));

    constexpr std::size_t VisualKeySignatureCount =
        std::size_t(1) << 15;

    std::vector<uint32_t> endCounts(
        VisualKeySignatureCount, 0);
    std::vector<uint32_t> minimumEndCounts(
        VisualKeySignatureCount, 0);
    std::vector<uint16_t> touchedEnds;
    std::vector<uint16_t> touchedMinimumEnds;
    touchedEnds.reserve(1024);
    touchedMinimumEnds.reserve(1024);

    auto keySignatureFromNote =
        [](const VisualNote& note) -> uint16_t {
            const uint32_t pitch =
                (note.packedData >> 8) & 0x7f;
            const uint32_t globalColor =
                (note.packedData >> 16) & 0x0f;
            const uint32_t trackColor =
                (note.packedData >> 20) & 0x0f;
            return static_cast<uint16_t>(
                pitch |
                (globalColor << 7) |
                (trackColor << 11));
        };

    auto packedFromKeySignature =
        [](uint16_t signature) -> uint32_t {
            const uint32_t pitch = signature & 0x7f;
            const uint32_t globalColor =
                (signature >> 7) & 0x0f;
            const uint32_t trackColor =
                (signature >> 11) & 0x0f;
            return pitch |
                (globalColor << 8) |
                (trackColor << 12);
        };

    auto countKeyboardEnd =
        [](std::vector<uint32_t>& counts,
           std::vector<uint16_t>& touched,
           uint16_t signature) {
            if (counts[signature] == 0)
                touched.push_back(signature);
            if (counts[signature] !=
                std::numeric_limits<uint32_t>::max()) {
                ++counts[signature];
            }
        };

    auto flushKeyboardEnds =
        [&](uint32_t tick,
            std::vector<uint32_t>& counts,
            std::vector<uint16_t>& touched) {
            for (uint16_t signature : touched) {
                const uint32_t count = counts[signature];
                if (count != 0) {
                    output.visualKeyEnds.push_back({
                        tick,
                        count,
                        packedFromKeySignature(signature)
                    });
                    counts[signature] = 0;
                }
            }
            touched.clear();
        };

    std::array<uint32_t, VisualHashSize>
        buckets;

    buckets.fill(
        VisualNoIndex);

    std::vector<VisualPendingKey>
        pendingKeys;

    pendingKeys.reserve(
        std::min<std::size_t>(
            static_cast<std::size_t>(
                output.noteCount),
            65536u));

    // MPWGL2 per-track colors are assigned by first appearance in the final
    // start-sorted note stream, not merely by track/channel mask order.
    std::array<uint32_t, VisualHashSize>
        colorBuckets;

    colorBuckets.fill(
        VisualNoIndex);

    struct ColorKey {
        uint32_t key = 0;
        uint32_t nextHash = VisualNoIndex;
        uint8_t color = 0;
    };

    std::vector<ColorKey> colorKeys;
    colorKeys.reserve(256);

    uint32_t nextPerTrackColor = 0;

    auto getPerTrackColor =
        [&](uint16_t track,
            uint8_t channel,
            bool create) -> uint8_t {
            const uint32_t key =
                uint32_t(track) * 16u +
                uint32_t(channel & 15u);

            const uint32_t bucket =
                visualKeyHash(key);

            uint32_t index =
                colorBuckets[bucket];

            while (index != VisualNoIndex) {
                if (colorKeys[index].key == key)
                    return colorKeys[index].color;

                index =
                    colorKeys[index].nextHash;
            }

            if (!create)
                return channel & 15u;

            const uint32_t newIndex =
                static_cast<uint32_t>(
                    colorKeys.size());

            ColorKey entry;
            entry.key = key;
            entry.color =
                static_cast<uint8_t>(
                    nextPerTrackColor++ &
                    15u);

            entry.nextHash =
                colorBuckets[bucket];

            colorKeys.push_back(entry);
            colorBuckets[bucket] =
                newIndex;

            return entry.color;
        };

    for (const TickGroup& group :
         output.tickGroups) {
        // These vectors are flushed at the end of every tick group. A NoteOff
        // can either end at the group tick or, for a zero-length note, at the
        // MPWGL2 minimum-duration tick. All zero-length closures in one group
        // share the same start tick and therefore the same adjusted end tick.
        uint32_t adjustedEndTick = 0;

        const std::size_t begin =
            group.eventOffset;

        const std::size_t end =
            begin + group.eventCount;

        for (std::size_t eventIndex =
                 begin;
             eventIndex < end;
             ++eventIndex) {
            CompactEvent& event =
                output.events[eventIndex];

            const uint8_t command =
                event.status & 0xf0;

            if (command != 0x90 &&
                command != 0x80) {
                continue;
            }

            const uint8_t channel =
                event.status & 0x0f;

            const uint8_t pitch =
                event.data1 & 0x7f;

            const uint16_t track =
                eventTracks[eventIndex];

            const bool noteOn =
                command == 0x90 &&
                event.data2 != 0;

            const uint8_t perTrackColor =
                getPerTrackColor(
                    track,
                    channel,
                    noteOn);

            // Keep the compact runtime stream's per-track color nibble in sync
            // with MPWGL2's first-note-appearance color ordering.
            event.color =
                static_cast<uint8_t>(
                    (event.color & 0x0f) |
                    ((perTrackColor & 0x0f) << 4));

            const uint32_t key =
                (uint32_t(track) << 11) |
                (uint32_t(channel) << 7) |
                uint32_t(pitch);

            const uint32_t pendingIndex =
                findOrCreateVisualPendingKey(
                    key,
                    buckets,
                    pendingKeys);

            VisualPendingKey& queue =
                pendingKeys[pendingIndex];

            if (noteOn) {
                const uint32_t noteIndex =
                    static_cast<uint32_t>(
                        output.visualNotes.size());

                // endTick is temporarily the FIFO next pointer. It is replaced
                // with the actual NoteOff tick when the note closes.
                output.visualNotes.push_back({
                    group.tick,
                    VisualNoIndex,
                    uint32_t(event.data2) |
                    (uint32_t(pitch) << 8) |
                    (uint32_t(
                        event.color &
                        0x0f) << 16) |
                    (uint32_t(
                        perTrackColor &
                        0x0f) << 20) |
                    (uint32_t(
                        channel &
                        0x0f) << 24)
                });

                if (queue.tail !=
                    VisualNoIndex) {
                    output.visualNotes[
                        queue.tail]
                        .endTick =
                            noteIndex;
                } else {
                    queue.head =
                        noteIndex;
                }

                queue.tail =
                    noteIndex;

                continue;
            }

            if (queue.head ==
                VisualNoIndex) {
                continue;
            }

            const uint32_t noteIndex =
                queue.head;

            const uint32_t next =
                output.visualNotes[
                    noteIndex]
                    .endTick;

            queue.head = next;

            if (next ==
                VisualNoIndex) {
                queue.tail =
                    VisualNoIndex;
            }

            VisualNote& note =
                output.visualNotes[
                    noteIndex];

            note.endTick =
                group.tick >
                    note.startTick
                ? group.tick
                : minimumEndTick(
                    output,
                    note.startTick);

            const uint16_t signature =
                keySignatureFromNote(note);

            if (note.endTick == group.tick) {
                countKeyboardEnd(
                    endCounts,
                    touchedEnds,
                    signature);
            } else {
                adjustedEndTick = note.endTick;
                countKeyboardEnd(
                    minimumEndCounts,
                    touchedMinimumEnds,
                    signature);
            }
        }

        flushKeyboardEnds(
            group.tick,
            endCounts,
            touchedEnds);

        if (!touchedMinimumEnds.empty()) {
            // minimumEndTick() depends only on this group's tick for these
            // zero-length notes, so one adjusted tick covers the whole group.
            flushKeyboardEnds(
                adjustedEndTick,
                minimumEndCounts,
                touchedMinimumEnds);
        }
    }

    // MPWGL2 closes orphan NoteOns at that track's EndOfTrack, not at the
    // global file end. Flush every remaining FIFO with the same rule.
    for (const VisualPendingKey& queueInfo :
         pendingKeys) {
        uint32_t noteIndex =
            queueInfo.head;

        if (noteIndex == VisualNoIndex)
            continue;

        const uint16_t track =
            static_cast<uint16_t>(
                queueInfo.key >> 11);

        const uint32_t trackEnd =
            track < scans.size()
                ? scans[track].maxTick
                : output.maxTick;

        uint32_t regularCount = 0;
        uint32_t adjustedCount = 0;
        uint16_t signature = 0;
        uint32_t adjustedTick = 0;
        bool haveSignature = false;

        while (noteIndex !=
               VisualNoIndex) {
            VisualNote& note =
                output.visualNotes[
                    noteIndex];

            const uint32_t next =
                note.endTick;

            note.endTick =
                trackEnd >
                    note.startTick
                ? trackEnd
                : minimumEndTick(
                    output,
                    note.startTick);

            if (!haveSignature) {
                signature =
                    keySignatureFromNote(note);
                haveSignature = true;
            }

            if (note.endTick == trackEnd) {
                if (regularCount !=
                    std::numeric_limits<uint32_t>::max()) {
                    ++regularCount;
                }
            } else {
                adjustedTick = note.endTick;
                if (adjustedCount !=
                    std::numeric_limits<uint32_t>::max()) {
                    ++adjustedCount;
                }
            }

            noteIndex = next;
        }

        if (regularCount != 0) {
            output.visualKeyEnds.push_back({
                trackEnd,
                regularCount,
                packedFromKeySignature(signature)
            });
        }

        if (adjustedCount != 0) {
            output.visualKeyEnds.push_back({
                adjustedTick,
                adjustedCount,
                packedFromKeySignature(signature)
            });
        }
    }

    // Normal NoteOffs were emitted in source-tick order, while zero-length and
    // orphan closures can target slightly later ticks. Sort only the already
    // compressed events, then coalesce duplicates in place. This keeps peak
    // memory bounded by visualNotes + the final keyboard timeline.
    std::sort(
        output.visualKeyEnds.begin(),
        output.visualKeyEnds.end(),
        [](const VisualKeyEvent& a,
           const VisualKeyEvent& b) {
            if (a.tick != b.tick)
                return a.tick < b.tick;
            return a.packedData < b.packedData;
        });

    std::size_t endWrite = 0;
    for (const VisualKeyEvent& event :
         output.visualKeyEnds) {
        if (endWrite != 0) {
            VisualKeyEvent& previous =
                output.visualKeyEnds[endWrite - 1];

            if (previous.tick == event.tick &&
                previous.packedData == event.packedData &&
                uint64_t(previous.count) + event.count <=
                    std::numeric_limits<uint32_t>::max()) {
                previous.count += event.count;
                continue;
            }
        }

        output.visualKeyEnds[endWrite++] = event;
    }
    output.visualKeyEnds.resize(endWrite);

    // The source event stream is globally tick ordered and events at equal
    // ticks were written track-by-track. Therefore visualNotes is already in
    // the same stable start order produced by MPWGL2's final sort.
    output.noteCount =
        output.visualNotes.size();

    output.visualBlockMaxEnd.clear();

    const std::size_t blockSize =
        MidiDocument::VisualSeekBlockSize;

    const std::size_t blockCount =
        (output.visualNotes.size() +
         blockSize - 1) /
        blockSize;

    output.visualBlockMaxEnd.resize(
        blockCount,
        0);

    for (std::size_t i = 0;
         i < output.visualNotes.size();
         ++i) {
        const std::size_t block =
            i / blockSize;

        output.visualBlockMaxEnd[block] =
            std::max(
                output.visualBlockMaxEnd[block],
                output.visualNotes[i].endTick);
    }
}


uint32_t visualKeyPacked(const VisualNote& note)
{
    const uint32_t pitch = (note.packedData >> 8) & 0x7f;
    const uint32_t globalColor = (note.packedData >> 16) & 0x0f;
    const uint32_t trackColor = (note.packedData >> 20) & 0x0f;
    return pitch | (globalColor << 8) | (trackColor << 12);
}

uint32_t visualKeySignature(uint32_t packed)
{
    const uint32_t pitch = packed & 0x7f;
    const uint32_t globalColor = (packed >> 8) & 0x0f;
    const uint32_t trackColor = (packed >> 12) & 0x0f;
    return pitch | (globalColor << 7) | (trackColor << 11);
}

uint32_t packedFromVisualKeySignature(uint32_t signature)
{
    const uint32_t pitch = signature & 0x7f;
    const uint32_t globalColor = (signature >> 7) & 0x0f;
    const uint32_t trackColor = (signature >> 11) & 0x0f;
    return pitch | (globalColor << 8) | (trackColor << 12);
}

void buildVisualKeyIndex(MidiDocument& output)
{
    output.visualKeyStarts.clear();
    // visualKeyEnds is built incrementally during note pairing to avoid a
    // second O(noteCount) allocation on large black MIDIs.
    output.visualKeyOwners.clear();

    const auto& notes = output.visualNotes;
    if (notes.empty())
        return;

    // 7 pitch bits + 4 global color bits + 4 track color bits.
    constexpr std::size_t SignatureCount = std::size_t(1) << 15;
    std::vector<uint32_t> counts(SignatureCount, 0);
    std::vector<uint16_t> touched;
    touched.reserve(1024);

    std::array<uint32_t, 128> ownerPacked{};
    std::array<uint8_t, 128> ownerTouched{};
    std::vector<uint8_t> touchedPitches;
    touchedPitches.reserve(128);

    output.visualKeyStarts.reserve(
        std::min<std::size_t>(notes.size(), 1u << 20));
    output.visualKeyOwners.reserve(
        std::min<std::size_t>(notes.size(), 1u << 18));

    std::size_t begin = 0;
    while (begin < notes.size()) {
        const uint32_t tick = notes[begin].startTick;
        std::size_t end = begin + 1;
        while (end < notes.size() && notes[end].startTick == tick)
            ++end;

        touched.clear();
        touchedPitches.clear();

        for (std::size_t i = begin; i < end; ++i) {
            const uint32_t packed = visualKeyPacked(notes[i]);
            const uint32_t signature = visualKeySignature(packed);

            if (counts[signature] == 0)
                touched.push_back(static_cast<uint16_t>(signature));
            ++counts[signature];

            const uint8_t pitch = static_cast<uint8_t>(packed & 0x7f);
            if (!ownerTouched[pitch]) {
                ownerTouched[pitch] = 1;
                touchedPitches.push_back(pitch);
            }
            // Source order is stable, so this ends as the newest note color.
            ownerPacked[pitch] = packed;
        }

        for (uint16_t signature : touched) {
            output.visualKeyStarts.push_back({
                tick,
                counts[signature],
                packedFromVisualKeySignature(signature)
            });
            counts[signature] = 0;
        }

        for (uint8_t pitch : touchedPitches) {
            output.visualKeyOwners.push_back({
                tick,
                ownerPacked[pitch]
            });
            ownerTouched[pitch] = 0;
        }

        begin = end;
    }

}

void buildDerivedStats(MidiDocument& output)
{
    output.derivedPeakNps = 0;
    output.derivedPeakNpsTime = 0.0f;
    output.derivedPeakPolyphony = 0;
    output.derivedNpsTimeline.clear();

    if (output.durationSeconds > 0.0f) {
        constexpr double TimelineStep = 0.5;
        const std::size_t bucketCount =
            static_cast<std::size_t>(
                std::ceil(double(output.durationSeconds) / TimelineStep)) + 1;

        output.derivedNpsTimeline.assign(bucketCount, 0);

        for (const TickGroup& group : output.tickGroups) {
            if (group.noteOnCount == 0)
                continue;

            const double seconds = output.tickToSeconds(group.tick);
            const std::size_t bucket =
                std::min(
                    bucketCount - 1,
                    static_cast<std::size_t>(
                        std::max(0.0, std::floor(seconds / TimelineStep))));

            const uint64_t sum =
                uint64_t(output.derivedNpsTimeline[bucket]) + group.noteOnCount;
            output.derivedNpsTimeline[bucket] =
                static_cast<uint32_t>(
                    std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max()));
        }
    }

    // MPWGL2 samples peak NPS every 0.1 seconds and counts NoteOns in [t-1,t].
    for (double t = 0.0;
         t <= double(output.durationSeconds) + 0.000001;
         t += 0.1) {
        const double hiTick = output.secondsToTick(t);
        const double loTick =
            output.secondsToTick(std::max(0.0, t - 1.0));
        const std::size_t lo = output.lowerBoundVisualStart(loTick);
        const std::size_t hi = output.upperBoundVisualStart(hiTick);
        const uint64_t count = hi >= lo ? uint64_t(hi - lo) : 0u;

        if (count > output.derivedPeakNps) {
            output.derivedPeakNps = static_cast<uint32_t>(
                std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max()));
            output.derivedPeakNpsTime = static_cast<float>(t);
        }
    }

    struct PolyEdge {
        uint32_t tick = 0;
        int8_t direction = 0;
    };

    constexpr std::size_t MaxPeakNotes = 400000;
    const std::size_t visualCount = output.visualNotes.size();
    const std::size_t stride =
        visualCount > MaxPeakNotes
            ? static_cast<std::size_t>(
                  std::ceil(double(visualCount) / double(MaxPeakNotes)))
            : 1;

    std::vector<PolyEdge> edges;
    edges.reserve(
        (visualCount / std::max<std::size_t>(1, stride) + 1) * 2);

    for (std::size_t i = 0; i < visualCount; i += stride) {
        const VisualNote& note = output.visualNotes[i];
        edges.push_back({note.startTick, +1});
        edges.push_back({note.endTick, -1});
    }

    std::sort(
        edges.begin(),
        edges.end(),
        [](const PolyEdge& a, const PolyEdge& b) {
            if (a.tick != b.tick)
                return a.tick < b.tick;
            // NoteOns before NoteOffs at the same tick, matching MPWGL2.
            return a.direction > b.direction;
        });

    int active = 0;
    int peak = 0;
    for (const PolyEdge& edge : edges) {
        active += edge.direction;
        peak = std::max(peak, active);
    }

    output.derivedPeakPolyphony = static_cast<uint32_t>(std::max(0, peak));
    output.derivedStatsReady = true;
}

} // namespace

double MidiDocument::tickToSeconds(uint32_t tick) const
{
    if (tempoMap.empty() ||
        tempoSeconds.empty()) {
        return
            double(tick) /
            double(std::max<uint16_t>(
                1,
                ticksPerBeat)) *
            0.5;
    }

    const auto it =
        std::upper_bound(
            tempoMap.begin(),
            tempoMap.end(),
            tick,
            [](uint32_t value, const TempoChange& tempo) {
                return value < tempo.tick;
            });

    std::size_t index = 0;

    if (it != tempoMap.begin())
        index =
            static_cast<std::size_t>(
                (it - tempoMap.begin()) - 1);

    index = std::min(
        index,
        tempoSeconds.size() - 1);

    const uint32_t us =
        tempoMap[index].microsecondsPerBeat != 0
            ? tempoMap[index].microsecondsPerBeat
            : 500000;

    return
        tempoSeconds[index] +
        double(tick - tempoMap[index].tick) /
        double(std::max<uint16_t>(1, ticksPerBeat)) *
        (double(us) / 1'000'000.0);
}

double MidiDocument::secondsToTick(double seconds) const
{
    if (seconds <= 0.0)
        return 0.0;

    if (tempoMap.empty() ||
        tempoSeconds.empty()) {
        return
            seconds *
            double(std::max<uint16_t>(
                1,
                ticksPerBeat)) *
            2.0;
    }

    const auto it =
        std::upper_bound(
            tempoSeconds.begin(),
            tempoSeconds.end(),
            seconds);

    std::size_t index = 0;

    if (it != tempoSeconds.begin())
        index =
            static_cast<std::size_t>(
                (it - tempoSeconds.begin()) - 1);

    index = std::min(
        index,
        tempoMap.size() - 1);

    const uint32_t us =
        tempoMap[index].microsecondsPerBeat != 0
            ? tempoMap[index].microsecondsPerBeat
            : 500000;

    return
        double(tempoMap[index].tick) +
        (seconds - tempoSeconds[index]) *
        double(std::max<uint16_t>(1, ticksPerBeat)) /
        (double(us) / 1'000'000.0);
}

std::size_t MidiDocument::lowerBoundGroup(uint32_t tick) const
{
    return static_cast<std::size_t>(
        std::lower_bound(
            tickGroups.begin(),
            tickGroups.end(),
            tick,
            [](const TickGroup& group, uint32_t value) {
                return group.tick < value;
            }) -
        tickGroups.begin());
}

std::size_t MidiDocument::upperBoundGroup(uint32_t tick) const
{
    return static_cast<std::size_t>(
        std::upper_bound(
            tickGroups.begin(),
            tickGroups.end(),
            tick,
            [](uint32_t value, const TickGroup& group) {
                return value < group.tick;
            }) -
        tickGroups.begin());
}

std::size_t MidiDocument::lowerBoundVisualStart(double tick) const
{
    return static_cast<std::size_t>(
        std::lower_bound(
            visualNotes.begin(),
            visualNotes.end(),
            tick,
            [](const VisualNote& note, double value) {
                return double(note.startTick) < value;
            }) -
        visualNotes.begin());
}

std::size_t MidiDocument::upperBoundVisualStart(double tick) const
{
    return static_cast<std::size_t>(
        std::upper_bound(
            visualNotes.begin(),
            visualNotes.end(),
            tick,
            [](double value, const VisualNote& note) {
                return value < double(note.startTick);
            }) -
        visualNotes.begin());
}

static bool parseSource(
    MidiByteSource& source,
    MidiDocument& output,
    MidiParseProgress progress,
    void* progressUser,
    const char*& errorMessage)
{
    auto report =
        [&](int percent, const char* stage) {
            if (progress)
                progress(progressUser, percent, stage);
        };

    report(1, "Validating MIDI");
    output = MidiDocument{};
    errorMessage = "Unknown error";
    source.readFailed = false;

    if (source.size < 14) {
        errorMessage = "MIDI data is too short";
        return false;
    }

    SourceCursor cursor(source, 0, source.size);
    bool ok = true;

    uint8_t chunkId[4]{};
    if (!cursor.read(chunkId, sizeof(chunkId)) ||
        std::memcmp(chunkId, "MThd", 4) != 0) {
        errorMessage = source.readFailed
            ? "Could not read MIDI source"
            : "Missing MThd header";
        return false;
    }

    const uint32_t headerLength =
        read32(cursor, ok);

    if (!ok ||
        headerLength < 6 ||
        uint64_t(headerLength) > cursor.remaining()) {
        errorMessage = source.readFailed
            ? "Could not read MIDI source"
            : "Invalid MIDI header";
        return false;
    }

    const uint64_t headerEnd =
        cursor.position() + uint64_t(headerLength);

    output.format =
        read16(cursor, ok);
    output.trackCount =
        read16(cursor, ok);
    output.ticksPerBeat =
        read16(cursor, ok);

    if (!ok || output.format > 1) {
        errorMessage =
            "Only MIDI Format 0 and 1 are supported";
        return false;
    }

    if (output.ticksPerBeat == 0 ||
        (output.ticksPerBeat & 0x8000)) {
        errorMessage =
            "SMPTE timing is not supported";
        return false;
    }

    if (cursor.position() > headerEnd ||
        !cursor.skip(headerEnd - cursor.position())) {
        errorMessage = "Invalid MIDI header";
        return false;
    }

    std::vector<TrackRef> tracks;
    tracks.reserve(output.trackCount);

    for (uint16_t track = 0;
         track < output.trackCount;
         ++track) {
        if (!cursor.read(chunkId, sizeof(chunkId)) ||
            std::memcmp(chunkId, "MTrk", 4) != 0) {
            errorMessage = source.readFailed
                ? "Could not read MIDI source"
                : "Invalid or truncated MTrk chunk";
            return false;
        }

        const uint32_t length =
            read32(cursor, ok);

        if (!ok || uint64_t(length) > cursor.remaining()) {
            errorMessage = source.readFailed
                ? "Could not read MIDI source"
                : "Truncated MIDI track";
            return false;
        }

        tracks.push_back({
            cursor.position(),
            length
        });

        if (!cursor.skip(length)) {
            errorMessage = "Truncated MIDI track";
            return false;
        }
    }

    report(5, "Indexing tracks");

    output.activeChannelMasks.assign(
        tracks.size(),
        0);

    output.tempoMap.reserve(64);
    output.tempoMap.push_back({
        0,
        500000
    });

    std::vector<TrackScan> scans(
        tracks.size());

    uint64_t totalEvents = 0;
    uint64_t totalNotes = 0;
    uint64_t totalControls = 0;

    // Pass 1: index/count each track. Browser builds use a fixed-size read
    // window over the File/Blob; no raw file-sized allocation exists in WASM.
    for (std::size_t track = 0;
         track < tracks.size();
         ++track) {
        if (!scanTrack(
                source,
                tracks[track],
                track,
                scans[track],
                output,
                totalEvents,
                totalNotes,
                totalControls)) {
            errorMessage = source.readFailed
                ? "Could not read MIDI source"
                : "Malformed MIDI event data";
            output = MidiDocument{};
            return false;
        }

        output.maxTick =
            std::max(
                output.maxTick,
                scans[track].maxTick);

        const int percent =
            5 +
            static_cast<int>(
                30.0 *
                double(track + 1) /
                double(std::max<std::size_t>(1, tracks.size())));
        report(percent, "Indexing tracks");
    }

    report(38, "Building tempo index");
    buildTempoIndex(output);

    std::size_t minimumGlobalGroups = 0;
    for (const auto& scan : scans)
        minimumGlobalGroups =
            std::max(minimumGlobalGroups, scan.groups.size());

    output.tickGroups.reserve(minimumGlobalGroups);

    if (!buildGlobalTickIndex(
            scans,
            output,
            totalEvents)) {
        errorMessage =
            "MIDI contains too many channel events for the current event index";
        output = MidiDocument{};
        return false;
    }

    report(46, "Building global event index");

    output.noteCount = totalNotes;
    output.controlEventCount = totalControls;
    output.durationSeconds =
        static_cast<float>(
            output.tickToSeconds(output.maxTick));

    output.events.resize(
        static_cast<std::size_t>(totalEvents));

    // Temporary only during load. Track identity is required to reproduce
    // MPWGL2's exact (track,channel,pitch) FIFO pairing. It is discarded once
    // immutable visualNotes have been built.
    std::vector<uint16_t> eventTracks(
        static_cast<std::size_t>(totalEvents));

    std::array<uint8_t, 16> globalColors{};
    std::vector<std::array<uint8_t, 16>>
        trackColors;

    buildColorTables(
        output,
        globalColors,
        trackColors);

    std::vector<uint32_t> writeCursors(
        output.tickGroups.size());

    for (std::size_t i = 0;
         i < output.tickGroups.size();
         ++i) {
        writeCursors[i] =
            output.tickGroups[i].eventOffset;
    }

    // Pass 2: revisit the same source ranges and write compact events directly
    // into final tick-group positions. Only the cursor window is resident.
    report(50, "Decoding MIDI events");
    for (std::size_t track = 0;
         track < tracks.size();
         ++track) {
        if (!parseTrackEvents(
                source,
                tracks[track],
                track,
                scans[track],
                globalColors,
                trackColors,
                writeCursors,
                eventTracks,
                output)) {
            errorMessage = source.readFailed
                ? "Could not read MIDI source"
                : "Malformed MIDI event data";
            output = MidiDocument{};
            return false;
        }

        const int percent =
            50 +
            static_cast<int>(
                25.0 *
                double(track + 1) /
                double(std::max<std::size_t>(1, tracks.size())));
        report(percent, "Decoding MIDI events");
    }

    report(78, "Pairing visual notes");
    buildVisualNotes(
        output,
        eventTracks,
        scans);

    report(87, "Finalizing keyboard timeline");
    buildVisualKeyIndex(output);

    report(90, "Analyzing MIDI density");
    buildDerivedStats(output);

    report(93, "Finalizing visual index");

    // Free the temporary 2-byte/event track side-buffer before returning the
    // document to Qt; steady-state RAM remains compact.
    eventTracks.clear();
    eventTracks.shrink_to_fit();

    report(95, "Sorting SysEx events");

    std::stable_sort(
        output.sysEx.begin(),
        output.sysEx.end(),
        [](const SysExEvent& a, const SysExEvent& b) {
            return a.tick < b.tick;
        });

    errorMessage = "";
    report(100, "Parsed");
    return true;
}

bool MidiParser::parse(
    const uint8_t* data,
    std::size_t size,
    MidiDocument& output,
    MidiParseProgress progress,
    void* progressUser)
{
    MidiByteSource source;
    source.size = static_cast<uint64_t>(size);
    source.contiguous = data;

    return parseSource(
        source,
        output,
        progress,
        progressUser,
        errorMessage_);
}

bool MidiParser::parseReadAt(
    uint64_t size,
    MidiReadAt readAt,
    void* readUser,
    MidiDocument& output,
    MidiParseProgress progress,
    void* progressUser)
{
    MidiByteSource source;
    source.size = size;
    source.readAt = readAt;
    source.readUser = readUser;

    if (!readAt) {
        output = MidiDocument{};
        errorMessage_ = "MIDI read callback is unavailable";
        return false;
    }

    return parseSource(
        source,
        output,
        progress,
        progressUser,
        errorMessage_);
}

} // namespace wasmidi
