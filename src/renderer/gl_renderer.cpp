#include "gl_renderer.hpp"
#include <cstring>
#include <cmath>

namespace wasmidi {

namespace {

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        return 0;
    }
    
    return shader;
}

GLuint createProgram(const char* vertSource, const char* fragSource) {
    const GLuint vert = compileShader(GL_VERTEX_SHADER, vertSource);
    const GLuint frag = compileShader(GL_FRAGMENT_SHADER, fragSource);
    
    const GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);
    
    glDeleteShader(vert);
    glDeleteShader(frag);
    
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        return 0;
    }
    
    return program;
}

}

GLRenderer::GLRenderer() {
    memset(channelColors_, 0, sizeof(channelColors_));
}

GLRenderer::~GLRenderer() {
    if (rollProgram_) glDeleteProgram(rollProgram_);
    if (keyboardProgram_) glDeleteProgram(keyboardProgram_);
    if (rollVAO_) glDeleteVertexArrays(1, &rollVAO_);
    if (rollVBO_) glDeleteBuffers(1, &rollVBO_);
    if (rollInstanceVBO_) glDeleteBuffers(1, &rollInstanceVBO_);
    if (keyboardVAO_) glDeleteVertexArrays(1, &keyboardVAO_);
    if (keyboardVBO_) glDeleteBuffers(1, &keyboardVBO_);
    if (keyboardInstanceVBO_) glDeleteBuffers(1, &keyboardInstanceVBO_);
}

bool GLRenderer::initialize(
    int rollWidth, int rollHeight,
    int keyboardWidth, int keyboardHeight
) {
    rollWidth_ = rollWidth;
    rollHeight_ = rollHeight;
    keyboardWidth_ = keyboardWidth;
    keyboardHeight_ = keyboardHeight;
    
    if (!createRollProgram()) {
        return false;
    }
    
    if (!createKeyboardProgram()) {
        return false;
    }
    
    glGenVertexArrays(1, &rollVAO_);
    glGenBuffers(1, &rollVBO_);
    glGenBuffers(1, &rollInstanceVBO_);
    
    glGenVertexArrays(1, &keyboardVAO_);
    glGenBuffers(1, &keyboardVBO_);
    glGenBuffers(1, &keyboardInstanceVBO_);
    
    generateKeyboardLayout();
    
    return true;
}

void GLRenderer::resize(
    int rollWidth, int rollHeight,
    int keyboardWidth, int keyboardHeight
) {
    rollWidth_ = rollWidth;
    rollHeight_ = rollHeight;
    keyboardWidth_ = keyboardWidth;
    keyboardHeight_ = keyboardHeight;
}

void GLRenderer::setNotes(const std::vector<NoteInstance>& notes) {
    notes_ = notes;
    uploadNotes();
}

void GLRenderer::setCurrentTime(float seconds) {
    currentTime_ = seconds;
}

void GLRenderer::setNoteSpeed(float secondsPerWindow) {
    noteSpeed_ = secondsPerWindow;
}

void GLRenderer::setPostBuffer(float seconds) {
    postBuffer_ = seconds;
}

void GLRenderer::setChannelColor(uint8_t channel, uint8_t r, uint8_t g, uint8_t b) {
    if (channel < 16) {
        channelColors_[channel][0] = r;
        channelColors_[channel][1] = g;
        channelColors_[channel][2] = b;
    }
}

void GLRenderer::setActiveNotes(const std::vector<uint8_t>& activePitches) {
    activePitches_ = activePitches;
}

void GLRenderer::renderRoll() {
    glViewport(0, keyboardHeight_, rollWidth_, rollHeight_);
    glClearColor(0.027f, 0.027f, 0.102f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (notes_.empty()) {
        return;
    }
    
    glUseProgram(rollProgram_);
    glBindVertexArray(rollVAO_);
    
    glUniform1f(rollTimeUniform_, currentTime_);
    glUniform1f(rollWindowUniform_, noteSpeed_);
    glUniform1f(rollPostBufferUniform_, postBuffer_);
    glUniform2f(rollResolutionUniform_, static_cast<float>(rollWidth_), static_cast<float>(rollHeight_));
    
    float activeMultiplier = 1.5f;
    glUniform1f(rollActiveMultUniform_, activeMultiplier);
    
    float noteColor[3] = {0.65f, 0.55f, 0.98f};
    glUniform3fv(rollColorUniform_, 1, noteColor);
    
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(notes_.size()));
    
    glBindVertexArray(0);
}

void GLRenderer::renderKeyboard() {
    glViewport(0, 0, keyboardWidth_, keyboardHeight_);
    glClearColor(0.027f, 0.027f, 0.102f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    glUseProgram(keyboardProgram_);
    glBindVertexArray(keyboardVAO_);
    
    float hue = 230.0f;
    glUniform1f(keyboardHueUniform_, hue);
    
    float activeColor[3] = {0.65f, 0.55f, 0.98f};
    glUniform3fv(keyboardActiveColorUniform_, 1, activeColor);
    
    float isActive = activePitches_.empty() ? 0.0f : 1.0f;
    glUniform1f(keyboardIsActiveUniform_, isActive);
    
    glUniform2f(keyboardResolutionUniform_, static_cast<float>(keyboardWidth_), static_cast<float>(keyboardHeight_));
    
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 
        static_cast<GLsizei>(whiteKeys_.size() + blackKeys_.size()));
    
    glBindVertexArray(0);
}

bool GLRenderer::createRollProgram() {
    static const char* vertSource = R"(
        #version 300 es
        precision highp float;
        
        layout(location = 0) in vec2 vertexPosition;
        layout(location = 1) in vec4 noteData;
        
        uniform float uCurrentTime;
        uniform float uWindowSeconds;
        uniform float uPostBuffer;
        uniform vec2 uResolution;
        
        out vec4 vColor;
        out float vActive;
        
        void main() {
            float noteStart = noteData.x;
            float noteEnd = noteData.y;
            float pitch = noteData.z;
            float channel = noteData.w;
            
            float relativeStart = noteStart - uCurrentTime;
            float relativeEnd = noteEnd - uCurrentTime;
            
            float xMin = (relativeStart - uPostBuffer) / uWindowSeconds;
            float xMax = (relativeEnd - uPostBuffer) / uWindowSeconds;
            
            float x = mix(xMin, xMax, vertexPosition.x);
            float y = pitch / 127.0;
            
            vec2 pos = vec2(
                x * 2.0 - 1.0,
                y * 2.0 - 1.0
            );
            
            gl_Position = vec4(pos, 0.0, 1.0);
            
            vActive = float(relativeStart < 0.0 && relativeEnd > 0.0);
            vColor = vec4(1.0, 1.0, 1.0, 1.0);
        }
    )";
    
    static const char* fragSource = R"(
        #version 300 es
        precision mediump float;
        
        in vec4 vColor;
        in float vActive;
        
        uniform vec3 uNoteColor;
        uniform float uActiveMultiplier;
        
        out vec4 fragColor;
        
        void main() {
            float multiplier = vActive > 0.5 ? uActiveMultiplier : 1.0;
            vec3 color = uNoteColor * multiplier;
            fragColor = vec4(color, 0.9);
        }
    )";
    
    rollProgram_ = createProgram(vertSource, fragSource);
    
    if (!rollProgram_) {
        return false;
    }
    
    rollTimeUniform_ = glGetUniformLocation(rollProgram_, "uCurrentTime");
    rollWindowUniform_ = glGetUniformLocation(rollProgram_, "uWindowSeconds");
    rollPostBufferUniform_ = glGetUniformLocation(rollProgram_, "uPostBuffer");
    rollColorUniform_ = glGetUniformLocation(rollProgram_, "uNoteColor");
    rollActiveMultUniform_ = glGetUniformLocation(rollProgram_, "uActiveMultiplier");
    rollResolutionUniform_ = glGetUniformLocation(rollProgram_, "uResolution");
    
    return true;
}

bool GLRenderer::createKeyboardProgram() {
    static const char* vertSource = R"(
        #version 300 es
        precision highp float;
        
        layout(location = 0) in vec2 vertexPosition;
        layout(location = 1) in vec4 keyData;
        
        uniform vec2 uResolution;
        
        out vec4 vColor;
        out float vIsBlack;
        
        void main() {
            float x = keyData.x;
            float y = keyData.y;
            float w = keyData.z;
            float h = keyData.w;
            
            vec2 pos = vec2(
                x + vertexPosition.x * w,
                y + vertexPosition.y * h
            );
            
            pos = pos * 2.0 - 1.0;
            
            gl_Position = vec4(pos, 0.0, 1.0);
            
            vIsBlack = keyData.z < 0.02 ? 1.0 : 0.0;
            vColor = vec4(1.0, 1.0, 1.0, 1.0);
        }
    )";
    
    static const char* fragSource = R"(
        #version 300 es
        precision mediump float;
        
        in vec4 vColor;
        in float vIsBlack;
        
        uniform float uHue;
        uniform vec3 uActiveColor;
        uniform float uIsActive;
        
        out vec4 fragColor;
        
        vec3 hsl2rgb(vec3 c) {
            vec3 rgb = clamp(abs(mod(c.x * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
            return c.z + c.y * (rgb - 0.5) * (1.0 - abs(2.0 * c.z - 1.0));
        }
        
        void main() {
            float lightness = vIsBlack > 0.5 ? 0.15 : 0.5;
            float saturation = 0.3;
            
            vec3 baseColor = hsl2rgb(vec3(uHue / 360.0, saturation, lightness));
            
            if (uIsActive > 0.5) {
                baseColor = mix(baseColor, uActiveColor, 0.7);
            }
            
            fragColor = vec4(baseColor, 1.0);
        }
    )";
    
    keyboardProgram_ = createProgram(vertSource, fragSource);
    
    if (!keyboardProgram_) {
        return false;
    }
    
    keyboardHueUniform_ = glGetUniformLocation(keyboardProgram_, "uHue");
    keyboardActiveColorUniform_ = glGetUniformLocation(keyboardProgram_, "uActiveColor");
    keyboardIsActiveUniform_ = glGetUniformLocation(keyboardProgram_, "uIsActive");
    keyboardResolutionUniform_ = glGetUniformLocation(keyboardProgram_, "uResolution");
    
    return true;
}

void GLRenderer::uploadNotes() {
    if (notes_.empty()) {
        return;
    }
    
    glBindVertexArray(rollVAO_);
    
    glBindBuffer(GL_ARRAY_BUFFER, rollVBO_);
    static const float quadVertices[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, rollInstanceVBO_);
    glBufferData(GL_ARRAY_BUFFER, 
        static_cast<GLsizeiptr>(notes_.size() * sizeof(NoteInstance)),
        notes_.data(), GL_DYNAMIC_DRAW);
    
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(NoteInstance), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);
    
    glBindVertexArray(0);
}

void GLRenderer::uploadKeyboard() {
    glBindVertexArray(keyboardVAO_);
    
    glBindBuffer(GL_ARRAY_BUFFER, keyboardVBO_);
    static const float quadVertices[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    std::vector<KeyInstance> allKeys = whiteKeys_;
    allKeys.insert(allKeys.end(), blackKeys_.begin(), blackKeys.end());
    
    glBindBuffer(GL_ARRAY_BUFFER, keyboardInstanceVBO_);
    glBufferData(GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(allKeys.size() * sizeof(KeyInstance)),
        allKeys.data(), GL_STATIC_DRAW);
    
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(KeyInstance), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);
    
    glBindVertexArray(0);
}

void GLRenderer::generateKeyboardLayout() {
    const int whiteCount = 0;
    const float whiteKeyWidth = 1.0f / 52.0f;
    const float blackKeyWidth = whiteKeyWidth * 0.58f;
    const float whiteKeyHeight = 1.0f;
    const float blackKeyHeight = 0.62f;
    
    const int isBlack[] = {0,1,0,1,0,0,1,0,1,0,1,0};
    int whiteIndex = 0;
    
    for (int octave = 0; octave < 10; ++octave) {
        for (int i = 0; i < 12; ++i) {
            const int note = octave * 12 + i;
            const bool isBlackKey = isBlack[i] != 0;
            
            if (!isBlackKey) {
                const float x = whiteIndex * whiteKeyWidth;
                const float y = 0.0f;
                whiteKeys_.push_back({x, y, whiteKeyWidth, whiteKeyHeight});
                ++whiteIndex;
            } else {
                const float x = (whiteIndex - 1) * whiteKeyWidth + whiteKeyWidth * 0.5f - blackKeyWidth * 0.5f;
                const float y = 1.0f - blackKeyHeight;
                blackKeys_.push_back({x, y, blackKeyWidth, blackKeyHeight});
            }
        }
    }
    
    uploadKeyboard();
}

}