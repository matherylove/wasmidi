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
    float x, y, w, h;
    float r, g, b, active;
};

GLuint compileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint makeProgram()
{
    static const char* vertex = R"GLSL(#version 300 es
precision highp float;

layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 aRect;
layout(location=2) in vec4 aColor;

out vec2 vUv;
out vec4 vColor;
out float vBlack;

void main()
{
    vec2 p =
        aRect.xy +
        aCorner * aRect.zw;

    gl_Position =
        vec4(
            p * 2.0 - 1.0,
            0.0,
            1.0);

    vUv = aCorner;
    vColor = aColor;
    vBlack = float(aRect.w < 0.8);
}
)GLSL";

    static const char* fragment = R"GLSL(#version 300 es
precision mediump float;

in vec2 vUv;
in vec4 vColor;
in float vBlack;

uniform float uHue;

out vec4 fragColor;

vec3 hsv2rgb(vec3 c)
{
    vec4 K =
        vec4(
            1.0,
            2.0 / 3.0,
            1.0 / 3.0,
            3.0);

    vec3 p =
        abs(
            fract(c.xxx + K.xyz) *
            6.0 -
            K.www);

    return
        c.z *
        mix(
            K.xxx,
            clamp(
                p - K.xxx,
                0.0,
                1.0),
            c.y);
}

void main()
{
    float hue =
        fract(uHue / 360.0);

    vec3 top =
        vBlack > 0.5
            ? hsv2rgb(
                vec3(hue, .20, .12))
            : hsv2rgb(
                vec3(hue, .30, .18));

    vec3 bottom =
        vBlack > 0.5
            ? hsv2rgb(
                vec3(hue, .15, .03))
            : hsv2rgb(
                vec3(hue, .22, .07));

    vec3 color =
        mix(top, bottom, vUv.y);

    if (vColor.a > 0.5) {
        color =
            mix(
                color,
                vColor.rgb,
                vBlack > 0.5
                    ? .94
                    : .86);
    }

    if (vBlack < 0.5 &&
        min(vUv.x, 1.0 - vUv.x) <
            .025) {
        color *= .58;
    }

    fragColor = vec4(color, 1.0);
}
)GLSL";

    const GLuint vs =
        compileShader(
            GL_VERTEX_SHADER,
            vertex);

    const GLuint fs =
        compileShader(
            GL_FRAGMENT_SHADER,
            fragment);

    if (!vs || !fs)
        return 0;

    const GLuint program =
        glCreateProgram();

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(
        program,
        GL_LINK_STATUS,
        &ok);

    if (!ok) {
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

class KeyboardRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    explicit KeyboardRenderer(qreal dpr)
        : dpr_(
            std::max<qreal>(
                1.0,
                dpr))
    {
    }

    ~KeyboardRenderer() override
    {
        if (vbo_)
            glDeleteBuffers(1, &vbo_);

        if (instanceVbo_)
            glDeleteBuffers(
                1,
                &instanceVbo_);

        if (vao_)
            glDeleteVertexArrays(
                1,
                &vao_);

        if (program_)
            glDeleteProgram(program_);
    }

    QOpenGLFramebufferObject*
    createFramebufferObject(
        const QSize& size) override
    {
        const QSize cssSize(
            std::max(
                64,
                qRound(
                    size.width() /
                    dpr_)),
            std::max(
                64,
                qRound(
                    size.height() /
                    dpr_)));

        return new QOpenGLFramebufferObject(
            cssSize);
    }

    void synchronize(
        QQuickFramebufferObject* item) override
    {
        auto* keyboard =
            static_cast<Keyboard*>(item);

        auto* controller =
            qobject_cast<MainWindow*>(
                keyboard->controller());

        if (!controller)
            return;

        const auto nextMask =
            controller->activePitchMask();

        const auto nextColorIndices =
            controller->activePitchColorIndices();

        const auto nextColors =
            controller->channelColors();

        if (nextMask != mask_ ||
            nextColorIndices != colorIndices_ ||
            nextColors != colors_) {
            mask_ = nextMask;
            colorIndices_ = nextColorIndices;
            colors_ = nextColors;
            dirty_ = true;
        }

        // Background hue is a shader uniform; changing it does not require a
        // 128-instance VBO rebuild. This keeps keyboard rendering effectively
        // pre-cached between actual key/color state changes.
        hue_ = controller->dominantHue();
    }

    void render() override
    {
        if (!program_)
            initialize();

        if (!program_)
            return;

        if (auto* fbo =
                framebufferObject()) {
            width_ =
                std::max(
                    1,
                    fbo->size().width());

            height_ =
                std::max(
                    1,
                    fbo->size().height());
        }

        if (dirty_)
            rebuild();

        glViewport(
            0,
            0,
            width_,
            height_);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        glClearColor(
            .01f,
            .01f,
            .025f,
            1.0f);

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program_);

        glUniform1f(
            hueUniform_,
            hue_);

        glBindVertexArray(vao_);

        glDrawArraysInstanced(
            GL_TRIANGLES,
            0,
            6,
            static_cast<GLsizei>(
                keys_.size()));

        glBindVertexArray(0);

        QQuickOpenGLUtils::
            resetOpenGLState();
    }

private:
    void initialize()
    {
        program_ = makeProgram();

        if (!program_)
            return;

        hueUniform_ =
            glGetUniformLocation(
                program_,
                "uHue");

        static const float quad[] = {
            0,0, 1,0, 0,1,
            0,1, 1,0, 1,1
        };

        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);

        glGenBuffers(1, &vbo_);
        glBindBuffer(
            GL_ARRAY_BUFFER,
            vbo_);

        glBufferData(
            GL_ARRAY_BUFFER,
            sizeof(quad),
            quad,
            GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);

        glVertexAttribPointer(
            0,
            2,
            GL_FLOAT,
            GL_FALSE,
            0,
            nullptr);

        glGenBuffers(
            1,
            &instanceVbo_);

        glBindBuffer(
            GL_ARRAY_BUFFER,
            instanceVbo_);

        glEnableVertexAttribArray(1);

        glVertexAttribPointer(
            1,
            4,
            GL_FLOAT,
            GL_FALSE,
            sizeof(KeyInstance),
            reinterpret_cast<void*>(0));

        glVertexAttribDivisor(1, 1);

        glEnableVertexAttribArray(2);

        glVertexAttribPointer(
            2,
            4,
            GL_FLOAT,
            GL_FALSE,
            sizeof(KeyInstance),
            reinterpret_cast<void*>(
                4 * sizeof(float)));

        glVertexAttribDivisor(2, 1);

        glBindVertexArray(0);
    }

    std::array<float, 4>
    activeColor(int note) const
    {
        if (!mask_[note] ||
            colorIndices_[note] < 0 ||
            colorIndices_[note] >=
                colors_.size()) {
            return {0, 0, 0, 0};
        }

        const QColor color =
            colors_[
                colorIndices_[note]];

        return {
            float(color.redF()),
            float(color.greenF()),
            float(color.blueF()),
            1.0f
        };
    }

    void rebuild()
    {
        static const bool black[12] = {
            false,true,false,true,false,false,
            true,false,true,false,true,false
        };

        keys_.clear();
        keys_.reserve(128);

        int whiteCount = 0;

        for (int note = 0;
             note < 128;
             ++note) {
            if (!black[note % 12])
                ++whiteCount;
        }

        const float whiteWidth =
            1.0f /
            float(whiteCount);

        const float blackWidth =
            whiteWidth * .58f;

        int whiteIndex = 0;

        for (int note = 0;
             note < 128;
             ++note) {
            if (black[note % 12])
                continue;

            const auto color =
                activeColor(note);

            keys_.push_back({
                whiteIndex *
                    whiteWidth,
                0.0f,
                whiteWidth,
                1.0f,
                color[0],
                color[1],
                color[2],
                color[3]
            });

            ++whiteIndex;
        }

        whiteIndex = 0;

        for (int note = 0;
             note < 128;
             ++note) {
            if (black[note % 12]) {
                const float x =
                    whiteIndex *
                        whiteWidth -
                    blackWidth *
                        .5f;

                const auto color =
                    activeColor(note);

                keys_.push_back({
                    x,
                    0.0f,
                    blackWidth,
                    .62f,
                    color[0],
                    color[1],
                    color[2],
                    color[3]
                });
            } else {
                ++whiteIndex;
            }
        }

        glBindBuffer(
            GL_ARRAY_BUFFER,
            instanceVbo_);

        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(
                keys_.size() *
                sizeof(KeyInstance)),
            keys_.data(),
            GL_DYNAMIC_DRAW);

        dirty_ = false;
    }

    qreal dpr_ = 1.0;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint instanceVbo_ = 0;

    GLint hueUniform_ = -1;

    int width_ = 1;
    int height_ = 1;
    bool dirty_ = true;
    float hue_ = 230.0f;

    std::array<uint8_t, 128> mask_{};
    std::array<int8_t, 128> colorIndices_{};
    QVector<QColor> colors_;
    std::vector<KeyInstance> keys_;
};

} // namespace

Keyboard::Keyboard(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
    setTextureFollowsItemSize(true);

    resizeSettleTimer_.setSingleShot(true);
    resizeSettleTimer_.setInterval(140);

    connect(
        &resizeSettleTimer_,
        &QTimer::timeout,
        this,
        [this]() {
            setTextureFollowsItemSize(true);
            update();
        });
}

void Keyboard::geometryChange(
    const QRectF& newGeometry,
    const QRectF& oldGeometry)
{
    QQuickFramebufferObject::geometryChange(
        newGeometry,
        oldGeometry);

    if (newGeometry.size() == oldGeometry.size())
        return;

    setTextureFollowsItemSize(false);
    resizeSettleTimer_.start();
    update();
}

void Keyboard::setController(QObject* controller)
{
    if (controller_ == controller)
        return;

    if (controller_) {
        QObject::disconnect(
            controller_.data(),
            nullptr,
            this,
            nullptr);
    }

    controller_ = controller;

    if (auto* player =
            qobject_cast<MainWindow*>(
                controller_.data())) {
        auto request =
            [this]() {
                update();
            };

        connect(
            player,
            &MainWindow::activePitchesChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::channelColorsChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::neuralVisualChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::documentRevisionChanged,
            this,
            request);
    }

    emit controllerChanged();
    update();
}

QQuickFramebufferObject::Renderer*
Keyboard::createRenderer() const
{
    const qreal dpr =
        window()
            ? window()->devicePixelRatio()
            : 1.0;

    return new KeyboardRenderer(dpr);
}
