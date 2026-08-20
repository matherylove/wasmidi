#ifndef UI_H
#define UI_H

#include <cstdint>
#include <functional>

namespace wasmidi {

struct Button {
    float x, y, w, h;
    const char* label;
    bool hovered;
    bool pressed;
    std::function<void()> onClick;
};

struct Slider {
    float x, y, w, h;
    float value;
    float minVal;
    float maxVal;
    const char* label;
    std::function<void(float)> onValueChange;
};

struct Panel {
    float x, y, w, h;
    const char* title;
    bool visible;
};

class UI {
public:
    UI();
    ~UI();
    
    void init();
    void shutdown();
    
    void handleMouseMove(float x, float y);
    void handleMouseDown(float x, float y);
    void handleMouseUp(float x, float y);
    void handleKeyDown(uint32_t key);
    
    void render();
    
    // Factory methods
    Button& addButton(float x, float y, float w, float h, const char* label);
    Slider& addSlider(float x, float y, float w, float h, float min, float max, const char* label);
    Panel& addPanel(float x, float y, float w, float h, const char* title);
    
private:
    std::vector<Button> buttons;
    std::vector<Slider> sliders;
    std::vector<Panel> panels;
    
    Button* hoveredButton;
    Button* pressedButton;
    Slider* draggedSlider;
};

} // namespace wasmidi

#endif // UI_H