#pragma once

#include <GLES3/gl3.h>

#include "../midi/midi_parser.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wasmidi {

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    bool initialize();
    void resize(int width, int height);

    void setDocument(const MidiDocument* document);
    void setCurrentTime(float seconds);
    void setNoteSpeed(float secondsPerWindow);
    void setPostBuffer(float seconds);
    void setPerTrackColors(bool enabled);
    void setChannelColor(uint8_t channel,
                         uint8_t r,
                         uint8_t g,
                         uint8_t b);
    void setNeuralVisual(float hue, float activity);

    void renderRoll();

private:
    struct RenderNote {
        int32_t startTick = 0;
        int32_t endTick = 0;
        uint32_t packedData = 0;
    };
    static_assert(sizeof(RenderNote) == 12,
                  "RenderNote must remain 12 bytes");

    struct NeuralNode {
        float x = 0.0f;
        float y = 0.0f;
        float radius = 1.0f;
        float pulse = 0.0f;
        float pulseSpeed = 0.01f;
        float steer = 0.0f;
        float steerSpeed = 0.004f;
    };

    struct NeuralLineVertex {
        float x = 0.0f;
        float y = 0.0f;
        float alpha = 0.0f;
    };

    struct NeuralPointVertex {
        float x = 0.0f;
        float y = 0.0f;
        float size = 1.0f;
        float alpha = 1.0f;
    };

    bool createPrograms();
    void destroy();

    void initializeNeuralNodes();
    void updateNeuralNodes();
    void renderBackground();

    void allocateRing(std::size_t capacity);
    void ensureRingSpace();
    void reserveForSweep(uint32_t fromTick, uint32_t toTick);
    void resetSweep();

    void sweepRange(uint32_t fromTick, uint32_t toTick);
    void processEvent(const CompactEvent& event, uint32_t tick);
    void beginActiveNote(std::size_t stateIndex,
                         uint32_t tick,
                         uint8_t pitch,
                         uint8_t velocity,
                         uint8_t color);
    void finishActiveNote(std::size_t stateIndex, uint32_t tick);
    void appendCompleted(int32_t startTick,
                         int32_t endTick,
                         uint32_t packedData);

    void uploadCompletedRange(int64_t begin, int64_t end);
    void rebuildOpenNotes();
    void uploadOpenNotes();

    void setCompletedInstanceBase(std::size_t physicalIndex);

    void advanceTail(uint32_t viewStart);
    void calculateView(uint32_t& currentTick,
                       uint32_t& viewStart,
                       uint32_t& viewEnd,
                       uint32_t& sweepEnd) const;

    uint8_t eventColor(const CompactEvent& event) const;

    int width_ = 1;
    int height_ = 1;

    float currentTime_ = 0.0f;
    float noteSpeed_ = 1.0f;
    float postBuffer_ = 0.0f;
    bool perTrackColors_ = false;

    float neuralHue_ = 230.0f;
    float neuralTargetHue_ = 230.0f;
    float neuralActivity_ = 0.0f;

    const MidiDocument* document_ = nullptr;

    bool initialized_ = false;
    bool paletteDirty_ = true;
    bool forceSweepReset_ = true;
    bool openDirty_ = true;

    GLuint noteProgram_ = 0;
    GLuint completedVao_ = 0;
    GLuint completedVbo_ = 0;
    GLuint openVao_ = 0;
    GLuint openVbo_ = 0;

    GLint viewStartUniform_ = -1;
    GLint viewEndUniform_ = -1;
    GLint currentTickUniform_ = -1;
    GLint paletteUniform_ = -1;

    GLuint backgroundProgram_ = 0;
    GLuint backgroundVao_ = 0;
    GLint backgroundHueUniform_ = -1;
    GLint backgroundActivityUniform_ = -1;
    GLint backgroundAspectUniform_ = -1;

    GLuint neuralProgram_ = 0;
    GLuint neuralLineVao_ = 0;
    GLuint neuralLineVbo_ = 0;
    GLuint neuralPointVao_ = 0;
    GLuint neuralPointVbo_ = 0;
    GLint neuralHueUniform_ = -1;
    GLint neuralPointModeUniform_ = -1;

    std::array<std::array<uint8_t, 4>, 16>
        channelColors_{};

    std::vector<RenderNote> ring_;
    std::size_t ringCapacity_ = 0;
    std::size_t ringMask_ = 0;

    int64_t head_ = 0;
    int64_t tail_ = 0;

    // The visual merge state is bounded to MIDI's 16*128 channel/pitch states.
    // Open notes live here, not in the completed-note VBO, so NoteOff never
    // requires a random glBufferSubData update.
    std::array<uint32_t, 16 * 128> activeCount_{};
    std::array<uint8_t, 16 * 128> activeColor_{};
    std::array<int32_t, 16 * 128> activeStartTick_{};
    std::array<uint32_t, 16 * 128> activePackedData_{};

    std::vector<RenderNote> openNotes_;

    std::array<NeuralNode, 95> neuralNodes_{};
    std::vector<NeuralLineVertex> neuralLines_;
    std::vector<NeuralPointVertex> neuralPoints_;

    std::chrono::steady_clock::time_point neuralClock_ =
        std::chrono::steady_clock::now();

    int64_t lastSweepEnd_ = -1;
    uint32_t lastViewSpan_ = 0;
};

} // namespace wasmidi
