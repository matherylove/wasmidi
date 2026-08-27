#ifndef GL_RENDERER_H
#define GL_RENDERER_H

#include "midi_parser.h"
#include <cstdint>

namespace wasmidi {

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();
    
    bool init(int width, int height);
    void shutdown();
    void resize(int width, int height);
    
    void setMidiFile(const MidiFile* file);
    void setCurrentTime(float time);
    void setNoteSpeed(float speed);
    void setPostBuffer(float seconds);
    
    void render();
    
    // Color management
    void setColorForChannel(uint8_t ch, uint8_t r, uint8_t g, uint8_t b);
    void setPerTrackMode(bool enable);
    
private:
    int width;
    int height;
    const MidiFile* midiFile;
    float currentTime;
    float noteSpeed;
    float postBuffer;
    bool perTrackMode;
    
    // WebGL resources
    uint32_t program;
    uint32_t vao;
    uint32_t vbo;
    uint32_t texture;
    uint32_t fbo;
    
    // Shader locations
    int locUTex;
    int locUPhX;
    int locUPhW;
    
    // Render state
    uint32_t texW;
    uint32_t texH;
    uint32_t texFront;
    bool forceFullRedraw;
    
    void initTextures();
    void writeStrip(uint32_t currentTick);
    void scrollAndAdvance(uint32_t currentTick, int32_t delta);
    void renderFullTexture(uint32_t currentTick);
};

} // namespace wasmidi

#endif // GL_RENDERER_H