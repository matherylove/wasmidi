#pragma once

#include <GLES3/gl3.h>

#include "../midi/midi_parser.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
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

    // Called only by the browser visual-cache Worker bridge.
    void receiveVisualPage(uint32_t generation, uint32_t spanTicks,
                           uint32_t pageIndex, const uint32_t* words,
                           uint32_t noteCount, uint32_t sourceCount,
                           double difficulty);

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
    void rebuildCarryCache(uint32_t viewStart, std::size_t desiredBegin);
    void advanceCarryCache(uint32_t viewStart, std::size_t oldBegin, std::size_t desiredBegin);
    void uploadCarryCache();
    void setInstanceBase(std::size_t physicalIndex);

    struct VisualPage {
        uint32_t spanTicks = 0;
        uint32_t pageIndex = 0;
        uint32_t sourceCount = 0;
        double difficulty = 0.0;
        std::vector<VisualNote> notes;
    };

    void resetVisualPageCache(bool reinstallDocument);
    void primeVisualPageCache(uint32_t viewStart, uint32_t viewEnd);
    bool collectCachedPageNotes(uint32_t searchStart, uint32_t viewEnd,
                                std::vector<VisualNote>& output) const;
    bool buildDenseDrawList(uint32_t viewStart, uint32_t viewEnd,
                            std::size_t desiredBegin, std::size_t desiredEnd);
    void uploadDenseDrawList();
    void drawDenseNotes();

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
    GLuint carryVao_ = 0;
    GLuint carryVbo_ = 0;
    GLuint denseVao_ = 0;
    GLuint denseVbo_ = 0;

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
    // Notes whose starts have left the rolling start-time cache but whose ends
    // still intersect the visible history. Kept in source order and drawn first.
    std::vector<VisualNote> carryNotes_;
    // Resolution-dependent dense draw list. It is rebuilt only for MIDI views
    // large enough to benefit, and contains only notes that can contribute at
    // least one currently visible pixel after source-order occlusion.
    std::vector<VisualNote> denseNotes_;
    std::vector<VisualNote> denseSourceScratch_;
    // One bit per snapped horizontal pixel per pitch. Bitset coverage makes
    // full-occlusion queries O(width/64) instead of scanning every pixel for
    // every rejected note at Black-MIDI crashpoints.
    std::vector<uint64_t> denseCoverage_;
    std::unordered_map<uint32_t, VisualPage> visualPages_;
    uint32_t visualCacheGeneration_ = 1;
    uint32_t visualPageSpanTicks_ = 0;
    uint32_t visualWantedFirstPage_ = 0;
    uint32_t visualWantedPageCount_ = 0;
    uint32_t visualCurrentPage_ = 0;
    int visualPrimeWidth_ = 0;
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
