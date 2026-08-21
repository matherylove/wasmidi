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
    void ensureRingCapacity(std::size_t required);
    void uploadSourceRange(std::size_t begin, std::size_t end);
    void rebuildVisualCache(std::size_t begin, std::size_t end);
    void syncVisualCache(uint32_t viewStart, uint32_t viewEnd);
    void setInstanceBase(std::size_t physicalIndex);

    void calculateView(uint32_t& currentTick,
                       uint32_t& viewStart,
                       uint32_t& viewEnd) const;

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
    bool forceCacheReset_ = true;

    GLuint noteProgram_ = 0;
    GLuint noteVao_ = 0;
    GLuint noteVbo_ = 0;

    GLint viewStartUniform_ = -1;
    GLint viewEndUniform_ = -1;
    GLint currentTickUniform_ = -1;
    GLint viewportWidthUniform_ = -1;
    GLint perTrackUniform_ = -1;
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

    // CPU mirror of only the cached start-ordered visual range.
    std::vector<VisualNote> ring_;
    std::size_t ringCapacity_ = 0;
    std::size_t ringMask_ = 0;

    // Absolute source indices into MidiDocument::visualNotes. Physical VBO
    // location is sourceIndex & ringMask_. This preserves source/draw order.
    std::size_t sourceBegin_ = 0;
    std::size_t sourceEnd_ = 0;

    std::array<NeuralNode, 95> neuralNodes_{};
    std::vector<NeuralLineVertex> neuralLines_;
    std::vector<NeuralPointVertex> neuralPoints_;

    std::chrono::steady_clock::time_point neuralClock_ =
        std::chrono::steady_clock::now();
};

} // namespace wasmidi
