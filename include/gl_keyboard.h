#ifndef GL_KEYBOARD_H
#define GL_KEYBOARD_H

#include <cstdint>

namespace wasmidi {

class GLKeyboard {
public:
    GLKeyboard();
    ~GLKeyboard();
    
    bool init(int width, int height);
    void shutdown();
    void resize(int width, int height);
    
    void setCurrentTime(float time);
    void setActiveNote(uint8_t note, uint8_t ch, bool active);
    void setDominantColor(uint8_t r, uint8_t g, uint8_t b);
    
    void render();
    
private:
    int width;
    int height;
    float currentTime;
    uint8_t dominantHue;
    
    // Key colors (128 notes)
    struct KeyColor {
        uint8_t r, g, b;
        bool active;
    } keyColors[128];
    
    // WebGL resources
    uint32_t program;
    uint32_t vao;
    uint32_t vbo;
    
    void drawWhiteKeys();
    void drawBlackKeys();
    void drawKeyHighlight(uint8_t note);
};

} // namespace wasmidi

#endif // GL_KEYBOARD_H