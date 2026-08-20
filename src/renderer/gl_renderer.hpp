#pragma once

#include <GLES3/gl3.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "../midi/midi_parser.hpp"

namespace wasmidi {

struct TempoPoint {
    uint32_t tick = 0;
    uint32_t microsecondsPerBeat = 500000;
};

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    bool initialize();
    void resize(int width, int height);

    void setNotesView(const std::vector<NoteEvent>* notes);
    void setTempoMap(const std::vector<TempoPoint>& tempoMap,
                     uint16_t ticksPerBeat);
    void setActiveChannelMasks(
        const std::vector<uint32_t>& activeChannelMasks);

    void setCurrentTime(float seconds);
    void setNoteSpeed(float secondsPerWindow);
    void setPostBuffer(float seconds);
    void setPerTrackColors(bool enabled);
    void setChannelColor(uint8_t channel,
                         uint8_t r, uint8_t g, uint8_t b);

    void renderRoll();

private:
    bool createPrograms();
    void destroy();
    void destroyTextures();
    void initTextures(int width, int height);

    void rebuildTempoIndex();
    double secToTick(double seconds) const;
    void recalcTickWindow();
    double ticksPerColumn() const;

    std::size_t lowerBoundTick(double tick) const;
    std::size_t upperBoundTick(double tick) const;

    void rebuildColorMaps();
    uint8_t colorIndexFor(uint16_t track, uint8_t channel) const;

    void writeStrip(std::vector<uint8_t>& buffer,
                    int columnStart,
                    int columnCount,
                    double currentTick);
    void renderFullTexture(double currentTick);
    void scrollAndAdvance(double currentTick, int deltaColumns);
    void drawFrontTexture(GLint targetFramebuffer);

    int width_ = 1;
    int height_ = 1;
    int textureWidth_ = 0;
    int textureHeight_ = 0;

    float currentTime_ = 0.0f;
    float noteSpeed_ = 1.0f;
    float postBuffer_ = 0.0f;
    bool perTrackColors_ = false;

    bool initialized_ = false;
    bool forceFullRedraw_ = true;

    const std::vector<NoteEvent>* notes_ = nullptr;
    std::vector<TempoPoint> tempoMap_;
    std::vector<uint32_t> activeChannelMasks_;

    uint16_t ppq_ = 480;

    std::vector<double> tempoTicks_;
    std::vector<double> tempoSeconds_;
    std::vector<double> tempoUsPerBeat_;

    double windowTicks_ = 960.0;
    double postTicks_ = 0.0;
    double lastRenderTick_ = -1.0;

    std::array<std::array<uint8_t, 4>, 16> channelColors_{};
    std::array<int8_t, 16> globalChannelColor_{};
    std::unordered_map<uint32_t, uint8_t> perTrackColor_;

    GLuint scrollProgram_ = 0;
    GLuint blitProgram_ = 0;
    GLuint emptyVao_ = 0;

    GLuint textures_[2] = {0, 0};
    GLuint framebuffers_[2] = {0, 0};
    int frontIndex_ = 0;

    GLint scrollTexUniform_ = -1;
    GLint playheadXUniform_ = -1;
    GLint playheadWidthUniform_ = -1;

    GLint blitSourceUniform_ = -1;
    GLint blitOffsetUniform_ = -1;

    std::vector<uint8_t> fullBuffer_;
    std::vector<uint8_t> stripBuffer_;
};

} // namespace wasmidi
