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

struct TrackRef {
    const uint8_t* data = nullptr;
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

inline bool need(const uint8_t* p, const uint8_t* end, std::size_t bytes)
{
    return p <= end && static_cast<std::size_t>(end - p) >= bytes;
}

uint16_t read16(const uint8_t*& p, const uint8_t* end, bool& ok)
{
    if (!ok || !need(p, end, 2)) {
        ok = false;
        return 0;
    }

    const uint16_t value =
        (uint16_t(p[0]) << 8) |
        uint16_t(p[1]);

    p += 2;
    return value;
}

uint32_t read32(const uint8_t*& p, const uint8_t* end, bool& ok)
{
    if (!ok || !need(p, end, 4)) {
        ok = false;
        return 0;
    }

    const uint32_t value =
        (uint32_t(p[0]) << 24) |
        (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) |
        uint32_t(p[3]);

    p += 4;
    return value;
}

uint32_t readVarLen(const uint8_t*& p, const uint8_t* end, bool& ok)
{
    uint32_t value = 0;

    for (int i = 0; i < 4; ++i) {
        if (!ok || p >= end) {
            ok = false;
            return 0;
        }

        const uint8_t byte = *p++;
        value = (value << 7) | uint32_t(byte & 0x7f);

        if ((byte & 0x80) == 0)
            return value;
    }

    ok = false;
    return 0;
}

// SMF running status applies to channel messages. System common/SysEx/meta
// cancels it; realtime bytes are tolerated without replacing it.
bool readStatus(const uint8_t*& p,
                const uint8_t* end,
                uint8_t& running,
                uint8_t& status)
{
    if (p >= end)
        return false;

    status = *p;

    if (status < 0x80) {
        if (running == 0)
            return false;
        status = running;
        return true;
    }

    ++p;

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

bool scanTrack(const TrackRef& track,
               std::size_t trackIndex,
               TrackScan& scan,
               MidiDocument& output,
               uint64_t& totalEvents,
               uint64_t& totalNotes,
               uint64_t& totalControls)
{
    const uint8_t* p = track.data;
    const uint8_t* end = p + track.size;

    uint32_t tick = 0;
    uint8_t running = 0;
    bool ok = true;

    while (p < end && ok) {
        tick += readVarLen(p, end, ok);

        uint8_t status = 0;
        if (!ok || !readStatus(p, end, running, status))
            return false;

        if (status == 0xff) {
            if (p >= end)
                return false;

            const uint8_t meta = *p++;
            const uint32_t length = readVarLen(p, end, ok);

            if (!ok || !need(p, end, length))
                return false;

            if (meta == 0x51 && length >= 3) {
                const uint32_t us =
                    (uint32_t(p[0]) << 16) |
                    (uint32_t(p[1]) << 8) |
                    uint32_t(p[2]);

                output.tempoMap.push_back({
                    tick,
                    us
                });
            }

            p += length;

            if (meta == 0x2f)
                break;

            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            const uint32_t length = readVarLen(p, end, ok);

            if (!ok || !need(p, end, length))
                return false;

            p += length;
            continue;
        }

        if (status >= 0xf0) {
            const int bytes = systemDataBytes(status);

            if (!need(p, end, static_cast<std::size_t>(bytes)))
                return false;

            p += bytes;
            continue;
        }

        const uint8_t command = status & 0xf0;
        const uint8_t channel = status & 0x0f;
        const int dataBytes =
            (command == 0xc0 || command == 0xd0) ? 1 : 2;

        if (!need(p, end, static_cast<std::size_t>(dataBytes)))
            return false;

        const uint8_t data1 = *p++;
        const uint8_t data2 = dataBytes == 2 ? *p++ : 0;

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
    const TrackRef& track,
    std::size_t trackIndex,
    const TrackScan& scan,
    const std::array<uint8_t, 16>& globalColors,
    const std::vector<std::array<uint8_t, 16>>& trackColors,
    std::vector<uint32_t>& writeCursors,
    std::vector<uint16_t>& eventTracks,
    MidiDocument& output)
{
    const uint8_t* p = track.data;
    const uint8_t* end = p + track.size;

    uint32_t tick = 0;
    uint8_t running = 0;
    bool ok = true;
    std::size_t localGroup = 0;

    while (p < end && ok) {
        tick += readVarLen(p, end, ok);

        uint8_t status = 0;

        if (!ok || !readStatus(p, end, running, status))
            return false;

        if (status == 0xff) {
            if (p >= end)
                return false;

            const uint8_t meta = *p++;
            const uint32_t length = readVarLen(p, end, ok);

            if (!ok || !need(p, end, length))
                return false;

            p += length;

            if (meta == 0x2f)
                break;

            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            const uint32_t length = readVarLen(p, end, ok);

            if (!ok || !need(p, end, length))
                return false;

            SysExEvent event;
            event.tick = tick;
            event.data.reserve(
                static_cast<std::size_t>(length) + 1);
            event.data.push_back(status);
            event.data.insert(
                event.data.end(),
                p,
                p + length);

            output.sysEx.push_back(std::move(event));
            p += length;
            continue;
        }

        if (status >= 0xf0) {
            const int bytes = systemDataBytes(status);

            if (!need(p, end, static_cast<std::size_t>(bytes)))
                return false;

            p += bytes;
            continue;
        }

        const uint8_t command = status & 0xf0;
        const uint8_t channel = status & 0x0f;
        const int dataBytes =
            (command == 0xc0 || command == 0xd0) ? 1 : 2;

        if (!need(p, end, static_cast<std::size_t>(dataBytes)))
            return false;

        uint8_t data1 = *p++;
        uint8_t data2 = dataBytes == 2 ? *p++ : 0;

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
        }
    }

    // MPWGL2 closes orphan NoteOns at that track's EndOfTrack, not at the
    // global file end. Flush every remaining FIFO with the same rule.
    for (const VisualPendingKey& queueInfo :
         pendingKeys) {
        uint32_t noteIndex =
            queueInfo.head;

        const uint16_t track =
            static_cast<uint16_t>(
                queueInfo.key >> 11);

        const uint32_t trackEnd =
            track < scans.size()
                ? scans[track].maxTick
                : output.maxTick;

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

            noteIndex = next;
        }
    }

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

bool MidiParser::parse(
    const uint8_t* data,
    std::size_t size,
    MidiDocument& output)
{
    output = MidiDocument{};
    errorMessage_ = "Unknown error";

    if (!data || size < 14) {
        errorMessage_ = "MIDI data is too short";
        return false;
    }

    const uint8_t* p = data;
    const uint8_t* end = data + size;
    bool ok = true;

    if (!need(p, end, 4) ||
        std::memcmp(p, "MThd", 4) != 0) {
        errorMessage_ = "Missing MThd header";
        return false;
    }

    p += 4;

    const uint32_t headerLength =
        read32(p, end, ok);

    if (!ok ||
        headerLength < 6 ||
        !need(p, end, headerLength)) {
        errorMessage_ = "Invalid MIDI header";
        return false;
    }

    const uint8_t* headerEnd =
        p + headerLength;

    output.format =
        read16(p, headerEnd, ok);
    output.trackCount =
        read16(p, headerEnd, ok);
    output.ticksPerBeat =
        read16(p, headerEnd, ok);

    if (!ok || output.format > 1) {
        errorMessage_ =
            "Only MIDI Format 0 and 1 are supported";
        return false;
    }

    if (output.ticksPerBeat == 0 ||
        (output.ticksPerBeat & 0x8000)) {
        errorMessage_ =
            "SMPTE timing is not supported";
        return false;
    }

    p = headerEnd;

    std::vector<TrackRef> tracks;
    tracks.reserve(output.trackCount);

    for (uint16_t track = 0;
         track < output.trackCount;
         ++track) {
        if (!need(p, end, 8) ||
            std::memcmp(p, "MTrk", 4) != 0) {
            errorMessage_ =
                "Invalid or truncated MTrk chunk";
            return false;
        }

        p += 4;
        const uint32_t length =
            read32(p, end, ok);

        if (!ok || !need(p, end, length)) {
            errorMessage_ =
                "Truncated MIDI track";
            return false;
        }

        tracks.push_back({
            p,
            length
        });

        p += length;
    }

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

    // Pass 1: index/count each track. There is no NoteEvent allocation,
    // no NoteOn->NoteOff pairing and no global note sort.
    for (std::size_t track = 0;
         track < tracks.size();
         ++track) {
        if (!scanTrack(
                tracks[track],
                track,
                scans[track],
                output,
                totalEvents,
                totalNotes,
                totalControls)) {
            errorMessage_ =
                "Malformed MIDI event data";
            output = MidiDocument{};
            return false;
        }

        output.maxTick =
            std::max(
                output.maxTick,
                scans[track].maxTick);
    }

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
        errorMessage_ =
            "MIDI contains too many channel events for wasm32";
        output = MidiDocument{};
        return false;
    }

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

    // Pass 2: write compact events directly into final tick-group positions.
    for (std::size_t track = 0;
         track < tracks.size();
         ++track) {
        if (!parseTrackEvents(
                tracks[track],
                track,
                scans[track],
                globalColors,
                trackColors,
                writeCursors,
                eventTracks,
                output)) {
            errorMessage_ =
                "Malformed MIDI event data";
            output = MidiDocument{};
            return false;
        }
    }

    buildVisualNotes(
        output,
        eventTracks,
        scans);

    // Free the temporary 2-byte/event track side-buffer before returning the
    // document to Qt; steady-state RAM remains compact.
    eventTracks.clear();
    eventTracks.shrink_to_fit();

    std::stable_sort(
        output.sysEx.begin(),
        output.sysEx.end(),
        [](const SysExEvent& a, const SysExEvent& b) {
            return a.tick < b.tick;
        });

    errorMessage_ = "";
    return true;
}

} // namespace wasmidi
