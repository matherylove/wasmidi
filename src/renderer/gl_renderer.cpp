#include "gl_renderer.hpp"

#include <algorithm>
#include <cstring>

namespace wasmidi {
namespace {

GLuint compileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(const char* vertexSource, const char* fragmentSource)
{
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

} // namespace

GLRenderer::GLRenderer()
{
    const float defaults[16][3] = {
        {0.506f,0.549f,0.973f},{0.655f,0.545f,0.980f},{0.753f,0.518f,0.988f},{0.910f,0.475f,0.976f},
        {0.957f,0.447f,0.714f},{0.984f,0.443f,0.522f},{0.984f,0.573f,0.235f},{0.980f,0.800f,0.082f},
        {0.639f,0.902f,0.208f},{0.290f,0.871f,0.502f},{0.204f,0.827f,0.600f},{0.176f,0.831f,0.749f},
        {0.133f,0.827f,0.933f},{0.220f,0.741f,0.973f},{0.376f,0.647f,0.980f},{0.545f,0.361f,0.965f}
    };
    std::memcpy(channelColors_.data(), defaults, sizeof(defaults));
}

GLRenderer::~GLRenderer()
{
    destroy();
}

bool GLRenderer::initialize()
{
    if (initialized_)
        return true;
    if (!createProgram())
        return false;

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    static const float quad[] = {
        0.f, 0.f,  1.f, 0.f,  0.f, 1.f,
        0.f, 1.f,  1.f, 0.f,  1.f, 1.f
    };

    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glGenBuffers(1, &instanceVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(NoteInstance), reinterpret_cast<void*>(0));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(NoteInstance), reinterpret_cast<void*>(4 * sizeof(float)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    initialized_ = true;
    notesDirty_ = true;
    return true;
}

void GLRenderer::destroy()
{
    if (instanceVbo_) glDeleteBuffers(1, &instanceVbo_);
    if (quadVbo_) glDeleteBuffers(1, &quadVbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    instanceVbo_ = quadVbo_ = vao_ = program_ = 0;
    initialized_ = false;
}

void GLRenderer::resize(int width, int height)
{
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void GLRenderer::setNotes(const std::vector<NoteInstance>& notes)
{
    notes_ = notes;
    notesDirty_ = true;
}

void GLRenderer::setCurrentTime(float seconds) { currentTime_ = seconds; }
void GLRenderer::setNoteSpeed(float secondsPerWindow) { noteSpeed_ = std::max(0.1f, secondsPerWindow); }
void GLRenderer::setPostBuffer(float seconds) { postBuffer_ = std::max(0.0f, seconds); }
void GLRenderer::setPerTrackColors(bool enabled) { perTrackColors_ = enabled; }

void GLRenderer::setChannelColor(uint8_t channel, uint8_t r, uint8_t g, uint8_t b)
{
    if (channel >= channelColors_.size())
        return;
    channelColors_[channel] = {r / 255.0f, g / 255.0f, b / 255.0f};
}

bool GLRenderer::createProgram()
{
    static const char* vertex = R"GLSL(#version 300 es
precision highp float;
layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 aNote;      // start, end, pitch, channel
layout(location=2) in vec2 aExtra;     // track, velocity
uniform float uCurrentTime;
uniform float uWindowSeconds;
uniform float uPostBuffer;
uniform bool uPerTrack;
uniform vec3 uColors[16];
out vec3 vColor;
out float vActive;
out float vVelocity;
void main() {
    float start = aNote.x;
    float end = max(aNote.y, start + 0.001);
    float pitch = clamp(aNote.z, 0.0, 127.0);
    float relStart = start - uCurrentTime;
    float relEnd = end - uCurrentTime;
    float hitLine = 0.075;
    float y0 = hitLine + ((relStart + uPostBuffer) / uWindowSeconds) * (1.0 - hitLine);
    float y1 = hitLine + ((relEnd + uPostBuffer) / uWindowSeconds) * (1.0 - hitLine);
    float keyW = 1.0 / 128.0;
    float x0 = pitch * keyW;
    float x1 = x0 + keyW * 0.94;
    float x = mix(x0, x1, aCorner.x);
    float y = mix(y0, y1, aCorner.y);
    gl_Position = vec4(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
    int colorIndex = uPerTrack ? int(mod(aExtra.x, 16.0)) : int(clamp(aNote.w, 0.0, 15.0));
    vColor = uColors[colorIndex];
    vActive = float(start <= uCurrentTime && end >= uCurrentTime);
    vVelocity = clamp(aExtra.y, 0.0, 1.0);
}
)GLSL";

    static const char* fragment = R"GLSL(#version 300 es
precision mediump float;
in vec3 vColor;
in float vActive;
in float vVelocity;
out vec4 fragColor;
void main() {
    float velocityGain = mix(0.58, 1.0, vVelocity);
    vec3 color = vColor * velocityGain;
    if (vActive > 0.5)
        color = min(vec3(1.0), color * 1.35 + vec3(0.08));
    fragColor = vec4(color, 0.94);
}
)GLSL";

    program_ = linkProgram(vertex, fragment);
    if (!program_)
        return false;

    currentTimeUniform_ = glGetUniformLocation(program_, "uCurrentTime");
    windowUniform_ = glGetUniformLocation(program_, "uWindowSeconds");
    postBufferUniform_ = glGetUniformLocation(program_, "uPostBuffer");
    perTrackUniform_ = glGetUniformLocation(program_, "uPerTrack");
    colorsUniform_ = glGetUniformLocation(program_, "uColors[0]");
    return true;
}

void GLRenderer::uploadNotes()
{
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(notes_.size() * sizeof(NoteInstance)),
                 notes_.empty() ? nullptr : notes_.data(),
                 GL_STATIC_DRAW);
    notesDirty_ = false;
}

void GLRenderer::renderRoll()
{
    if (!initialized_ && !initialize())
        return;
    if (notesDirty_)
        uploadNotes();

    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.027f, 0.027f, 0.102f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (notes_.empty())
        return;

    glUseProgram(program_);
    glUniform1f(currentTimeUniform_, currentTime_);
    glUniform1f(windowUniform_, noteSpeed_);
    glUniform1f(postBufferUniform_, postBuffer_);
    glUniform1i(perTrackUniform_, perTrackColors_ ? 1 : 0);
    glUniform3fv(colorsUniform_, 16, channelColors_[0].data());

    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(notes_.size()));
    glBindVertexArray(0);
}

} // namespace wasmidi
