#include "gl_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

GLuint linkProgram(const char* vertexSource,
                   const char* fragmentSource)
{
    const GLuint vertex =
        compileShader(GL_VERTEX_SHADER, vertexSource);

    const GLuint fragment =
        compileShader(GL_FRAGMENT_SHADER, fragmentSource);

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

constexpr std::size_t InitialRingCapacity =
    std::size_t(1) << 18;

constexpr std::size_t TotalMidiStates =
    16u * 128u;

float wrapHue(float value)
{
    while (value < 0.0f)
        value += 360.0f;

    while (value >= 360.0f)
        value -= 360.0f;

    return value;
}

} // namespace

GLRenderer::GLRenderer()
{
    static const uint8_t defaults[16][4] = {
        {129,140,248,255},{167,139,250,255},{196,181,253,255},{251,146, 60,255},
        { 74,222,128,255},{ 56,189,248,255},{244,114,182,255},{250,204, 21,255},
        {248,113,113,255},{ 52,211,153,255},{ 96,165,250,255},{232,121,249,255},
        {251,113,133,255},{163,230, 53,255},{ 34,211,238,255},{251,191, 36,255}
    };

    for (int i = 0; i < 16; ++i) {
        channelColors_[i] = {
            defaults[i][0],
            defaults[i][1],
            defaults[i][2],
            defaults[i][3]
        };
    }

    openNotes_.reserve(TotalMidiStates);
    neuralLines_.reserve(95u * 24u);
    neuralPoints_.reserve(95u);

    initializeNeuralNodes();
}

GLRenderer::~GLRenderer()
{
    destroy();
}

bool GLRenderer::createPrograms()
{
    static const char* noteVertex = R"GLSL(#version 300 es
precision highp float;
precision highp int;

layout(location=0) in int aStartTick;
layout(location=1) in int aEndTick;
layout(location=2) in uint aPackedData;

uniform float uViewStart;
uniform float uViewEnd;
uniform float uCurrentTick;
uniform vec3 uPalette[16];

flat out vec3 vColor;
flat out float vActive;

void main()
{
    int endTick =
        aEndTick > 0
            ? aEndTick
            : int(uViewEnd);

    uint vertexId =
        uint(gl_VertexID);

    float useEnd =
        float(vertexId & 1u);

    float useTop =
        float(
            (vertexId >> 1u) & 1u);

    float rangeTicks =
        max(
            1.0,
            uViewEnd -
            uViewStart);

    float startX =
        (float(aStartTick) -
         uViewStart) /
        rangeTicks *
        2.0 -
        1.0;

    float endX =
        (float(endTick) -
         uViewStart) /
        rangeTicks *
        2.0 -
        1.0;

    float x =
        mix(
            startX,
            endX,
            useEnd);

    uint pitch =
        (aPackedData >> 8u) &
        0xffu;

    uint colorIndex =
        (aPackedData >> 16u) &
        0x0fu;

    float yBottom =
        -1.0 +
        float(pitch) /
        128.0 *
        2.0;

    float yTop =
        -1.0 +
        float(pitch + 1u) /
        128.0 *
        2.0;

    float y =
        mix(
            yBottom,
            yTop,
            useTop);

    // SharpMIDI's duration-depth trick dramatically reduces fragment overdraw
    // on dense same-pitch rectangles. Shorter notes win the depth test.
    float duration =
        max(
            0.0,
            float(
                endTick -
                aStartTick));

    float z =
        clamp(
            duration /
            16777216.0,
            0.0,
            0.999);

    vColor =
        uPalette[
            int(colorIndex)];

    vActive =
        (uCurrentTick >=
             float(aStartTick) &&
         uCurrentTick <=
             float(endTick))
            ? 1.0
            : 0.0;

    gl_Position =
        vec4(
            x,
            y,
            z,
            1.0);
}
)GLSL";

    static const char* noteFragment = R"GLSL(#version 300 es
precision mediump float;

flat in vec3 vColor;
flat in float vActive;

out vec4 fragColor;

void main()
{
    vec3 color = vColor;

    if (vActive > 0.5)
        color =
            min(
                color * 1.55 +
                vec3(0.12),
                vec3(1.0));

    fragColor =
        vec4(
            color,
            1.0);
}
)GLSL";

    static const char* backgroundVertex = R"GLSL(#version 300 es
precision highp float;

out vec2 vUv;

void main()
{
    vec2 pos =
        vec2(
            (gl_VertexID & 1) == 0
                ? -1.0
                : 1.0,
            (gl_VertexID & 2) == 0
                ? -1.0
                : 1.0);

    vUv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.9999, 1.0);
}
)GLSL";

    static const char* backgroundFragment = R"GLSL(#version 300 es
precision mediump float;

in vec2 vUv;

uniform float uHue;
uniform float uActivity;
uniform float uAspect;

out vec4 fragColor;

vec3 hsl2rgb(float h, float s, float l)
{
    h = fract(h);
    s = clamp(s, 0.0, 1.0);
    l = clamp(l, 0.0, 1.0);

    float c =
        (1.0 -
         abs(2.0 * l - 1.0)) *
        s;

    float hp = h * 6.0;
    float x =
        c *
        (1.0 -
         abs(
             mod(hp, 2.0) -
             1.0));

    vec3 rgb;

    if (hp < 1.0)
        rgb = vec3(c,x,0);
    else if (hp < 2.0)
        rgb = vec3(x,c,0);
    else if (hp < 3.0)
        rgb = vec3(0,c,x);
    else if (hp < 4.0)
        rgb = vec3(0,x,c);
    else if (hp < 5.0)
        rgb = vec3(x,0,c);
    else
        rgb = vec3(c,0,x);

    float m = l - c * 0.5;
    return rgb + vec3(m);
}

float blob(vec2 uv, vec2 center, float radius)
{
    vec2 d = uv - center;
    d.x *= uAspect;

    float dist =
        length(d);

    return
        max(
            0.0,
            1.0 -
            dist /
            radius);
}

void main()
{
    float hue =
        fract(uHue / 360.0);

    float activity =
        clamp(
            uActivity,
            0.0,
            1.0);

    vec3 c1 =
        hsl2rgb(
            hue,
            0.52,
            0.03 +
            activity * 0.04);

    vec3 c2 =
        hsl2rgb(
            fract(
                hue +
                55.0 / 360.0),
            0.48,
            0.05 +
            activity * 0.05);

    float mixValue =
        clamp(
            (vUv.x +
             (1.0 - vUv.y)) *
            0.5,
            0.0,
            1.0);

    vec3 color =
        mix(
            c1,
            c2,
            mixValue);

    float b1 =
        blob(
            vUv,
            vec2(.15,.80),
            .30);

    float b2 =
        blob(
            vUv,
            vec2(.82,.25),
            .28);

    vec3 blob1 =
        hsl2rgb(
            hue,
            .65,
            .28 +
            activity * .14);

    vec3 blob2 =
        hsl2rgb(
            fract(
                hue +
                50.0 / 360.0),
            .65,
            .28 +
            activity * .14);

    color +=
        blob1 *
        b1 * b1 *
        (.09 +
         activity * .07);

    color +=
        blob2 *
        b2 * b2 *
        (.09 +
         activity * .07);

    fragColor =
        vec4(
            clamp(
                color,
                0.0,
                1.0),
            1.0);
}
)GLSL";

    static const char* neuralVertex = R"GLSL(#version 300 es
precision highp float;

layout(location=0) in vec2 aPosition;
layout(location=1) in float aValue;

uniform int uPointMode;

out float vAlpha;

void main()
{
    gl_Position =
        vec4(
            aPosition,
            0.998,
            1.0);

    vAlpha =
        uPointMode == 0
            ? aValue
            : 0.95;

    if (uPointMode != 0)
        gl_PointSize = aValue;
}
)GLSL";

    static const char* neuralFragment = R"GLSL(#version 300 es
precision mediump float;

uniform float uHue;
uniform int uPointMode;

in float vAlpha;

out vec4 fragColor;

vec3 hsv2rgb(vec3 c)
{
    vec4 K =
        vec4(
            1.0,
            2.0/3.0,
            1.0/3.0,
            3.0);

    vec3 p =
        abs(
            fract(
                c.xxx +
                K.xyz) *
            6.0 -
            K.www);

    return
        c.z *
        mix(
            K.xxx,
            clamp(
                p -
                K.xxx,
                0.0,
                1.0),
            c.y);
}

void main()
{
    float hueOffset =
        uPointMode == 0
            ? 25.0
            : 35.0;

    vec3 color =
        hsv2rgb(
            vec3(
                fract(
                    (uHue +
                     hueOffset) /
                    360.0),
                uPointMode == 0
                    ? .70
                    : .78,
                uPointMode == 0
                    ? .65
                    : .68));

    float alpha = vAlpha;

    if (uPointMode != 0) {
        vec2 p =
            gl_PointCoord *
            2.0 -
            1.0;

        float d =
            dot(p,p);

        if (d > 1.0)
            discard;

        alpha *=
            (1.0 -
             smoothstep(
                 0.0,
                 1.0,
                 d));
    }

    fragColor =
        vec4(
            color,
            alpha);
}
)GLSL";

    noteProgram_ =
        linkProgram(
            noteVertex,
            noteFragment);

    backgroundProgram_ =
        linkProgram(
            backgroundVertex,
            backgroundFragment);

    neuralProgram_ =
        linkProgram(
            neuralVertex,
            neuralFragment);

    if (!noteProgram_ ||
        !backgroundProgram_ ||
        !neuralProgram_) {
        return false;
    }

    viewStartUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uViewStart");

    viewEndUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uViewEnd");

    currentTickUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uCurrentTick");

    paletteUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uPalette[0]");

    backgroundHueUniform_ =
        glGetUniformLocation(
            backgroundProgram_,
            "uHue");

    backgroundActivityUniform_ =
        glGetUniformLocation(
            backgroundProgram_,
            "uActivity");

    backgroundAspectUniform_ =
        glGetUniformLocation(
            backgroundProgram_,
            "uAspect");

    neuralHueUniform_ =
        glGetUniformLocation(
            neuralProgram_,
            "uHue");

    neuralPointModeUniform_ =
        glGetUniformLocation(
            neuralProgram_,
            "uPointMode");

    return true;
}

bool GLRenderer::initialize()
{
    if (initialized_)
        return true;

    if (!createPrograms())
        return false;

    glGenVertexArrays(
        1,
        &completedVao_);

    glGenBuffers(
        1,
        &completedVbo_);

    glGenVertexArrays(
        1,
        &openVao_);

    glGenBuffers(
        1,
        &openVbo_);

    glBindVertexArray(openVao_);
    glBindBuffer(
        GL_ARRAY_BUFFER,
        openVbo_);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            TotalMidiStates *
            sizeof(RenderNote)),
        nullptr,
        GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(
        0, 1, GL_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            offsetof(
                RenderNote,
                startTick)));
    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(
        1, 1, GL_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            offsetof(
                RenderNote,
                endTick)));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(
        2, 1, GL_UNSIGNED_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            offsetof(
                RenderNote,
                packedData)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);

    glGenVertexArrays(
        1,
        &backgroundVao_);

    glGenVertexArrays(
        1,
        &neuralLineVao_);

    glGenBuffers(
        1,
        &neuralLineVbo_);

    glBindVertexArray(
        neuralLineVao_);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        neuralLineVbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(NeuralLineVertex),
        reinterpret_cast<void*>(
            offsetof(
                NeuralLineVertex,
                x)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1,
        1,
        GL_FLOAT,
        GL_FALSE,
        sizeof(NeuralLineVertex),
        reinterpret_cast<void*>(
            offsetof(
                NeuralLineVertex,
                alpha)));

    glBindVertexArray(0);

    glGenVertexArrays(
        1,
        &neuralPointVao_);

    glGenBuffers(
        1,
        &neuralPointVbo_);

    glBindVertexArray(
        neuralPointVao_);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        neuralPointVbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(NeuralPointVertex),
        reinterpret_cast<void*>(
            offsetof(
                NeuralPointVertex,
                x)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1,
        1,
        GL_FLOAT,
        GL_FALSE,
        sizeof(NeuralPointVertex),
        reinterpret_cast<void*>(
            offsetof(
                NeuralPointVertex,
                size)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    allocateRing(
        InitialRingCapacity);

    initialized_ = true;
    return true;
}

void GLRenderer::destroy()
{
    const GLuint buffers[] = {
        completedVbo_,
        openVbo_,
        neuralLineVbo_,
        neuralPointVbo_
    };

    for (GLuint buffer : buffers) {
        if (buffer)
            glDeleteBuffers(1, &buffer);
    }

    const GLuint vaos[] = {
        completedVao_,
        openVao_,
        backgroundVao_,
        neuralLineVao_,
        neuralPointVao_
    };

    for (GLuint vao : vaos) {
        if (vao)
            glDeleteVertexArrays(1, &vao);
    }

    const GLuint programs[] = {
        noteProgram_,
        backgroundProgram_,
        neuralProgram_
    };

    for (GLuint program : programs) {
        if (program)
            glDeleteProgram(program);
    }

    completedVbo_ = 0;
    openVbo_ = 0;
    neuralLineVbo_ = 0;
    neuralPointVbo_ = 0;

    completedVao_ = 0;
    openVao_ = 0;
    backgroundVao_ = 0;
    neuralLineVao_ = 0;
    neuralPointVao_ = 0;

    noteProgram_ = 0;
    backgroundProgram_ = 0;
    neuralProgram_ = 0;

    ring_.clear();
    openNotes_.clear();

    ringCapacity_ = 0;
    ringMask_ = 0;

    initialized_ = false;
}

void GLRenderer::resize(
    int width,
    int height)
{
    width_ =
        std::max(
            1,
            width);

    height_ =
        std::max(
            1,
            height);
}

void GLRenderer::setDocument(
    const MidiDocument* document)
{
    document_ = document;
    resetSweep();
}

void GLRenderer::setCurrentTime(
    float seconds)
{
    currentTime_ =
        std::max(
            0.0f,
            seconds);
}

void GLRenderer::setNoteSpeed(
    float secondsPerWindow)
{
    const float value =
        std::clamp(
            secondsPerWindow,
            0.1f,
            60.0f);

    if (std::abs(
            noteSpeed_ -
            value) <
        0.00001f) {
        return;
    }

    noteSpeed_ = value;
    forceSweepReset_ = true;
}

void GLRenderer::setPostBuffer(
    float seconds)
{
    const float value =
        std::clamp(
            seconds,
            0.0f,
            10.0f);

    if (std::abs(
            postBuffer_ -
            value) <
        0.00001f) {
        return;
    }

    postBuffer_ = value;
    forceSweepReset_ = true;
}

void GLRenderer::setPerTrackColors(
    bool enabled)
{
    if (perTrackColors_ == enabled)
        return;

    perTrackColors_ = enabled;
    forceSweepReset_ = true;
}

void GLRenderer::setChannelColor(
    uint8_t channel,
    uint8_t r,
    uint8_t g,
    uint8_t b)
{
    if (channel >=
        channelColors_.size()) {
        return;
    }

    const std::array<uint8_t, 4>
        value =
            {r,g,b,255};

    if (channelColors_[channel] ==
        value) {
        return;
    }

    channelColors_[channel] =
        value;

    paletteDirty_ = true;
}

void GLRenderer::setNeuralVisual(
    float hue,
    float activity)
{
    neuralTargetHue_ =
        wrapHue(hue);

    neuralActivity_ =
        std::clamp(
            activity,
            0.0f,
            1.0f);
}

void GLRenderer::initializeNeuralNodes()
{
    uint32_t state =
        0x4d595df4u;

    auto random01 =
        [&state]() {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;

            return
                float(
                    state &
                    0x00ffffffu) /
                float(
                    0x01000000u);
        };

    for (auto& node :
         neuralNodes_) {
        node.x = random01();
        node.y = random01();

        node.radius =
            .7f +
            random01() *
            1.2f;

        node.pulse =
            random01() *
            6.28318530718f;

        node.pulseSpeed =
            .006f +
            random01() *
            .014f;

        node.steer =
            random01() *
            6.28318530718f;

        node.steerSpeed =
            .003f +
            random01() *
            .004f;
    }
}

void GLRenderer::updateNeuralNodes()
{
    const auto now =
        std::chrono::steady_clock::now();

    float frameScale =
        std::chrono::duration<float>(
            now -
            neuralClock_).count() *
        60.0f;

    neuralClock_ = now;

    frameScale =
        std::clamp(
            frameScale,
            0.0f,
            3.0f);

    float hueDiff =
        neuralTargetHue_ -
        neuralHue_;

    if (hueDiff > 180.0f)
        hueDiff -= 360.0f;

    if (hueDiff < -180.0f)
        hueDiff += 360.0f;

    neuralHue_ =
        wrapHue(
            neuralHue_ +
            hueDiff *
            .02f *
            frameScale);

    const float speed =
        .28f +
        neuralActivity_ *
        2.2f;

    neuralPoints_.clear();

    for (auto& node :
         neuralNodes_) {
        node.steer +=
            node.steerSpeed *
            (1.0f +
             neuralActivity_ *
             6.0f) *
            frameScale;

        node.x +=
            std::cos(node.steer) *
            speed /
            float(width_) *
            frameScale;

        node.y +=
            std::sin(node.steer) *
            speed /
            float(height_) *
            frameScale;

        node.pulse +=
            node.pulseSpeed *
            frameScale;

        if (node.x < -.04f)
            node.x = 1.04f;

        if (node.x > 1.04f)
            node.x = -.04f;

        if (node.y < -.06f)
            node.y = 1.06f;

        if (node.y > 1.06f)
            node.y = -.06f;

        const float pointRadius =
            node.radius *
            (.8f +
             .2f *
             std::sin(
                 node.pulse)) *
            (1.0f +
             neuralActivity_ *
             .5f);

        neuralPoints_.push_back({
            node.x * 2.0f - 1.0f,
            1.0f - node.y * 2.0f,
            std::max(
                1.0f,
                pointRadius *
                2.6f),
            .95f
        });
    }

    neuralLines_.clear();

    const float threshold =
        130.0f;

    for (std::size_t i = 0;
         i < neuralNodes_.size();
         ++i) {
        const auto& a =
            neuralNodes_[i];

        const float ax =
            a.x *
            float(width_);

        const float ay =
            a.y *
            float(height_);

        for (std::size_t j = i + 1;
             j < neuralNodes_.size();
             ++j) {
            const auto& b =
                neuralNodes_[j];

            const float dx =
                ax -
                b.x *
                float(width_);

            const float dy =
                ay -
                b.y *
                float(height_);

            const float distance2 =
                dx * dx +
                dy * dy;

            if (distance2 >=
                threshold *
                threshold) {
                continue;
            }

            const float distance =
                std::sqrt(
                    distance2);

            const float alpha =
                (1.0f -
                 distance /
                 threshold) *
                (.28f +
                 neuralActivity_ *
                 .22f);

            neuralLines_.push_back({
                a.x * 2.0f - 1.0f,
                1.0f - a.y * 2.0f,
                alpha
            });

            neuralLines_.push_back({
                b.x * 2.0f - 1.0f,
                1.0f - b.y * 2.0f,
                alpha
            });
        }
    }
}

void GLRenderer::renderBackground()
{
    updateNeuralNodes();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glUseProgram(
        backgroundProgram_);

    glUniform1f(
        backgroundHueUniform_,
        neuralHue_);

    glUniform1f(
        backgroundActivityUniform_,
        neuralActivity_);

    glUniform1f(
        backgroundAspectUniform_,
        float(width_) /
        float(std::max(1, height_)));

    glBindVertexArray(
        backgroundVao_);

    glDrawArrays(
        GL_TRIANGLE_STRIP,
        0,
        4);

    glBindVertexArray(0);

    glEnable(GL_BLEND);

    glBlendFunc(
        GL_SRC_ALPHA,
        GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(
        neuralProgram_);

    glUniform1f(
        neuralHueUniform_,
        neuralHue_);

    if (!neuralLines_.empty()) {
        glBindBuffer(
            GL_ARRAY_BUFFER,
            neuralLineVbo_);

        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(
                neuralLines_.size() *
                sizeof(
                    NeuralLineVertex)),
            neuralLines_.data(),
            GL_STREAM_DRAW);

        glUniform1i(
            neuralPointModeUniform_,
            0);

        glBindVertexArray(
            neuralLineVao_);

        glDrawArrays(
            GL_LINES,
            0,
            static_cast<GLsizei>(
                neuralLines_.size()));
    }

    if (!neuralPoints_.empty()) {
        glBindBuffer(
            GL_ARRAY_BUFFER,
            neuralPointVbo_);

        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(
                neuralPoints_.size() *
                sizeof(
                    NeuralPointVertex)),
            neuralPoints_.data(),
            GL_STREAM_DRAW);

        glUniform1i(
            neuralPointModeUniform_,
            1);

        glBindVertexArray(
            neuralPointVao_);

        glDrawArrays(
            GL_POINTS,
            0,
            static_cast<GLsizei>(
                neuralPoints_.size()));
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    glDisable(GL_BLEND);
}

void GLRenderer::allocateRing(
    std::size_t capacity)
{
    std::size_t rounded = 1;

    while (rounded < capacity)
        rounded <<= 1;

    const std::size_t oldCapacity =
        ringCapacity_;

    const std::size_t oldMask =
        ringMask_;

    std::vector<RenderNote>
        oldRing;

    oldRing.swap(ring_);

    ringCapacity_ = rounded;
    ringMask_ =
        ringCapacity_ - 1;

    ring_.resize(
        ringCapacity_);

    if (!oldRing.empty() &&
        oldCapacity != 0) {
        for (int64_t id = tail_;
             id < head_;
             ++id) {
            ring_[
                static_cast<
                    std::size_t>(
                        id) &
                ringMask_] =
            oldRing[
                static_cast<
                    std::size_t>(
                        id) &
                oldMask];
        }
    }

    glBindBuffer(
        GL_ARRAY_BUFFER,
        completedVbo_);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            ringCapacity_ *
            sizeof(RenderNote)),
        nullptr,
        GL_DYNAMIC_DRAW);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);

    if (head_ > tail_)
        uploadCompletedRange(
            tail_,
            head_);
}

void GLRenderer::ensureRingSpace()
{
    if (ringCapacity_ == 0) {
        allocateRing(
            InitialRingCapacity);
        return;
    }

    if (head_ - tail_ <
        static_cast<int64_t>(
            ringCapacity_ - 1)) {
        return;
    }

    allocateRing(
        ringCapacity_ * 2);
}

void GLRenderer::reserveForSweep(
    uint32_t fromTick,
    uint32_t toTick)
{
    if (!document_ ||
        fromTick > toTick) {
        return;
    }

    const std::size_t begin =
        document_->
            lowerBoundGroup(
                fromTick);

    const std::size_t end =
        document_->
            upperBoundGroup(
                toTick);

    uint64_t possibleNotes = 0;

    for (std::size_t i = begin;
         i < end;
         ++i) {
        possibleNotes +=
            document_->
                tickGroups[i]
                .noteOnCount;
    }

    const uint64_t required =
        uint64_t(
            std::max<int64_t>(
                0,
                head_ -
                tail_)) +
        possibleNotes +
        1u;

    if (required <= ringCapacity_)
        return;

    std::size_t target =
        std::max<std::size_t>(
            ringCapacity_,
            InitialRingCapacity);

    while (uint64_t(target) <
           required) {
        target <<= 1;
    }

    allocateRing(target);
}

void GLRenderer::resetSweep()
{
    head_ = 0;
    tail_ = 0;

    activeCount_.fill(0);
    activeColor_.fill(0);
    activeStartTick_.fill(0);
    activePackedData_.fill(0);

    openNotes_.clear();

    lastSweepEnd_ = -1;
    lastViewSpan_ = 0;

    forceSweepReset_ = true;
    openDirty_ = true;
}

uint8_t GLRenderer::eventColor(
    const CompactEvent& event) const
{
    return document_
        ? document_->
            colorIndex(
                event,
                perTrackColors_)
        : 0;
}

void GLRenderer::appendCompleted(
    int32_t startTick,
    int32_t endTick,
    uint32_t packedData)
{
    ensureRingSpace();

    const std::size_t physical =
        static_cast<
            std::size_t>(
                head_) &
        ringMask_;

    ring_[physical] = {
        startTick,
        endTick,
        packedData
    };

    ++head_;
}

void GLRenderer::beginActiveNote(
    std::size_t stateIndex,
    uint32_t tick,
    uint8_t pitch,
    uint8_t velocity,
    uint8_t color)
{
    activeStartTick_[stateIndex] =
        static_cast<int32_t>(
            std::min<uint32_t>(
                tick,
                uint32_t(
                    std::numeric_limits<
                        int32_t>::max())));

    activePackedData_[stateIndex] =
        uint32_t(velocity) |
        (uint32_t(pitch) << 8) |
        (uint32_t(
            color & 0x0f) << 16);

    activeColor_[stateIndex] =
        color;

    openDirty_ = true;
}

void GLRenderer::finishActiveNote(
    std::size_t stateIndex,
    uint32_t tick)
{
    appendCompleted(
        activeStartTick_[
            stateIndex],
        static_cast<int32_t>(
            std::min<uint32_t>(
                tick,
                uint32_t(
                    std::numeric_limits<
                        int32_t>::max()))),
        activePackedData_[
            stateIndex]);

    openDirty_ = true;
}

void GLRenderer::processEvent(
    const CompactEvent& event,
    uint32_t tick)
{
    const uint8_t command =
        event.status & 0xf0;

    if (command != 0x90 &&
        command != 0x80) {
        return;
    }

    const uint8_t channel =
        event.status & 0x0f;

    const uint8_t pitch =
        event.data1 & 0x7f;

    const std::size_t stateIndex =
        std::size_t(channel) *
        128u +
        std::size_t(pitch);

    const uint8_t color =
        eventColor(event);

    uint32_t& count =
        activeCount_[stateIndex];

    if (command == 0x90 &&
        event.data2 != 0) {
        if (count != 0 &&
            activeColor_[
                stateIndex] !=
                color) {
            finishActiveNote(
                stateIndex,
                tick);

            count = 0;
        }

        if (count == 0) {
            beginActiveNote(
                stateIndex,
                tick,
                pitch,
                event.data2,
                color);
        }

        ++count;
        return;
    }

    if (count == 0)
        return;

    if (activeColor_[
            stateIndex] !=
        color) {
        return;
    }

    --count;

    if (count == 0)
        finishActiveNote(
            stateIndex,
            tick);
}

void GLRenderer::uploadCompletedRange(
    int64_t begin,
    int64_t end)
{
    if (end <= begin ||
        ringCapacity_ == 0) {
        return;
    }

    glBindBuffer(
        GL_ARRAY_BUFFER,
        completedVbo_);

    int64_t cursor = begin;

    while (cursor < end) {
        const std::size_t physical =
            static_cast<
                std::size_t>(
                    cursor) &
            ringMask_;

        const std::size_t available =
            ringCapacity_ -
            physical;

        const std::size_t count =
            static_cast<
                std::size_t>(
                    std::min<
                        int64_t>(
                            end -
                            cursor,
                            static_cast<
                                int64_t>(
                                    available)));

        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(
                physical *
                sizeof(RenderNote)),
            static_cast<GLsizeiptr>(
                count *
                sizeof(RenderNote)),
            ring_.data() +
                physical);

        cursor +=
            static_cast<
                int64_t>(
                    count);
    }

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);
}

void GLRenderer::rebuildOpenNotes()
{
    if (!openDirty_)
        return;

    openNotes_.clear();

    for (std::size_t state = 0;
         state < TotalMidiStates;
         ++state) {
        if (activeCount_[state] == 0)
            continue;

        openNotes_.push_back({
            activeStartTick_[state],
            0,
            activePackedData_[state]
        });
    }

    openDirty_ = false;
}

void GLRenderer::uploadOpenNotes()
{
    rebuildOpenNotes();

    if (openNotes_.empty())
        return;

    glBindBuffer(
        GL_ARRAY_BUFFER,
        openVbo_);

    glBufferSubData(
        GL_ARRAY_BUFFER,
        0,
        static_cast<GLsizeiptr>(
            openNotes_.size() *
            sizeof(RenderNote)),
        openNotes_.data());

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);
}

void GLRenderer::sweepRange(
    uint32_t fromTick,
    uint32_t toTick)
{
    if (!document_ ||
        document_->tickGroups.empty() ||
        fromTick > toTick) {
        return;
    }

    reserveForSweep(
        fromTick,
        toTick);

    const std::size_t firstGroup =
        document_->
            lowerBoundGroup(
                fromTick);

    const std::size_t lastGroup =
        document_->
            upperBoundGroup(
                toTick);

    const int64_t appendBegin =
        head_;

    for (std::size_t groupIndex =
             firstGroup;
         groupIndex < lastGroup;
         ++groupIndex) {
        const TickGroup& group =
            document_->
                tickGroups[
                    groupIndex];

        const std::size_t begin =
            group.eventOffset;

        const std::size_t end =
            begin +
            group.eventCount;

        for (std::size_t eventIndex =
                 begin;
             eventIndex < end;
             ++eventIndex) {
            processEvent(
                document_->
                    events[
                        eventIndex],
                group.tick);
        }
    }

    // Completed notes were appended in monotonically increasing NoteOff tick
    // order, so the whole update is one/two contiguous WebGL transfers.
    uploadCompletedRange(
        appendBegin,
        head_);

    if (openDirty_)
        uploadOpenNotes();
}

void GLRenderer::advanceTail(
    uint32_t viewStart)
{
    if (ringCapacity_ == 0)
        return;

    const int64_t safeTail =
        head_ -
        static_cast<
            int64_t>(
                ringCapacity_);

    if (tail_ < safeTail)
        tail_ = safeTail;

    // Completed notes are appended at NoteOff time, therefore EndTick is
    // monotonic. Tail culling is O(number actually discarded), with no scan
    // across millions of retained instances.
    while (tail_ < head_) {
        const RenderNote& note =
            ring_[
                static_cast<
                    std::size_t>(
                        tail_) &
                ringMask_];

        if (note.endTick <
            static_cast<int32_t>(
                viewStart)) {
            ++tail_;
        } else {
            break;
        }
    }
}

void GLRenderer::calculateView(
    uint32_t& currentTick,
    uint32_t& viewStart,
    uint32_t& viewEnd,
    uint32_t& sweepEnd) const
{
    if (!document_) {
        currentTick =
            viewStart =
            viewEnd =
            sweepEnd = 0;
        return;
    }

    currentTick =
        static_cast<uint32_t>(
            std::clamp<double>(
                std::floor(
                    document_->
                        secondsToTick(
                            currentTime_)),
                0.0,
                document_->maxTick));

    const double rightSeconds =
        std::min<double>(
            document_->
                durationSeconds,
            double(currentTime_) +
            double(noteSpeed_));

    const uint32_t rightTick =
        static_cast<uint32_t>(
            std::clamp<double>(
                std::ceil(
                    document_->
                        secondsToTick(
                            rightSeconds)),
                currentTick + 1.0,
                double(
                    std::max<uint32_t>(
                        currentTick + 1,
                        document_->
                            maxTick))));

    const double historySeconds =
        postBuffer_ > 0.0f
            ? double(postBuffer_)
            : double(noteSpeed_) *
              (0.18 / 0.82);

    const double leftSeconds =
        std::max(
            0.0,
            double(currentTime_) -
            historySeconds);

    viewStart =
        static_cast<uint32_t>(
            std::max(
                0.0,
                std::floor(
                    document_->
                        secondsToTick(
                            leftSeconds))));

    viewEnd =
        std::max(
            viewStart + 1,
            rightTick);

    // Unlike desktop SharpMIDI, WebGL has no persistent mapped VBO. Sweeping
    // hidden lookahead notes only increases CPU/GPU work. The visible right
    // edge is sufficient because the next frame appends only the new delta.
    sweepEnd = viewEnd;
}

void GLRenderer::setCompletedInstanceBase(
    std::size_t physicalIndex)
{
    const std::size_t base =
        physicalIndex *
        sizeof(RenderNote);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        completedVbo_);

    glVertexAttribIPointer(
        0, 1,
        GL_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                RenderNote,
                startTick)));

    glVertexAttribIPointer(
        1, 1,
        GL_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                RenderNote,
                endTick)));

    glVertexAttribIPointer(
        2, 1,
        GL_UNSIGNED_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                RenderNote,
                packedData)));
}

void GLRenderer::renderRoll()
{
    if (!initialized_ &&
        !initialize()) {
        return;
    }

    glViewport(
        0, 0,
        width_,
        height_);

    renderBackground();

    if (!document_ ||
        document_->noteCount == 0 ||
        document_->tickGroups.empty()) {
        return;
    }

    uint32_t currentTick = 0;
    uint32_t viewStart = 0;
    uint32_t viewEnd = 0;
    uint32_t sweepEnd = 0;

    calculateView(
        currentTick,
        viewStart,
        viewEnd,
        sweepEnd);

    const uint32_t viewSpan =
        std::max<uint32_t>(
            1,
            viewEnd -
            viewStart);

    const bool incremental =
        !forceSweepReset_ &&
        lastSweepEnd_ >= 0 &&
        sweepEnd >=
            static_cast<uint32_t>(
                lastSweepEnd_) &&
        sweepEnd -
            static_cast<uint32_t>(
                lastSweepEnd_) <
            viewSpan;

    if (!incremental) {
        head_ = 0;
        tail_ = 0;

        activeCount_.fill(0);
        activeColor_.fill(0);
        activeStartTick_.fill(0);
        activePackedData_.fill(0);

        openNotes_.clear();
        openDirty_ = true;

        const uint32_t lookbehind =
            std::min(
                viewStart,
                std::max<uint32_t>(
                    viewSpan,
                    uint32_t(
                        std::max<uint16_t>(
                            1,
                            document_->
                                ticksPerBeat)) *
                    16u));

        const uint32_t sweepStart =
            viewStart -
            lookbehind;

        reserveForSweep(
            sweepStart,
            sweepEnd);

        sweepRange(
            sweepStart,
            sweepEnd);
    } else if (
        sweepEnd >
        static_cast<uint32_t>(
            lastSweepEnd_)) {
        sweepRange(
            static_cast<uint32_t>(
                lastSweepEnd_) +
            1u,
            sweepEnd);
    }

    lastSweepEnd_ =
        static_cast<int64_t>(
            sweepEnd);

    lastViewSpan_ = viewSpan;
    forceSweepReset_ = false;

    advanceTail(viewStart);

    if (openDirty_)
        uploadOpenNotes();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);

    glUseProgram(noteProgram_);

    glUniform1f(
        viewStartUniform_,
        static_cast<float>(
            viewStart));

    glUniform1f(
        viewEndUniform_,
        static_cast<float>(
            viewEnd));

    glUniform1f(
        currentTickUniform_,
        static_cast<float>(
            currentTick));

    if (paletteDirty_) {
        GLfloat palette[16 * 3];

        for (int i = 0;
             i < 16;
             ++i) {
            palette[
                i * 3 + 0] =
                channelColors_[i][0] /
                255.0f;

            palette[
                i * 3 + 1] =
                channelColors_[i][1] /
                255.0f;

            palette[
                i * 3 + 2] =
                channelColors_[i][2] /
                255.0f;
        }

        glUniform3fv(
            paletteUniform_,
            16,
            palette);

        paletteDirty_ = false;
    }

    const int64_t completed64 =
        std::max<int64_t>(
            0,
            head_ -
            tail_);

    if (completed64 > 0 &&
        ringCapacity_ != 0) {
        const std::size_t completed =
            static_cast<std::size_t>(
                std::min<int64_t>(
                    completed64,
                    static_cast<int64_t>(
                        ringCapacity_)));

        const std::size_t start =
            static_cast<std::size_t>(
                tail_) &
            ringMask_;

        const std::size_t first =
            std::min(
                completed,
                ringCapacity_ -
                start);

        const std::size_t second =
            completed - first;

        glBindVertexArray(
            completedVao_);

        if (first != 0) {
            setCompletedInstanceBase(
                start);

            glDrawArraysInstanced(
                GL_TRIANGLE_STRIP,
                0,
                4,
                static_cast<GLsizei>(
                    first));
        }

        if (second != 0) {
            setCompletedInstanceBase(
                0);

            glDrawArraysInstanced(
                GL_TRIANGLE_STRIP,
                0,
                4,
                static_cast<GLsizei>(
                    second));
        }
    }

    if (!openNotes_.empty()) {
        glBindVertexArray(
            openVao_);

        glDrawArraysInstanced(
            GL_TRIANGLE_STRIP,
            0,
            4,
            static_cast<GLsizei>(
                openNotes_.size()));
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    glDisable(GL_DEPTH_TEST);

    const float playheadFraction =
        std::clamp(
            float(
                currentTick -
                viewStart) /
            float(
                std::max<uint32_t>(
                    1,
                    viewEnd -
                    viewStart)),
            0.0f,
            1.0f);

    const int lineX =
        std::clamp(
            static_cast<int>(
                std::lround(
                    playheadFraction *
                    float(width_))),
            0,
            std::max(
                0,
                width_ - 1));

    glEnable(GL_SCISSOR_TEST);

    glScissor(
        lineX,
        0,
        std::max(
            1,
            width_ / 900),
        height_);

    glClearColor(
        .63f,
        .54f,
        .98f,
        .92f);

    glClear(
        GL_COLOR_BUFFER_BIT);

    glDisable(GL_SCISSOR_TEST);
}

} // namespace wasmidi
