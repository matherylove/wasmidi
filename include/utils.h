#ifndef UTILS_H
#define UTILS_H

#include <cstdint>
#include <cstring>

namespace wasmidi {

// Big-endian reads
inline uint32_t readU32BE(const uint8_t* data, uint32_t& pos) {
    uint32_t v = ((uint32_t)data[pos] << 24) |
                 ((uint32_t)data[pos + 1] << 16) |
                 ((uint32_t)data[pos + 2] << 8) |
                 (uint32_t)data[pos + 3];
    pos += 4;
    return v;
}

inline uint16_t readU16BE(const uint8_t* data, uint32_t& pos) {
    uint16_t v = (data[pos] << 8) | data[pos + 1];
    pos += 2;
    return v;
}

// Variable-length quantity (MIDI)
inline uint32_t readVarLen(const uint8_t* data, uint32_t& pos) {
    uint32_t v = 0;
    uint8_t b;
    do {
        b = data[pos++];
        v = (v << 7) | (b & 0x7F);
    } while (b & 0x80);
    return v;
}

// Clamp
template<typename T>
inline T clamp(T v, T lo, T hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// Tick to seconds conversion
float tickToSec(uint32_t tick, const struct TempoChange* tempoMap,
                int tempoCount, uint32_t ppq);

} // namespace wasmidi

#endif // UTILS_H