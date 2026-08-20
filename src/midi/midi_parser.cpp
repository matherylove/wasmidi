#include "midi_parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace wasmidi {
namespace {

uint16_t readU16BE(const uint8_t*& p, const uint8_t* end, bool& ok) {
    if (!ok || p + 2 > end) {
        ok = false;
        return 0;
    }
    
    const uint16_t value =
        static_cast<uint16_t>(p[0] << 8) |
        static_cast<uint16_t>(p[1]);
    
    p += 2;
    return value;
}

uint32_t readU32BE(const uint8_t*& p, const uint8_t* end, bool& ok) {
    if (!ok || p + 4 > end) {
        ok = false;
        return 0;
    }
    
    const uint32_t value =
        (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8) |
        static_cast<uint32_t>(p[3]);
    
    p += 4;
    return value;
}

uint32_t readVarLen(const uint8_t*& p, const uint8_t* end, bool& ok) {
    uint32_t value = 0;
    
    for (int i = 0; i < 4; ++i) {
        if (!ok || p >= end) {
            ok = false;
            return 0;
        }
        
        const uint8_t byte = *p++;
        value = (value << 7) | (byte & 0x7f);
        
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    
    ok = false;
    return 0;
}

struct PendingNote {
    uint32_t tick = 0;
    uint8_t velocity = 0;
};

uint32_t makeNoteKey(
    uint16_t track,
    uint8_t channel,
    uint8_t pitch
) {
    return
        (static_cast<uint32_t>(track) << 11) |
        (static_cast<uint32_t>(channel) << 7) |
        pitch;
}

}

const char* MidiParser::error() const {
    return errorMessage_;
}

bool MidiParser::parse(
    const uint8_t* data,
    std::size_t size,
    MidiDocument& output
) {
    output = MidiDocument{};
    errorMessage_ = "Unknown error";
    
    if (!data || size < 14) {
        errorMessage_ = "MIDI data is too short";
        return false;
    }
    
    const uint8_t* cursor = data;
    const uint8_t* end = data + size;
    bool ok = true;
    
    if (std::memcmp(cursor, "MThd", 4) != 0) {
        errorMessage_ = "Missing MThd header";
        return false;
    }
    
    cursor += 4;
    
    const uint32_t headerSize = readU32BE(cursor, end, ok);
    if (!ok || headerSize < 6 || cursor + headerSize > end) {
        errorMessage_ = "Invalid MIDI header";
        return false;
    }
    
    output.format = readU16BE(cursor, end, ok);
    output.trackCount = readU16BE(cursor, end, ok);
    output.ticksPerBeat = readU16BE(cursor, end, ok);
    
    if (!ok || output.ticksPerBeat == 0 ||
        (output.ticksPerBeat & 0x8000) != 0) {
        errorMessage_ = "Unsupported MIDI timing";
        return false;
    }
    
    const uint8_t* headerEnd = data + 8 + headerSize;
    if (cursor < headerEnd) {
        cursor = headerEnd;
    }
    
    struct Track {
        const uint8_t* data;
        std::size_t size;
    };
    
    std::vector<Track> tracks;
    tracks.reserve(output.trackCount);
    
    for (uint16_t i = 0; i < output.trackCount; ++i) {
        if (cursor + 8 > end) {
            errorMessage_ = "Truncated track header";
            return false;
        }
        
        if (std::memcmp(cursor, "MTrk", 4) != 0) {
            errorMessage_ = "Invalid MTrk chunk";
            return false;
        }
        
        cursor += 4;
        const uint32_t trackSize = readU32BE(cursor, end, ok);
        
        if (!ok || cursor + trackSize > end) {
            errorMessage_ = "Truncated track data";
            return false;
        }
        
        tracks.push_back({cursor, trackSize});
        cursor += trackSize;
    }
    
    output.activeChannelMasks.assign(
        tracks.size(),
        0
    );
    
    output.tempoMap.push_back({0, 500000});
    
    for (const auto& track : tracks) {
        const uint8_t* p = track.data;
        const uint8_t* trackEnd = track.data + track.size;
        uint32_t tick = 0;
        uint8_t runningStatus = 0;
        
        while (p < trackEnd && ok) {
            tick += readVarLen(p, trackEnd, ok);
            
            if (!ok || p >= trackEnd) {
                break;
            }
            
            uint8_t status = *p++;
            
            if (status == 0xff) {
                if (p >= trackEnd) {
                    ok = false;
                    break;
                }
                
                const uint8_t metaType = *p++;
                const uint32_t length = readVarLen(p, trackEnd, ok);
                
                if (!ok || p + length > trackEnd) {
                    ok = false;
                    break;
                }
                
                if (metaType == 0x51 && length >= 3) {
                    const uint32_t uspb =
                        (static_cast<uint32_t>(p[0]) << 16) |
                        (static_cast<uint32_t>(p[1]) << 8) |
                        p[2];
                    
                    output.tempoMap.push_back({tick, uspb});
                }
                
                p += length;
                
                if (metaType == 0x2f) {
                    break;
                }
                
                continue;
            }
            
            if (status == 0xf0 || status == 0xf7) {
                const uint32_t length = readVarLen(p, trackEnd, ok);
                
                if (!ok || p + length > trackEnd) {
                    ok = false;
                    break;
                }
                
                p += length;
                runningStatus = 0;
                continue;
            }
            
            if (status < 0x80) {
                if (runningStatus == 0) {
                    ok = false;
                    break;
                }
                --p;
                status = runningStatus;
            } else {
                runningStatus = status;
            }
            
            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            
            if (command == 0xc0 || command == 0xd0) {
                if (p + 1 > trackEnd) {
                    ok = false;
                    break;
                }
                ++p;
                continue;
            }
            
            if (p + 2 > trackEnd) {
                ok = false;
                break;
            }
            
            const uint8_t data1 = *p++;
            const uint8_t data2 = *p++;
            
            if (command == 0x90 || command == 0x80) {
                output.activeChannelMasks[&track - tracks.data()] |=
                    (1u << channel);
            }
            
            if (command == 0xb0 ||
                command == 0xe0 ||
                command == 0xa0) {
                output.controls.push_back({
                    static_cast<float>(
                        tickToSeconds(tick, output.tempoMap, output.ticksPerBeat)
                    ),
                    command,
                    channel,
                    data1,
                    data2
                });
            }
        }
    }
    
    std::sort(
        output.tempoMap.begin(),
        output.tempoMap.end(),
        [](const TempoChange& a, const TempoChange& b) {
            return a.tick < b.tick;
        }
    );
    
    std::unordered_map<uint32_t, PendingNote> pending;
    
    for (std::size_t trackIndex = 0;
         trackIndex < tracks.size();
         ++trackIndex) {
        const auto& track = tracks[trackIndex];
        const uint8_t* p = track.data;
        const uint8_t* trackEnd = track.data + track.size;
        uint32_t tick = 0;
        uint8_t runningStatus = 0;
        
        while (p < trackEnd && ok) {
            tick += readVarLen(p, trackEnd, ok);
            
            if (!ok || p >= trackEnd) {
                break;
            }
            
            uint8_t status = *p++;
            
            if (status == 0xff) {
                if (p >= trackEnd) {
                    ok = false;
                    break;
                }
                
                const uint8_t metaType = *p++;
                const uint32_t length = readVarLen(p, trackEnd, ok);
                
                if (!ok || p + length > trackEnd) {
                    ok = false;
                    break;
                }
                
                p += length;
                
                if (metaType == 0x2f) {
                    break;
                }
                
                continue;
            }
            
            if (status == 0xf0 || status == 0xf7) {
                const uint32_t length = readVarLen(p, trackEnd, ok);
                if (!ok || p + length > trackEnd) {
                    ok = false;
                    break;
                }
                p += length;
                runningStatus = 0;
                continue;
            }
            
            if (status < 0x80) {
                if (runningStatus == 0) {
                    ok = false;
                    break;
                }
                --p;
                status = runningStatus;
            } else {
                runningStatus = status;
            }
            
            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            
            if (command == 0xc0 || command == 0xd0) {
                if (p >= trackEnd) {
                    ok = false;
                    break;
                }
                ++p;
                continue;
            }
            
            if (p + 2 > trackEnd) {
                ok = false;
                break;
            }
            
            const uint8_t data1 = *p++;
            const uint8_t data2 = *p++;
            
            const uint32_t key =
                makeNoteKey(
                    static_cast<uint16_t>(trackIndex),
                    channel,
                    data1
                );
            
            if (command == 0x90 && data2 != 0) {
                pending[key] = {tick, data2};
            } else if (command == 0x80 ||
                       (command == 0x90 && data2 == 0)) {
                const auto it = pending.find(key);
                
                if (it != pending.end()) {
                    NoteEvent note;
                    note.startTime = static_cast<float>(
                        tickToSeconds(
                            it->second.tick,
                            output.tempoMap,
                            output.ticksPerBeat
                        )
                    );
                    note.endTime = static_cast<float>(
                        tickToSeconds(
                            tick,
                            output.tempoMap,
                            output.ticksPerBeat
                        )
                    );
                    note.pitch = data1;
                    note.channel = channel;
                    note.velocity = it->second.velocity;
                    note.track = static_cast<uint16_t>(trackIndex);
                    
                    if (note.endTime <= note.startTime) {
                        note.endTime = note.startTime + 0.015f;
                    }
                    
                    output.notes.push_back(note);
                    pending.erase(it);
                }
            }
        }
        
        const float trackEndSeconds = static_cast<float>(
            tickToSeconds(
                tick,
                output.tempoMap,
                output.ticksPerBeat
            )
        );
        
        for (auto it = pending.begin(); it != pending.end();) {
            const uint32_t key = it->first;
            const uint16_t track =
                static_cast<uint16_t>((key >> 11) & 0x1ff);
            
            if (track == trackIndex) {
                NoteEvent note;
                note.startTime = static_cast<float>(
                    tickToSeconds(
                        it->second.tick,
                        output.tempoMap,
                        output.ticksPerBeat
                    )
                );
                note.endTime = trackEndSeconds;
                note.pitch = static_cast<uint8_t>(key & 0x7f);
                note.channel = static_cast<uint8_t>((key >> 7) & 0x0f);
                note.velocity = it->second.velocity;
                note.track = track;
                
                if (note.endTime <= note.startTime) {
                    note.endTime = note.startTime + 0.015f;
                }
                
                output.notes.push_back(note);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    if (!ok) {
        errorMessage_ = "Malformed MIDI event data";
        output = MidiDocument{};
        return false;
    }
    
    std::sort(
        output.notes.begin(),
        output.notes.end(),
        [](const NoteEvent& a, const NoteEvent& b) {
            return a.startTime < b.startTime;
        }
    );
    
    for (const auto& note : output.notes) {
        output.durationSeconds =
            std::max(output.durationSeconds, note.endTime);
    }
    
    return true;
}

double MidiParser::tickToSeconds(
    uint32_t tick,
    const std::vector<TempoChange>& tempoMap,
    uint16_t ppq
) const {
    if (tempoMap.empty() || ppq == 0) {
        return 0.0;
    }
    
    std::size_t index = 0;
    
    for (std::size_t i = 1; i < tempoMap.size(); ++i) {
        if (tempoMap[i].tick > tick) {
            break;
        }
        index = i;
    }
    
    double seconds = 0.0;
    
    for (std::size_t i = 1; i <= index; ++i) {
        const auto& previous = tempoMap[i - 1];
        const auto& current = tempoMap[i];
        
        const double beats =
            static_cast<double>(current.tick - previous.tick) /
            static_cast<double>(ppq);
        
        seconds += beats *
            static_cast<double>(previous.microsecondsPerBeat) /
            1'000'000.0;
    }
    
    const auto& base = tempoMap[index];
    
    seconds +=
        static_cast<double>(tick - base.tick) /
        static_cast<double>(ppq) *
        static_cast<double>(base.microsecondsPerBeat) /
        1'000'000.0;
    
    return seconds;
}

}