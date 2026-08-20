#pragma once

#include <GLES3/gl3.h>
#include <array>
#include <cstdint>
#include <vector>

namespace wasmidi {

struct NoteInstance {
    float start;
    float end;
    float pitch;
    float channel;
    float track;
    float velocity;
};

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    bool initialize();
    void resize(int width, int height);
    void setNotes(const std::vector<NoteInstance>& notes);
    void setCurrentTime(float seconds);
    void setNoteSpeed(float secondsPerWindow);
    void setPostBuffer(float seconds);
    void setPerTrackColors(bool enabled);
    void setChannelColor(uint8_t channel, uint8_t r, uint8_t g, uint8_t b);
    void renderRoll();

private:
    bool createProgram();
    void uploadNotes();
    void destroy();

    int width_ = 1;
    int height_ = 1;
    float currentTime_ = 0.0f;
    float noteSpeed_ = 1.0f;
    float postBuffer_ = 0.0f;
    bool perTrackColors_ = false;
    bool initialized_ = false;
    bool notesDirty_ = false;

    std::vector<NoteInstance> notes_;
    std::array<std::array<float, 3>, 16> channelColors_{};

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint quadVbo_ = 0;
    GLuint instanceVbo_ = 0;

    GLint currentTimeUniform_ = -1;
    GLint windowUniform_ = -1;
    GLint postBufferUniform_ = -1;
    GLint perTrackUniform_ = -1;
    GLint colorsUniform_ = -1;
};

} // namespace wasmidi
