#ifndef SYNTH_VOICE_H
#define SYNTH_VOICE_H

#include <cstdint>

namespace wasmidi {

enum VoiceState {
    V_IDLE = 0,
    V_DELAY,
    V_ATTACK,
    V_HOLD,
    V_DECAY,
    V_SUSTAIN,
    V_RELEASE
};

struct Voice {
    VoiceState state;
    const struct SF2Region* region;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint16_t exClass;
    bool sustainHeld;
    bool quickRelease;
    float envLevel;
    uint32_t voiceAge;
    
    // Phase & pitch
    float basePhaseInc;
    float phaseInc;
    float phase;
    
    // Envelope timing (in samples)
    uint32_t delayLeft;
    uint32_t attackLeft;
    uint32_t holdLeft;
    float decayCoeff;
    float sustainLvl;
    float relCoeff;
    float quickRelCoeff;
    
    // Output gain & pan
    float gainVol;
    float panL;
    float panR;
    
    void noteOn(const SF2Region* region, uint8_t ch, uint8_t note, uint8_t vel,
                float sampleRate, float gain, float panL, float panR, uint32_t age);
    void noteOff();
    void quickRelease();
    void updatePitchBend(float bend);
    bool render(float* outL, float* outR, uint32_t offset, uint32_t count,
                const int16_t* sampleData, uint32_t sampleDataSize);
};

} // namespace wasmidi

#endif // SYNTH_VOICE_H