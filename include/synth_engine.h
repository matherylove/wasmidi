#ifndef SYNTH_ENGINE_H
#define SYNTH_ENGINE_H

#include "synth_voice.h"
#include "sf2_parser.h"
#include <vector>
#include <set>

namespace wasmidi {

struct ChannelState {
    uint8_t program;
    uint16_t bank;
    uint8_t bankLSB;
    float volume;
    float expression;
    float pan;
    bool sustain;
    float pitchBend;
    bool isDrum;
    
    void reset();
};

struct Limiter {
    bool enabled;
    float threshold;
    float gain;
    float atkCoeff;
    float relCoeff;
    
    void process(float* L, float* R, uint32_t count, float sampleRate);
    void updateAttack(float sampleRate, float sec);
    void updateRelease(float sampleRate, float sec);
};

class SynthEngine {
public:
    SynthEngine();
    ~SynthEngine();
    
    int init(const SF2File* sf2, float sampleRate, uint32_t numVoices = 128);
    void shutdown();
    
    void noteOn(uint8_t ch, uint8_t note, uint8_t vel, float time);
    void noteOff(uint8_t ch, uint8_t note, float time);
    void cc(uint8_t ch, uint8_t cc, uint8_t val, float time);
    void pitchBend(uint8_t ch, uint16_t val, float time);
    void programChange(uint8_t ch, uint8_t prog, float time);
    void reset();
    
    void render(float* outL, float* outR, uint32_t count);
    
    void loadSF2(const SF2File* sf2);
    void setNumVoices(uint32_t n);
    void setMasterVol(float vol);
    
    uint32_t getActiveVoices() const;
    
private:
    const SF2File* sf2;
    float sampleRate;
    uint32_t numVoices;
    uint32_t numLayers;
    float masterVol;
    
    std::vector<Voice> voices;
    std::set<Voice*> activeVoices;
    std::vector<ChannelState> channels;
    Limiter limiter;
    uint32_t voiceAge;
    
    Voice* steal();
    void applyGMReset();
    void dispatchSysEx(const uint8_t* data, uint32_t len);
};

} // namespace wasmidi

#endif // SYNTH_ENGINE_H