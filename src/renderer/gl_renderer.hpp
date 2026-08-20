#pragma once

#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <vector>
#include <cstdint>

namespace wasmidi {

struct NoteInstance {
    float start;
    float end;
    float pitch;
    float channel;
};

struct KeyInstance {
    float x, y, w, h;
};

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();
    
    bool initialize(int rollWidth, int rollHeight, int keyboardWidth, int keyboardHeight);
    void resize(int rollWidth, int rollHeight, int keyboardWidth, int keyboardHeight);
    
    void setNotes(const std::vector<NoteInstance>& notes);
    void setCurrentTime(float seconds);
    void setNoteSpeed(float secondsPerWindow);
    void setPostBuffer(float seconds);
    void setChannelColor(uint8_t channel, uint8_t r, uint8_t g, uint8_t b);
    void setActiveNotes(const std::vector<uint8_t>& activePitches);
    
    void renderRoll();
    void renderKeyboard();
    
private:
    int rollWidth_ = 0;
    int rollHeight_ = 0;
    int keyboardWidth_ = 0;
    int keyboardHeight_ = 0;
    
    float currentTime_ = 0.0f;
    float noteSpeed_ = 10.0f;
    float postBuffer_ = 0.0f;
    
    std::vector<NoteInstance> notes_;
    std::vector<uint8_t> activePitches_;
    uint8_t channelColors_[16][3];
    
    GLuint rollProgram_ = 0;
    GLuint keyboardProgram_ = 0;
    GLuint rollVAO_ = 0;
    GLuint rollVBO_ = 0;
    GLuint rollInstanceVBO_ = 0;
    GLuint keyboardVAO_ = 0;
    GLuint keyboardVBO_ = 0;
    GLuint keyboardInstanceVBO_ = 0;
    
    GLint rollTimeUniform_ = -1;
    GLint rollWindowUniform_ = -1;
    GLint rollPostBufferUniform_ = -1;
    GLint rollColorUniform_ = -1;
    GLint rollActiveMultUniform_ = -1;
    GLint rollResolutionUniform_ = -1;
    
    GLint keyboardHueUniform_ = -1;
    GLint keyboardActiveColorUniform_ = -1;
    GLint keyboardIsActiveUniform_ = -1;
    GLint keyboardResolutionUniform_ = -1;
    
    bool createRollProgram();
    bool createKeyboardProgram();
    void uploadNotes();
    void uploadKeyboard();
    void generateKeyboardLayout();
    
    std::vector<KeyInstance> whiteKeys_;
    std::vector<KeyInstance> blackKeys_;
};

}