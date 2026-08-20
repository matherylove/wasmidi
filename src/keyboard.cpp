#include "keyboard.hpp"
#include "mainwindow.hpp"

#include <GLES3/gl3.h>
#include <QOpenGLFramebufferObject>
#include <algorithm>
#include <array>
#include <vector>

namespace {

struct KeyInstance {
    float x, y, w, h;
    float black, active;
};

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

GLuint createKeyboardProgram()
{
    static const char* vertex = R"GLSL(#version 300 es
precision highp float;

layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 aRect;
layout(location=2) in vec2 aState;

out vec2 vUv;
out float vBlack;
out float vActive;

void main()
{
    vec2 p = aRect.xy + aCorner * aRect.zw;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);

    vUv = aCorner;
    vBlack = aState.x;
    vActive = aState.y;
}
)GLSL";

    static const char* fragment = R"GLSL(#version 300 es
precision mediump float;

in vec2 vUv;
in float vBlack;
in float vActive;

out vec4 fragColor;

void main()
{
    float sideEdge = min(vUv.x, 1.0 - vUv.x);
    float verticalEdge = min(vUv.y, 1.0 - vUv.y);

    vec3 color;

    if (vBlack > 0.5) {
        // MPWGL2-style black keys: almost-black with a violet/navy top sheen.
        color = mix(
            vec3(0.010, 0.011, 0.023),
            vec3(0.026, 0.028, 0.052),
            vUv.y
        );

        if (sideEdge < 0.055)
            color *= 0.62;
        if (verticalEdge < 0.018)
            color *= 0.72;

        // Subtle lower lip.
        if (vUv.y < 0.045)
            color = mix(color, vec3(0.065, 0.058, 0.100), 0.32);
    } else {
        // The original "white" keys are intentionally dark blue-violet.
        color = mix(
            vec3(0.040, 0.047, 0.085),
            vec3(0.070, 0.080, 0.140),
            vUv.y
        );

        // Crisp separators between adjacent white keys.
        if (sideEdge < 0.030)
            color = mix(color, vec3(0.010, 0.011, 0.023), 0.78);

        // Slight top rim and bottom shadow for the same dimensional feel as
        // the legacy keysCanvas.
        if (vUv.y > 0.975)
            color = mix(color, vec3(0.105, 0.095, 0.150), 0.34);
        if (vUv.y < 0.030)
            color *= 0.70;
    }

    if (vActive > 0.5) {
        vec3 active = vec3(0.54, 0.34, 0.95);
        float glow = vBlack > 0.5 ? 0.88 : 0.76;
        color = mix(color, active, glow);

        if (vUv.y > 0.90)
            color = min(vec3(1.0), color * 1.18 + vec3(0.04));
    }

    fragColor = vec4(color, 1.0);
}
)GLSL";

    const GLuint vs = compileShader(GL_VERTEX_SHADER, vertex);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

class KeyboardRenderer final : public QQuickFramebufferObject::Renderer {
public:
    ~KeyboardRenderer() override
    {
        if (instanceVbo_) glDeleteBuffers(1, &instanceVbo_);
        if (quadVbo_) glDeleteBuffers(1, &quadVbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (program_) glDeleteProgram(program_);
    }

    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject* item) override
    {
        auto* keyboard = static_cast<Keyboard*>(item);
        auto* controller = qobject_cast<MainWindow*>(keyboard->controller());

        width_ = std::max(1, static_cast<int>(keyboard->width()));
        height_ = std::max(1, static_cast<int>(keyboard->height()));
        active_ = controller
            ? controller->activePitchMask()
            : std::array<uint8_t, 128>{};

        dirty_ = true;
    }

    void render() override
    {
        if (!program_)
            initialize();
        if (!program_)
            return;

        if (dirty_)
            rebuildInstances();

        glViewport(0, 0, width_, height_);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0.013f, 0.013f, 0.033f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program_);
        glBindVertexArray(vao_);
        glDrawArraysInstanced(
            GL_TRIANGLES,
            0,
            6,
            static_cast<GLsizei>(instances_.size())
        );
        glBindVertexArray(0);

        update();
    }

private:
    void initialize()
    {
        program_ = createKeyboardProgram();
        if (!program_)
            return;

        static const float quad[] = {
            0.f, 0.f,  1.f, 0.f,  0.f, 1.f,
            0.f, 1.f,  1.f, 0.f,  1.f, 1.f
        };

        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);

        glGenBuffers(1, &quadVbo_);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0, 2, GL_FLOAT, GL_FALSE,
            2 * sizeof(float), nullptr
        );

        glGenBuffers(1, &instanceVbo_);
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            1, 4, GL_FLOAT, GL_FALSE,
            sizeof(KeyInstance), reinterpret_cast<void*>(0)
        );
        glVertexAttribDivisor(1, 1);

        glEnableVertexAttribArray(2);
        glVertexAttribPointer(
            2, 2, GL_FLOAT, GL_FALSE,
            sizeof(KeyInstance),
            reinterpret_cast<void*>(4 * sizeof(float))
        );
        glVertexAttribDivisor(2, 1);

        glBindVertexArray(0);
    }

    void rebuildInstances()
    {
        static const bool blackByPitchClass[12] = {
            false, true, false, true, false, false,
            true, false, true, false, true, false
        };

        instances_.clear();
        instances_.reserve(128);

        int whiteCount = 0;
        for (int note = 0; note < 128; ++note) {
            if (!blackByPitchClass[note % 12])
                ++whiteCount;
        }

        const float whiteWidth = 1.0f / static_cast<float>(whiteCount);
        const float blackWidth = whiteWidth * 0.58f;

        // White keys first.
        int whiteIndex = 0;
        for (int note = 0; note < 128; ++note) {
            if (blackByPitchClass[note % 12])
                continue;

            instances_.push_back({
                whiteIndex * whiteWidth,
                0.0f,
                whiteWidth,
                1.0f,
                0.0f,
                static_cast<float>(active_[note])
            });
            ++whiteIndex;
        }

        // Then black keys so they overlap naturally.
        whiteIndex = 0;
        for (int note = 0; note < 128; ++note) {
            if (blackByPitchClass[note % 12]) {
                const float x =
                    whiteIndex * whiteWidth - blackWidth * 0.5f;

                instances_.push_back({
                    x,
                    0.405f,
                    blackWidth,
                    0.595f,
                    1.0f,
                    static_cast<float>(active_[note])
                });
            } else {
                ++whiteIndex;
            }
        }

        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(
                instances_.size() * sizeof(KeyInstance)
            ),
            instances_.data(),
            GL_DYNAMIC_DRAW
        );

        dirty_ = false;
    }

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint quadVbo_ = 0;
    GLuint instanceVbo_ = 0;

    int width_ = 1;
    int height_ = 1;
    bool dirty_ = true;

    std::array<uint8_t, 128> active_{};
    std::vector<KeyInstance> instances_;
};

} // namespace

Keyboard::Keyboard(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
}

void Keyboard::setController(QObject* controller)
{
    if (controller_ == controller)
        return;

    controller_ = controller;
    emit controllerChanged();
    update();
}

QQuickFramebufferObject::Renderer* Keyboard::createRenderer() const
{
    return new KeyboardRenderer();
}
