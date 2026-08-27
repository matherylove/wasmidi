#ifndef SF2_PARSER_H
#define SF2_PARSER_H

#include <cstdint>
#include <vector>
#include <string>

namespace wasmidi {

struct SF2Sample {
    std::string name;
    uint32_t start;
    uint32_t end;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t sampleRate;
    uint8_t originalKey;
    int8_t correction;
    const int16_t* data;  // Pointer to sample data in buffer
};

struct SF2Region {
    uint16_t bank;
    uint16_t preset;
    uint8_t keyLo;
    uint8_t keyHi;
    uint8_t velLo;
    uint8_t velHi;
    uint8_t rootKey;
    int16_t coarseTune;
    int16_t fineTune;
    uint16_t scaleTune;
    float gain;
    float pan;
    uint16_t loopMode;
    uint16_t exClass;
    float volDelay;
    float volAttack;
    float volHold;
    float volDecay;
    float volSustain;
    float volRelease;
    SF2Sample sample;
};

struct SF2Preset {
    std::string name;
    uint16_t preset;
    uint16_t bank;
    std::vector<SF2Region> regions;
};

struct SF2File {
    std::vector<SF2Preset> presets;
    const int16_t* sampleData;  // Pointer to raw sample buffer
    uint32_t sampleDataSize;
};

// Load SF2 from memory buffer
int sf2_load(const uint8_t* data, uint32_t dataLen, SF2File& out);

// Free SF2 resources
void sf2_free(SF2File& file);

} // namespace wasmidi

#endif // SF2_PARSER_H