#include "keyboard.hpp"
#include "mainwindow.hpp"

#include <GLES3/gl3.h>

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <algorithm>
#include <array>
#include <vector>

namespace {

struct KeyInstance {
    float x, y, w, h, black, active;
};

GLuint shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        glDeleteShader(s);
        return 0;
    }

    return s;
}

GLuint program()
{
    static const char* vs = R"GLSL(#version 300 es
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

    static const char* fs = R"GLSL(#version 300 es
precision mediump float;

in vec2 vUv;
in float vBlack;
in float vActive;

out vec4 fragColor;

void main()
{
    vec3 c;

    if (vBlack > 0.5) {
        c = mix(
            vec3(.035,.037,.070),
            vec3(.008,.009,.020),
            vUv.y);
        if (vUv.y > .94)
            c *= .72;
    } else {
        c = mix(
            vec3(.075,.085,.145),
            vec3(.035,.042,.076),
            vUv.y);

        float edge =
            min(vUv.x, 1.0 - vUv.x);

        if (edge < .035)
            c *= .58;

        if (vUv.y < .10)
            c = mix(
                c,
                vec3(.11,.10,.16),
                .22);
    }

    if (vActive > 0.5) {
        c = mix(
            c,
            vec3(.62,.46,.98),
            vBlack > 0.5 ? .90 : .78);

        if (vUv.y < .14)
            c = min(
                vec3(1.0),
                c * 1.20 + vec3(.04));
    }

    fragColor = vec4(c,1);
}
)GLSL";

    GLuint v =
        shader(GL_VERTEX_SHADER, vs);
    GLuint f =
        shader(GL_FRAGMENT_SHADER, fs);

    if (!v || !f)
        return 0;

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);

    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok = 0;
    glGetProgramiv(
        p, GL_LINK_STATUS, &ok);

    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }

    return p;
}

class KeyboardRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    ~KeyboardRenderer() override
    {
        if (vbo_)
            glDeleteBuffers(1, &vbo_);
        if (inst_)
            glDeleteBuffers(1, &inst_);
        if (vao_)
            glDeleteVertexArrays(1, &vao_);
        if (prog_)
            glDeleteProgram(prog_);
    }

    QOpenGLFramebufferObject*
    createFramebufferObject(
        const QSize& size) override
    {
        return new QOpenGLFramebufferObject(size);
    }

    void synchronize(
        QQuickFramebufferObject* item) override
    {
        auto* keyboard =
            static_cast<Keyboard*>(item);

        auto* controller =
            qobject_cast<MainWindow*>(
                keyboard->controller());

        width_ =
            std::max(
                1,
                int(keyboard->width()));

        height_ =
            std::max(
                1,
                int(keyboard->height()));

        active_ = controller
            ? controller->activePitchMask()
            : std::array<uint8_t,128>{};

        dirty_ = true;
    }

    void render() override
    {
        if (!prog_)
            init();

        if (!prog_)
            return;

        if (dirty_)
            rebuild();

        glViewport(
            0, 0,
            width_, height_);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        glClearColor(
            .010f, .010f, .026f, 1.0f);

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(prog_);
        glBindVertexArray(vao_);

        glDrawArraysInstanced(
            GL_TRIANGLES,
            0,
            6,
            GLsizei(keys_.size()));

        glBindVertexArray(0);

        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    void init()
    {
        prog_ = program();

        if (!prog_)
            return;

        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);

        const float q[] = {
            0,0, 1,0, 0,1,
            0,1, 1,0, 1,1
        };

        glGenBuffers(1, &vbo_);
        glBindBuffer(
            GL_ARRAY_BUFFER, vbo_);

        glBufferData(
            GL_ARRAY_BUFFER,
            sizeof(q),
            q,
            GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);

        glVertexAttribPointer(
            0, 2,
            GL_FLOAT,
            GL_FALSE,
            0,
            nullptr);

        glGenBuffers(1, &inst_);
        glBindBuffer(
            GL_ARRAY_BUFFER, inst_);

        glEnableVertexAttribArray(1);

        glVertexAttribPointer(
            1, 4,
            GL_FLOAT,
            GL_FALSE,
            sizeof(KeyInstance),
            reinterpret_cast<void*>(0));

        glVertexAttribDivisor(1, 1);

        glEnableVertexAttribArray(2);

        glVertexAttribPointer(
            2, 2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(KeyInstance),
            reinterpret_cast<void*>(
                4 * sizeof(float)));

        glVertexAttribDivisor(2, 1);

        glBindVertexArray(0);
    }

    void rebuild()
    {
        static const bool black[12] = {
            false,true,false,true,false,false,
            true,false,true,false,true,false
        };

        keys_.clear();
        keys_.reserve(128);

        int whiteTotal = 0;

        for (int n = 0; n < 128; ++n) {
            if (!black[n % 12])
                ++whiteTotal;
        }

        const float ww =
            1.0f / float(whiteTotal);

        const float bw =
            ww * 0.58f;

        int wi = 0;

        for (int n = 0; n < 128; ++n) {
            if (black[n % 12])
                continue;

            keys_.push_back({
                wi * ww,
                0.0f,
                ww,
                1.0f,
                0.0f,
                float(active_[n])
            });

            ++wi;
        }

        wi = 0;

        for (int n = 0; n < 128; ++n) {
            if (black[n % 12]) {
                const float x =
                    wi * ww - bw * 0.5f;

                keys_.push_back({
                    x,
                    0.0f,
                    bw,
                    0.62f,
                    1.0f,
                    float(active_[n])
                });
            } else {
                ++wi;
            }
        }

        glBindBuffer(
            GL_ARRAY_BUFFER,
            inst_);

        glBufferData(
            GL_ARRAY_BUFFER,
            GLsizeiptr(
                keys_.size() *
                sizeof(KeyInstance)),
            keys_.data(),
            GL_DYNAMIC_DRAW);

        dirty_ = false;
    }

    GLuint prog_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint inst_ = 0;

    int width_ = 1;
    int height_ = 1;

    bool dirty_ = true;

    std::array<uint8_t,128> active_{};
    std::vector<KeyInstance> keys_;
};

} // namespace

Keyboard::Keyboard(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
}

void Keyboard::setController(QObject* controller)
{
    if (controller_ == controller)
        return;

    if (controller_) {
        QObject::disconnect(
            controller_.data(), nullptr,
            this, nullptr);
    }

    controller_ = controller;

    if (auto* player =
            qobject_cast<MainWindow*>(
                controller_.data())) {

        auto requestFrame = [this]() {
            update();

            if (window())
                window()->update();
        };

        connect(
            player,
            &MainWindow::currentTimeChanged,
            this,
            requestFrame);

        connect(
            player,
            &MainWindow::documentRevisionChanged,
            this,
            requestFrame);

        connect(
            player,
            &MainWindow::playingChanged,
            this,
            requestFrame);
    }

    emit controllerChanged();

    update();

    if (window())
        window()->update();
}

QQuickFramebufferObject::Renderer*
Keyboard::createRenderer() const
{
    return new KeyboardRenderer();
}
