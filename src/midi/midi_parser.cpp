#include "midi_parser.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

namespace wasmidi {
namespace {

uint16_t readU16BE(const uint8_t*& p, const uint8_t* end, bool& ok)
{
    if (!ok || end - p < 2) { ok = false; return 0; }
    const uint16_t v = static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
    p += 2;
    return v;
}

uint32_t readU32BE(const uint8_t*& p, const uint8_t* end, bool& ok)
{
    if (!ok || end - p < 4) { ok = false; return 0; }
    const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    p += 4;
    return v;
}

uint32_t readVarLen(const uint8_t*& p, const uint8_t* end, bool& ok)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        if (!ok || p >= end) { ok = false; return 0; }
        const uint8_t byte = *p++;
        value = (value << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0)
            return value;
    }
    ok = false;
    return 0;
}

struct TrackRef { const uint8_t* data = nullptr; std::size_t size = 0; };
struct PendingNote { uint32_t tick = 0; uint8_t velocity = 0; };

uint32_t makeNoteKey(uint8_t channel, uint8_t pitch)
{
    return (uint32_t(channel) << 7) | pitch;
}

bool readStatus(const uint8_t*& p, const uint8_t* end,
                uint8_t& runningStatus, uint8_t& status)
{
    if (p >= end)
        return false;
    status = *p++;
    if (status < 0x80) {
        if (runningStatus == 0)
            return false;
        --p;
        status = runningStatus;
    } else if (status < 0xf0) {
        runningStatus = status;
    }
    return true;
}

bool skipChannelMessage(const uint8_t*& p, const uint8_t* end, uint8_t status)
{
    const uint8_t command = status & 0xf0;
    const std::ptrdiff_t bytes = (command == 0xc0 || command == 0xd0) ? 1 : 2;
    if (end - p < bytes)
        return false;
    p += bytes;
    return true;
}

} // namespace

const char* MidiParser::error() const { return errorMessage_; }

bool MidiParser::parse(const uint8_t* data, std::size_t size, MidiDocument& output)
{
    output = MidiDocument{};
    errorMessage_ = "Unknown error";
    if (!data || size < 14) {
        errorMessage_ = "MIDI data is too short";
        return false;
    }

    bool ok = true;
    const uint8_t* p = data;
    const uint8_t* end = data + size;

    if (std::memcmp(p, "MThd", 4) != 0) {
        errorMessage_ = "Missing MThd header";
        return false;
    }
    p += 4;

    const uint32_t headerSize = readU32BE(p, end, ok);
    if (!ok || headerSize < 6 || std::size_t(end - p) < headerSize) {
        errorMessage_ = "Invalid MIDI header";
        return false;
    }

    output.format = readU16BE(p, end, ok);
    output.trackCount = readU16BE(p, end, ok);
    output.ticksPerBeat = readU16BE(p, end, ok);

    if (!ok || output.format > 1) {
        errorMessage_ = "Only MIDI Format 0 and 1 are supported";
        return false;
    }
    if (output.ticksPerBeat == 0 || (output.ticksPerBeat & 0x8000)) {
        errorMessage_ = "SMPTE timing is not supported";
        return false;
    }

    p = data + 8 + headerSize;

    std::vector<TrackRef> tracks;
    tracks.reserve(output.trackCount);

    for (uint16_t i = 0; i < output.trackCount; ++i) {
        if (end - p < 8 || std::memcmp(p, "MTrk", 4) != 0) {
            errorMessage_ = "Invalid or truncated MTrk chunk";
            return false;
        }

        p += 4;
        const uint32_t len = readU32BE(p, end, ok);
        if (!ok || std::size_t(end - p) < len) {
            errorMessage_ = "Truncated track data";
            return false;
        }

        tracks.push_back({p, len});
        p += len;
    }

    output.activeChannelMasks.assign(tracks.size(), 0);
    output.tempoMap.push_back({0, 500000});

    // Pass 1: complete tempo map, exactly before converting event times.
    for (const auto& track : tracks) {
        const uint8_t* tp = track.data;
        const uint8_t* te = track.data + track.size;
        uint32_t tick = 0;
        uint8_t running = 0;

        while (tp < te && ok) {
            tick += readVarLen(tp, te, ok);

            uint8_t status = 0;
            if (!ok || !readStatus(tp, te, running, status)) {
                ok = false;
                break;
            }

            if (status == 0xff) {
                running = 0;
                if (tp >= te) { ok = false; break; }

                const uint8_t meta = *tp++;
                const uint32_t len = readVarLen(tp, te, ok);
                if (!ok || std::size_t(te - tp) < len) {
                    ok = false;
                    break;
                }

                if (meta == 0x51 && len >= 3) {
                    const uint32_t uspb =
                        (uint32_t(tp[0]) << 16) |
                        (uint32_t(tp[1]) << 8) |
                        uint32_t(tp[2]);
                    output.tempoMap.push_back({tick, uspb});
                }

                tp += len;
                if (meta == 0x2f)
                    break;
                continue;
            }

            if (status == 0xf0 || status == 0xf7) {
                running = 0;
                const uint32_t len = readVarLen(tp, te, ok);
                if (!ok || std::size_t(te - tp) < len) {
                    ok = false;
                    break;
                }
                tp += len;
                continue;
            }

            if (status < 0xf0) {
                if (!skipChannelMessage(tp, te, status)) {
                    ok = false;
                    break;
                }
                continue;
            }

            ok = false;
        }
    }

    if (!ok) {
        errorMessage_ = "Malformed MIDI event data";
        output = MidiDocument{};
        return false;
    }

    std::stable_sort(output.tempoMap.begin(), output.tempoMap.end(),
        [](const TempoChange& a, const TempoChange& b) {
            return a.tick < b.tick;
        });

    std::vector<TempoChange> deduped;
    deduped.reserve(output.tempoMap.size());
    for (const auto& tempo : output.tempoMap) {
        if (!deduped.empty() && deduped.back().tick == tempo.tick)
            deduped.back() = tempo;
        else
            deduped.push_back(tempo);
    }
    output.tempoMap.swap(deduped);

    // Pass 2: notes and channel events using the finalized tempo map.
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const auto& track = tracks[trackIndex];
        const uint8_t* tp = track.data;
        const uint8_t* te = track.data + track.size;
        uint32_t tick = 0;
        uint8_t running = 0;

        // A deque per (channel,pitch) preserves overlapping repeated note-ons,
        // matching the optimized legacy parser.
        std::unordered_map<uint32_t, std::deque<PendingNote>> pending;

        while (tp < te && ok) {
            tick += readVarLen(tp, te, ok);

            uint8_t status = 0;
            if (!ok || !readStatus(tp, te, running, status)) {
                ok = false;
                break;
            }

            if (status == 0xff) {
                running = 0;
                if (tp >= te) { ok = false; break; }

                const uint8_t meta = *tp++;
                const uint32_t len = readVarLen(tp, te, ok);
                if (!ok || std::size_t(te - tp) < len) {
                    ok = false;
                    break;
                }

                tp += len;
                if (meta == 0x2f)
                    break;
                continue;
            }

            if (status == 0xf0 || status == 0xf7) {
                running = 0;
                const uint32_t len = readVarLen(tp, te, ok);
                if (!ok || std::size_t(te - tp) < len) {
                    ok = false;
                    break;
                }
                tp += len;
                continue;
            }

            if (status >= 0xf0) {
                ok = false;
                break;
            }

            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            const int count = (command == 0xc0 || command == 0xd0) ? 1 : 2;

            if (te - tp < count) {
                ok = false;
                break;
            }

            const uint8_t d1 = *tp++;
            const uint8_t d2 = count == 2 ? *tp++ : 0;
            const float seconds = static_cast<float>(
                tickToSeconds(tick, output.tempoMap, output.ticksPerBeat));

            if (command == 0x90 || command == 0x80)
                output.activeChannelMasks[trackIndex] |= (1u << channel);

            if (command == 0x90 && d2 != 0) {
                pending[makeNoteKey(channel, d1)].push_back({tick, d2});
                continue;
            }

            if (command == 0x80 || (command == 0x90 && d2 == 0)) {
                const uint32_t key = makeNoteKey(channel, d1);
                auto it = pending.find(key);

                if (it != pending.end() && !it->second.empty()) {
                    const PendingNote start = it->second.front();
                    it->second.pop_front();
                    if (it->second.empty())
                        pending.erase(it);

                    NoteEvent note;
                    note.startTick = start.tick;
                    note.endTick = tick > start.tick ? tick : start.tick + 1;
                    note.startTime = static_cast<float>(
                        tickToSeconds(start.tick, output.tempoMap,
                                      output.ticksPerBeat));
                    note.endTime = seconds <= note.startTime
                        ? note.startTime + 0.015f
                        : seconds;
                    note.pitch = d1;
                    note.channel = channel;
                    note.velocity = start.velocity;
                    note.track = static_cast<uint16_t>(trackIndex);
                    output.notes.push_back(note);
                }
                continue;
            }

            if (command == 0xa0 || command == 0xb0 ||
                command == 0xc0 || command == 0xd0 || command == 0xe0) {
                output.controls.push_back(
                    {seconds, command, channel, d1, d2});
            }
        }

        const float trackEnd = static_cast<float>(
            tickToSeconds(tick, output.tempoMap, output.ticksPerBeat));

        for (auto& [key, queue] : pending) {
            while (!queue.empty()) {
                const PendingNote start = queue.front();
                queue.pop_front();

                NoteEvent note;
                note.startTick = start.tick;
                note.endTick = tick > start.tick ? tick : start.tick + 1;
                note.startTime = static_cast<float>(
                    tickToSeconds(start.tick, output.tempoMap,
                                  output.ticksPerBeat));
                note.endTime = trackEnd <= note.startTime
                    ? note.startTime + 0.015f
                    : trackEnd;
                note.pitch = static_cast<uint8_t>(key & 0x7f);
                note.channel = static_cast<uint8_t>((key >> 7) & 0x0f);
                note.velocity = start.velocity;
                note.track = static_cast<uint16_t>(trackIndex);
                output.notes.push_back(note);
            }
        }
    }

    if (!ok) {
        errorMessage_ = "Malformed MIDI event data";
        output = MidiDocument{};
        return false;
    }

    std::sort(output.notes.begin(), output.notes.end(),
        [](const NoteEvent& a, const NoteEvent& b) {
            if (a.startTick != b.startTick)
                return a.startTick < b.startTick;
            return a.endTick < b.endTick;
        });

    std::sort(output.controls.begin(), output.controls.end(),
        [](const ControlEvent& a, const ControlEvent& b) {
            return a.time < b.time;
        });

    for (const auto& note : output.notes)
        output.durationSeconds =
            std::max(output.durationSeconds, note.endTime);

    errorMessage_ = "";
    return true;
}

double MidiParser::tickToSeconds(
    uint32_t tick,
    const std::vector<TempoChange>& tempoMap,
    uint16_t ppq) const
{
    if (tempoMap.empty() || ppq == 0)
        return 0.0;

    const auto upper = std::upper_bound(
        tempoMap.begin(), tempoMap.end(), tick,
        [](uint32_t value, const TempoChange& t) {
            return value < t.tick;
        });

    const std::size_t index =
        upper == tempoMap.begin()
            ? 0
            : static_cast<std::size_t>(
                (upper - tempoMap.begin()) - 1);

    double seconds = 0.0;

    for (std::size_t i = 1; i <= index; ++i) {
        const auto& prev = tempoMap[i - 1];
        const auto& cur = tempoMap[i];

        seconds +=
            (double(cur.tick - prev.tick) / double(ppq)) *
            (double(prev.microsecondsPerBeat) / 1'000'000.0);
    }

    const auto& base = tempoMap[index];
    seconds +=
        (double(tick - base.tick) / double(ppq)) *
        (double(base.microsecondsPerBeat) / 1'000'000.0);

    return seconds;
}

} // namespace wasmidi
