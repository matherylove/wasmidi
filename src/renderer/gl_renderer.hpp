#pragma once

#include <GLES3/gl3.h>

#include "../midi/midi_parser.hpp"

#include <array>
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

    void renderRoll();

private:
    struct RenderNote {
        int32_t startTick = 0;
        int32_t endTick = 0;      // 0 while held
        uint32_t packedData = 0;  // velocity | pitch<<8 | color<<16
    };
    static_assert(sizeof(RenderNote) == 12,
                  "RenderNote must remain 12 bytes");

    bool createProgram();
    void destroy();

    void allocateRing(std::size_t capacity);
    void ensureRingSpace();
    void resetSweep();

    void sweepRange(uint32_t fromTick, uint32_t toTick);
    void processEvent(const CompactEvent& event, uint32_t tick);
    void appendNote(uint32_t tick,
                    uint8_t pitch,
                    uint8_t velocity,
                    uint8_t color);
    void closeActiveNote(std::size_t stateIndex, uint32_t tick);

    void uploadAbsoluteRange(int64_t begin, int64_t end);
    void flushEndTickUpdates();
    void setInstanceBase(std::size_t physicalIndex);

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

    const MidiDocument* document_ = nullptr;

    bool initialized_ = false;
    bool paletteDirty_ = true;
    bool forceSweepReset_ = true;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint instanceVbo_ = 0;

    GLint viewStartUniform_ = -1;
    GLint viewEndUniform_ = -1;
    GLint currentTickUniform_ = -1;
    GLint paletteUniform_ = -1;

    std::array<std::array<uint8_t, 4>, 16>
        channelColors_{};

    std::vector<RenderNote> ring_;
    std::size_t ringCapacity_ = 0;
    std::size_t ringMask_ = 0;

    // Absolute monotonically increasing IDs. Physical VBO position is id&mask.
    int64_t head_ = 0;
    int64_t tail_ = 0;

    std::array<uint32_t, 16 * 128> activeCount_{};
    std::array<uint8_t, 16 * 128> activeColor_{};
    std::array<int64_t, 16 * 128> activeNoteId_{};

    std::vector<uint32_t> dirtyEndIndices_;

    int64_t lastSweepEnd_ = -1;
    uint32_t lastViewSpan_ = 0;
};

} // namespace wasmidi
