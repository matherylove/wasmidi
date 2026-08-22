#include "gl_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
namespace {
wasmidi::GLRenderer* g_visualCacheRenderer = nullptr;
}

EM_JS(void, wasmidi_visual_cache_install,
      (const uint32_t* words, uint32_t noteCount,
       const uint32_t* keyStarts, uint32_t keyStartCount,
       const uint32_t* keyEnds, uint32_t keyEndCount,
       const uint32_t* keyOwners, uint32_t keyOwnerCount,
       uint32_t generation, uint32_t maxTick), {
    const root = globalThis;
    const previous = root.__wasmidiVisualCache;
    if (previous && previous.worker) {
        try { previous.worker.terminate(); } catch (_) {}
    }

    const state = {
        worker: null,
        generation: generation >>> 0
    };
    root.__wasmidiVisualCache = state;

    if (typeof Worker !== 'function' || !noteCount) return;

    try {
        const copyBytes = (ptr, byteLength) => {
            const copy = new Uint8Array(byteLength >>> 0);
            if (byteLength)
                copy.set(HEAPU8.subarray(ptr, ptr + byteLength));
            return copy;
        };

        const noteBytes = (noteCount >>> 0) * 12;
        const startBytes = (keyStartCount >>> 0) * 12;
        const endBytes = (keyEndCount >>> 0) * 12;
        const ownerBytes = (keyOwnerCount >>> 0) * 8;

        const copy = copyBytes(words, noteBytes);
        const startsCopy = copyBytes(keyStarts, startBytes);
        const endsCopy = copyBytes(keyEnds, endBytes);
        const ownersCopy = copyBytes(keyOwners, ownerBytes);

        const worker = new Worker('./visual-cache-worker.js');
        state.worker = worker;

        worker.onmessage = event => {
            const message = event.data || {};
            if ((message.generation >>> 0) !== state.generation)
                return;

            if (message.type === 'keyPage') {
                const payload = new Uint8Array(
                    message.data || new ArrayBuffer(0));
                const wordCount = Math.floor(payload.byteLength / 4);
                let ptr = 0;
                try {
                    if (payload.byteLength) {
                        ptr = _malloc(payload.byteLength);
                        if (!ptr) return;
                        HEAPU8.set(payload, ptr);
                    }
                    _wasmidi_visual_key_page_ready(
                        message.generation >>> 0,
                        message.spanTicks >>> 0,
                        message.pageIndex >>> 0,
                        ptr,
                        wordCount >>> 0);
                } finally {
                    if (ptr) _free(ptr);
                }
                return;
            }

            if (message.type !== 'page')
                return;

            const payload = new Uint8Array(message.data || new ArrayBuffer(0));
            const count = Math.floor(payload.byteLength / 12);
            let ptr = 0;
            try {
                if (payload.byteLength) {
                    ptr = _malloc(payload.byteLength);
                    if (!ptr) return;
                    HEAPU8.set(payload, ptr);
                }
                _wasmidi_visual_page_ready(
                    message.generation >>> 0,
                    message.spanTicks >>> 0,
                    message.pageIndex >>> 0,
                    ptr,
                    count >>> 0,
                    message.sourceCount >>> 0,
                    Number(message.difficulty) || 0.0);
            } finally {
                if (ptr) _free(ptr);
            }
        };

        worker.onerror = event => {
            console.error('[WASMIDI visual cache worker]',
                          event && event.message ? event.message : event);
        };

        worker.postMessage({
            type: 'install',
            generation: state.generation,
            maxTick: maxTick >>> 0,
            notes: copy.buffer,
            keyStarts: startsCopy.buffer,
            keyEnds: endsCopy.buffer,
            keyOwners: ownersCopy.buffer
        }, [copy.buffer, startsCopy.buffer, endsCopy.buffer, ownersCopy.buffer]);
    } catch (error) {
        console.error('[WASMIDI visual cache]', error);
    }
});

EM_JS(void, wasmidi_visual_cache_shutdown, (), {
    const state = globalThis.__wasmidiVisualCache;
    if (state && state.worker) {
        try { state.worker.terminate(); } catch (_) {}
    }
    globalThis.__wasmidiVisualCache = null;
});

EM_JS(void, wasmidi_visual_cache_prime,
      (uint32_t generation, uint32_t spanTicks, uint32_t firstPage,
       uint32_t pageCount, uint32_t currentPage,
       uint32_t missingLo, uint32_t missingHi), {
    const mapped = globalThis.__wasmidiMappedMidi;
    if (mapped && mapped.worker && mapped.mappedStore) {
        mapped.worker.postMessage({
            type: 'visual-prime',
            generation: generation >>> 0,
            spanTicks: spanTicks >>> 0,
            firstPage: firstPage >>> 0,
            count: pageCount >>> 0,
            currentPage: currentPage >>> 0,
            missingLo: missingLo >>> 0,
            missingHi: missingHi >>> 0
        });
        return;
    }

    const state = globalThis.__wasmidiVisualCache;
    if (!state || !state.worker ||
        state.generation !== (generation >>> 0))
        return;

    state.worker.postMessage({
        type: 'prime',
        generation: generation >>> 0,
        spanTicks: spanTicks >>> 0,
        firstPage: firstPage >>> 0,
        count: pageCount >>> 0,
        currentPage: currentPage >>> 0,
        missingLo: missingLo >>> 0,
        missingHi: missingHi >>> 0
    });
});

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_visual_page_ready(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t noteCount,
    uint32_t sourceCount,
    double difficulty)
{
    if (g_visualCacheRenderer) {
        g_visualCacheRenderer->receiveVisualPage(
            generation, spanTicks, pageIndex, words,
            noteCount, sourceCount, difficulty);
    }
}
#endif

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
#ifdef __EMSCRIPTEN__
    g_visualCacheRenderer = this;
#endif

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

layout(location=0) in uint aStartTick;
layout(location=1) in uint aEndTick;
layout(location=2) in uint aPackedData;

uniform float uViewStart;
uniform float uViewEnd;
uniform float uCurrentTick;
uniform float uViewportWidth;
uniform int uPerTrack;
uniform vec3 uPalette[16];

flat out vec3 vColor;
flat out float vActive;

float snapTickX(float tick)
{
    float rangeTicks =
        max(1.0, uViewEnd - uViewStart);

    float normalized =
        (tick - uViewStart) /
        rangeTicks;

    // MPWGL2 converts ticks to integer texture columns with Math.round().
    // Snap to the current FBO's CSS-pixel columns to keep bar placement
    // visually identical while retaining vector/instanced rendering.
    float columns =
        max(1.0, uViewportWidth - 1.0);

    float column =
        floor(
            normalized * columns +
            0.5);

    return
        column / columns *
        2.0 -
        1.0;
}

void main()
{
    uint vertexId =
        uint(gl_VertexID);

    float useEnd =
        float(vertexId & 1u);

    float useTop =
        float(
            (vertexId >> 1u) & 1u);

    float startX =
        snapTickX(
            float(aStartTick));

    // SharpMIDI keeps currently-open notes with EndTick=0 and lets the
    // renderer extend them to the current view edge. This avoids ever needing
    // a permanent VisualNote object just to know a future NoteOff.
    float effectiveEndTick =
        aEndTick == 0u ? uViewEnd : float(aEndTick);

    float endX =
        snapTickX(
            effectiveEndTick);

    float x =
        mix(
            startX,
            endX,
            useEnd);

    uint pitch =
        (aPackedData >> 8u) &
        0xffu;

    uint globalColor =
        (aPackedData >> 16u) &
        0x0fu;

    uint trackColor =
        (aPackedData >> 20u) &
        0x0fu;

    uint colorIndex =
        uPerTrack != 0
            ? trackColor
            : globalColor;

    // Match MPWGL2:
    // rowCenter = round((pitch/127) * (height-1))
    // rowH      = max(1, floor(height/128))
    // The shader uses the equivalent normalized row center/height.
    float rowHeight =
        2.0 / 128.0;

    float centerY =
        -1.0 +
        float(pitch) /
        127.0 *
        2.0;

    float yBottom =
        clamp(
            centerY -
            rowHeight * 0.5,
            -1.0,
            1.0 -
            rowHeight);

    float yTop =
        yBottom +
        rowHeight;

    float y =
        mix(
            yBottom,
            yTop,
            useTop);

    vColor =
        uPalette[
            int(colorIndex)];

    vActive =
        (uCurrentTick >=
             float(aStartTick) &&
         uCurrentTick <=
             effectiveEndTick)
            ? 1.0
            : 0.0;

    gl_Position =
        vec4(
            x,
            y,
            0.0,
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

    // The horizontal note program is the only mandatory pipeline.
    // Background/neural effects are optional accelerations and must never make
    // the piano roll disappear if a browser/driver rejects one of their shaders.
    if (!noteProgram_)
        return false;

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

    viewportWidthUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uViewportWidth");

    perTrackUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uPerTrack");

    paletteUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uPalette[0]");

    if (backgroundProgram_) {
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
    }

    if (neuralProgram_) {
        neuralHueUniform_ =
            glGetUniformLocation(
                neuralProgram_,
                "uHue");

        neuralPointModeUniform_ =
            glGetUniformLocation(
                neuralProgram_,
                "uPointMode");
    }

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
        &noteVao_);

    glGenBuffers(
        1,
        &noteVbo_);

    glBindVertexArray(
        noteVao_);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        noteVbo_);

    glEnableVertexAttribArray(0);

    glVertexAttribIPointer(
        0, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            offsetof(
                VisualNote,
                startTick)));

    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);

    glVertexAttribIPointer(
        1, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            offsetof(
                VisualNote,
                endTick)));

    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);

    glVertexAttribIPointer(
        2, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            offsetof(
                VisualNote,
                packedData)));

    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenVertexArrays(1, &carryVao_);
    glGenBuffers(1, &carryVbo_);
    glBindVertexArray(carryVao_);
    glBindBuffer(GL_ARRAY_BUFFER, carryVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VisualNote), nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, startTick)));
    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, endTick)));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, packedData)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenVertexArrays(1, &denseVao_);
    glGenBuffers(1, &denseVbo_);
    glBindVertexArray(denseVao_);
    glBindBuffer(GL_ARRAY_BUFFER, denseVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VisualNote), nullptr, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, startTick)));
    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, endTick)));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, packedData)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

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
        0, 2, GL_FLOAT, GL_FALSE,
        sizeof(NeuralLineVertex),
        reinterpret_cast<void*>(
            offsetof(
                NeuralLineVertex,
                x)));

    glEnableVertexAttribArray(1);

    glVertexAttribPointer(
        1, 1, GL_FLOAT, GL_FALSE,
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
        0, 2, GL_FLOAT, GL_FALSE,
        sizeof(NeuralPointVertex),
        reinterpret_cast<void*>(
            offsetof(
                NeuralPointVertex,
                x)));

    glEnableVertexAttribArray(1);

    glVertexAttribPointer(
        1, 1, GL_FLOAT, GL_FALSE,
        sizeof(NeuralPointVertex),
        reinterpret_cast<void*>(
            offsetof(
                NeuralPointVertex,
                size)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    allocateRing(
        std::size_t(1) << 18);

    initialized_ = true;
    return true;
}

void GLRenderer::destroy()
{
    const GLuint buffers[] = {
        noteVbo_,
        carryVbo_,
        denseVbo_,
        neuralLineVbo_,
        neuralPointVbo_
    };

    for (GLuint buffer : buffers) {
        if (buffer)
            glDeleteBuffers(1, &buffer);
    }

    const GLuint vaos[] = {
        noteVao_,
        carryVao_,
        denseVao_,
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

    noteVbo_ = 0;
    carryVbo_ = 0;
    denseVbo_ = 0;
    neuralLineVbo_ = 0;
    neuralPointVbo_ = 0;

    noteVao_ = 0;
    carryVao_ = 0;
    denseVao_ = 0;
    backgroundVao_ = 0;
    neuralLineVao_ = 0;
    neuralPointVao_ = 0;

    noteProgram_ = 0;
    backgroundProgram_ = 0;
    neuralProgram_ = 0;

    ring_.clear();
    carryNotes_.clear();
    denseNotes_.clear();
    denseSourceScratch_.clear();
    denseCoverage_.clear();
    visualPages_.clear();
    ringCapacity_ = 0;
    ringMask_ = 0;
    sourceBegin_ = 0;
    sourceEnd_ = 0;

#ifdef __EMSCRIPTEN__
    if (g_visualCacheRenderer == this) {
        wasmidi_visual_cache_shutdown();
        g_visualCacheRenderer = nullptr;
    }
#endif

    initialized_ = false;
}

void GLRenderer::resize(int width, int height)
{
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void GLRenderer::setDocument(const MidiDocument* document)
{
    document_ = document;
    sourceBegin_ = 0;
    sourceEnd_ = 0;
    carryNotes_.clear();
    forceCacheReset_ = true;
    resetVisualPageCache(true);
}

void GLRenderer::setCurrentTime(float seconds)
{
    const float value = std::max(0.0f, seconds);
    const float delta = value - currentTime_;

    // Reverse seeks and large forward jumps must rebuild both the start-ordered
    // ring and the historical carry set. Incrementally walking every skipped
    // note on a seek is both slower and a common source of stale visual state.
    if (delta < -0.0005f ||
        std::abs(delta) > std::max(0.25f, noteSpeed_ * 0.50f)) {
        forceCacheReset_ = true;

        // A mapped-source seek invalidates the entire asynchronous tile
        // transaction, not just the legacy source ring. Pass 13.1 left old
        // pending pages/generation alive, so late replies from the previous
        // position could strand the new viewport with permanent holes.
        if (document_ && document_->remoteIndexed)
            resetVisualPageCache(false);
    }

    currentTime_ = value;
}

void GLRenderer::setNoteSpeed(float secondsPerWindow)
{
    const float value =
        std::clamp(
            secondsPerWindow,
            0.1f,
            60.0f);

    if (std::abs(noteSpeed_ - value) < 0.00001f)
        return;

    noteSpeed_ = value;
    forceCacheReset_ = true;
    resetVisualPageCache(true);
}

void GLRenderer::setPostBuffer(float seconds)
{
    const float value =
        std::clamp(
            seconds,
            0.0f,
            10.0f);

    if (std::abs(postBuffer_ - value) < 0.00001f)
        return;

    postBuffer_ = value;
    forceCacheReset_ = true;
    resetVisualPageCache(true);
}

void GLRenderer::setPerTrackColors(bool enabled)
{
    perTrackColors_ = enabled;
}

void GLRenderer::setChannelColor(
    uint8_t channel,
    uint8_t r,
    uint8_t g,
    uint8_t b)
{
    if (channel >= channelColors_.size())
        return;

    const std::array<uint8_t,4> value =
        {r,g,b,255};

    if (channelColors_[channel] == value)
        return;

    channelColors_[channel] = value;
    paletteDirty_ = true;
}

void GLRenderer::setNeuralVisual(float hue, float activity)
{
    neuralTargetHue_ = wrapHue(hue);
    neuralActivity_ =
        std::clamp(activity, 0.0f, 1.0f);
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

    if (backgroundProgram_) {
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
        glUseProgram(0);
    } else {
        // Safe visual fallback: notes remain usable even if the optional
        // background shader is rejected by a WebGL implementation.
        glClearColor(
            .010f,
            .010f,
            .026f,
            1.0f);

        glClear(
            GL_COLOR_BUFFER_BIT);
    }

    if (!neuralProgram_)
        return;

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

void GLRenderer::resetVisualPageCache(bool reinstallDocument)
{
    visualPages_.clear();
    visualPendingPages_.clear();
    visualPageSpanTicks_ = 0;
    visualWantedFirstPage_ = 0;
    visualWantedPageCount_ = 0;
    visualCurrentPage_ = 0;
    visualPrimeWidth_ = 0;
    ++visualPageRevision_;
    if (visualPageRevision_ == 0)
        ++visualPageRevision_;
    remoteDrawRevision_ = 0;
    remoteDrawSpan_ = 0;
    remoteDrawFirstPage_ = std::numeric_limits<uint32_t>::max();
    remoteDrawLastPage_ = std::numeric_limits<uint32_t>::max();
    remoteWantedDrawFirstPage_ = std::numeric_limits<uint32_t>::max();
    remoteWantedDrawLastPage_ = std::numeric_limits<uint32_t>::max();
    denseNotes_.clear();
    denseSourceScratch_.clear();

    // Every reset starts a new asynchronous page transaction. This is required
    // even for a seek-only reset where the document itself stays installed.
    ++visualCacheGeneration_;
    if (visualCacheGeneration_ == 0)
        ++visualCacheGeneration_;

    if (!reinstallDocument)
        return;

#ifdef __EMSCRIPTEN__
    // Remote-indexed documents keep source events in the persistent Memory64
    // parser Worker. There is intentionally nothing to copy into the legacy
    // visual-cache Worker here. visual_cache_prime() routes page requests to
    // the mapped Worker instead.
    if (document_ && document_->remoteIndexed) {
        wasmidi_visual_cache_shutdown();
    } else if (document_ && !document_->visualNotes.empty()) {
        const std::size_t count = document_->visualNotes.size();
        const uint32_t safeCount =
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    count,
                    std::numeric_limits<uint32_t>::max()));

        wasmidi_visual_cache_install(
            reinterpret_cast<const uint32_t*>(
                document_->visualNotes.data()),
            safeCount,
            reinterpret_cast<const uint32_t*>(
                document_->visualKeyStarts.data()),
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    document_->visualKeyStarts.size(),
                    std::numeric_limits<uint32_t>::max())),
            reinterpret_cast<const uint32_t*>(
                document_->visualKeyEnds.data()),
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    document_->visualKeyEnds.size(),
                    std::numeric_limits<uint32_t>::max())),
            reinterpret_cast<const uint32_t*>(
                document_->visualKeyOwners.data()),
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    document_->visualKeyOwners.size(),
                    std::numeric_limits<uint32_t>::max())),
            visualCacheGeneration_,
            document_->maxTick);
    }
#endif
}

void GLRenderer::receiveVisualPage(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t noteCount,
    uint32_t sourceCount,
    double difficulty)
{
    if (generation != visualCacheGeneration_ ||
        spanTicks == 0 ||
        spanTicks != visualPageSpanTicks_ ||
        visualWantedPageCount_ == 0 ||
        pageIndex < visualWantedFirstPage_ ||
        pageIndex >= visualWantedFirstPage_ + visualWantedPageCount_) {
        return;
    }

    visualPendingPages_.erase(pageIndex);

    VisualPage page;
    page.spanTicks = spanTicks;
    page.pageIndex = pageIndex;
    page.sourceCount = sourceCount;
    page.difficulty = difficulty;
    page.notes.resize(noteCount);

    if (noteCount && words) {
        std::memcpy(
            page.notes.data(),
            words,
            std::size_t(noteCount) * sizeof(VisualNote));
    }

    if (visualPages_.size() >= 64 &&
        visualPages_.find(pageIndex) == visualPages_.end()) {
        auto victim = visualPages_.end();
        uint32_t farthest = 0;
        for (auto it = visualPages_.begin(); it != visualPages_.end(); ++it) {
            const uint32_t distance =
                it->first > visualCurrentPage_
                    ? it->first - visualCurrentPage_
                    : visualCurrentPage_ - it->first;
            if (victim == visualPages_.end() || distance > farthest) {
                victim = it;
                farthest = distance;
            }
        }
        if (victim != visualPages_.end())
            visualPages_.erase(victim);
    }

    visualPages_[pageIndex] = std::move(page);

    // Far-ahead pages must not force the current VBO to be concatenated and
    // re-uploaded. Only pages the current viewport is waiting for invalidate
    // the assembled remote draw list.
    if (remoteWantedDrawFirstPage_ == std::numeric_limits<uint32_t>::max() ||
        (pageIndex >= remoteWantedDrawFirstPage_ &&
         pageIndex <= remoteWantedDrawLastPage_)) {
        ++visualPageRevision_;
        if (visualPageRevision_ == 0)
            ++visualPageRevision_;
    }
}

void GLRenderer::primeVisualPageCache(
    uint32_t viewStart,
    uint32_t viewEnd)
{
    if (!document_ ||
        (!document_->remoteIndexed && document_->visualNotes.empty()))
        return;

    const uint32_t actualSpan =
        std::max<uint32_t>(1, viewEnd - viewStart);

    if (visualPageSpanTicks_ == 0) {
        // A page is a cache tile, not a promise that every later viewport must
        // equal exactly one page. Keep this span stable across tempo changes.
        // Rebuilding all 64 pages whenever ticks-per-screen changes was far
        // slower than rendering directly and caused visible cache holes.
        visualPageSpanTicks_ = actualSpan;
        visualPages_.clear();
    }

    const uint32_t span = std::max<uint32_t>(1, visualPageSpanTicks_);
    const uint32_t currentPage = viewStart / span;
    const uint32_t firstPage = currentPage > 2 ? currentPage - 2 : 0;
    const uint32_t maxPage = document_->maxTick / span;
    const uint32_t available =
        maxPage >= firstPage ? maxPage - firstPage + 1 : 1;
    const uint32_t pageCount = std::min<uint32_t>(64, available);

    const bool requestChanged =
        firstPage != visualWantedFirstPage_ ||
        pageCount != visualWantedPageCount_ ||
        currentPage != visualCurrentPage_;

    visualWantedFirstPage_ = firstPage;
    visualWantedPageCount_ = pageCount;
    visualCurrentPage_ = currentPage;

    // A new prime message supersedes the Worker's current speculative build.
    // Forget in-flight bookkeeping so every still-missing tile in the new
    // window is explicitly requested again; otherwise a cancelled old request
    // can leave a permanent hole after seeking.
    if (requestChanged)
        visualPendingPages_.clear();

    for (auto it = visualPages_.begin(); it != visualPages_.end();) {
        if (it->second.spanTicks != span ||
            it->first < firstPage ||
            it->first >= firstPage + pageCount) {
            it = visualPages_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = visualPendingPages_.begin();
         it != visualPendingPages_.end();) {
        if (*it < firstPage || *it >= firstPage + pageCount)
            it = visualPendingPages_.erase(it);
        else
            ++it;
    }

    // Tell the Worker exactly which rolling tiles are absent AND not already
    // in flight. Unlike Pass 13.1, this is not gated solely on requestChanged:
    // if a page reply was dropped/cancelled, the renderer can recover it.
    uint32_t missingLo = 0;
    uint32_t missingHi = 0;
    for (uint32_t i = 0; i < pageCount; ++i) {
        const uint32_t pageIndex = firstPage + i;
        const auto cached = visualPages_.find(pageIndex);
        const bool missing =
            cached == visualPages_.end() || cached->second.spanTicks != span;
        if (!missing || visualPendingPages_.find(pageIndex) != visualPendingPages_.end())
            continue;
        if (i < 32)
            missingLo |= uint32_t(1) << i;
        else
            missingHi |= uint32_t(1) << (i - 32);
    }

#ifdef __EMSCRIPTEN__
    if (missingLo != 0 || missingHi != 0) {
        for (uint32_t i = 0; i < pageCount; ++i) {
            const bool requested =
                i < 32
                    ? ((missingLo >> i) & 1u) != 0
                    : ((missingHi >> (i - 32)) & 1u) != 0;
            if (requested)
                visualPendingPages_.insert(firstPage + i);
        }

        wasmidi_visual_cache_prime(
            visualCacheGeneration_,
            span,
            firstPage,
            pageCount,
            currentPage,
            missingLo,
            missingHi);
    }
#else
    (void)requestChanged;
    (void)missingLo;
    (void)missingHi;
#endif
}

bool GLRenderer::collectCachedPageNotes(
    uint32_t searchStart,
    uint32_t viewEnd,
    std::vector<VisualNote>& output) const
{
    output.clear();

    if (visualPageSpanTicks_ == 0 || visualPages_.empty())
        return false;

    const uint32_t span = visualPageSpanTicks_;
    const uint32_t firstPage = searchStart / span;
    const uint32_t lastPage = viewEnd / span;

    // A normal viewport touches at most three screen pages (one history page
    // plus the visible/future range). Refuse pathological stale-scale cases.
    if (lastPage < firstPage || lastPage - firstPage > 63)
        return false;

    std::size_t reserveCount = 0;
    for (uint32_t pageIndex = firstPage;
         pageIndex <= lastPage;
         ++pageIndex) {
        const auto it = visualPages_.find(pageIndex);
        if (it == visualPages_.end() ||
            it->second.spanTicks != span) {
            return false;
        }
        reserveCount += it->second.notes.size();
    }

    output.reserve(reserveCount);

    // Each tile is independently seekable, so it repeats notes that began in
    // an earlier tile and are still alive at this tile's left edge. Never draw
    // those carries twice: doing so changes MPWGL2's stable-start overwrite
    // order. Instead reconcile a later carry with the already-emitted open
    // instance. If the later tile contains its NoteOff, it also supplies the
    // real endTick so a long note stops at exactly the correct position.
    std::unordered_map<uint64_t, std::vector<std::size_t>> openByVisualKey;
    openByVisualKey.reserve(1024);

    const auto visualKey = [](const VisualNote& note) -> uint64_t {
        return (uint64_t(note.startTick) << 32) |
               uint64_t(note.packedData);
    };

    for (uint32_t pageIndex = firstPage;
         pageIndex <= lastPage;
         ++pageIndex) {
        const auto& page = visualPages_.at(pageIndex);
        const uint64_t pageStart64 = uint64_t(pageIndex) * uint64_t(span);
        const uint32_t pageStart = static_cast<uint32_t>(
            std::min<uint64_t>(
                pageStart64,
                std::numeric_limits<uint32_t>::max()));

        // For identical-looking overlapping notes, map carries to prior open
        // instances in FIFO order. MPWGL2 also pairs repeated NoteOns FIFO.
        std::unordered_map<uint64_t, std::size_t> carryCursor;

        for (const VisualNote& note : page.notes) {
            if (document_ && document_->remoteIndexed) {
                if (note.startTick > viewEnd ||
                    (note.endTick != 0 && note.endTick < searchStart)) {
                    continue;
                }

                const bool carry =
                    pageIndex != firstPage &&
                    note.startTick < pageStart;

                bool reconciledCarry = false;
                if (carry) {
                    const uint64_t key = visualKey(note);
                    const auto found = openByVisualKey.find(key);
                    if (found != openByVisualKey.end()) {
                        std::size_t& cursor = carryCursor[key];
                        auto& candidates = found->second;

                        while (cursor < candidates.size()) {
                            const std::size_t outputIndex = candidates[cursor++];
                            if (outputIndex >= output.size())
                                continue;

                            VisualNote& existing = output[outputIndex];
                            if (existing.endTick != 0 &&
                                existing.endTick < pageStart) {
                                continue;
                            }

                            if (note.endTick != 0)
                                existing.endTick = note.endTick;

                            reconciledCarry = true;
                            break;
                        }
                    }
                    // No earlier copy is resident (normally only possible when
                    // the first requested tile changed during recovery). Keep
                    // this carry so the screen remains complete.
                }

                if (!reconciledCarry) {
                    output.push_back(note);
                    if (note.endTick == 0) {
                        openByVisualKey[visualKey(note)].push_back(
                            output.size() - 1);
                    }
                }
                continue;
            }

            if (note.startTick < searchStart || note.startTick > viewEnd)
                continue;
            output.push_back(note);
        }
    }

    // Pages are already emitted in MPWGL2 stable-start order. Because the
    // cache uses half-open tick tiles, every non-carry note belongs to exactly
    // one page; concatenating pages in ascending page order therefore preserves
    // that global order. Re-sorting here used incomplete carry end-times and
    // could undo the parser's exact closure ordering after a seek.

    return true;
}

bool GLRenderer::buildDenseDrawList(
    uint32_t viewStart,
    uint32_t viewEnd,
    std::size_t desiredBegin,
    std::size_t desiredEnd)
{
    if (!document_ || viewEnd <= viewStart)
        return false;

    constexpr std::size_t DenseThreshold = 8192;
    constexpr std::size_t MaxRawRecoveryNotes = 1000000;

    const std::size_t rawVisibleCount =
        carryNotes_.size() +
        (desiredEnd > desiredBegin ? desiredEnd - desiredBegin : 0);

    if (rawVisibleCount < DenseThreshold)
        return false;

    const uint32_t span = viewEnd - viewStart;
    const uint32_t searchStart = viewStart > span ? viewStart - span : 0;

    std::vector<VisualNote> pageNotes;
    const bool pagesReady =
        collectCachedPageNotes(searchStart, viewEnd, pageNotes);

    if (!pagesReady && rawVisibleCount > MaxRawRecoveryNotes) {
        // Recovery path: do not replace a GPU-heavy frame with a million-note
        // CPU copy while the background page Worker is still finishing it.
        return false;
    }

    denseSourceScratch_.clear();
    denseSourceScratch_.reserve(
        carryNotes_.size() +
        (pagesReady
            ? pageNotes.size()
            : (desiredEnd > desiredBegin ? desiredEnd - desiredBegin : 0)));

    denseSourceScratch_.insert(
        denseSourceScratch_.end(),
        carryNotes_.begin(),
        carryNotes_.end());

    if (pagesReady) {
        denseSourceScratch_.insert(
            denseSourceScratch_.end(),
            pageNotes.begin(),
            pageNotes.end());
    } else {
        const auto& notes = document_->visualNotes;
        const std::size_t begin = std::min(desiredBegin, notes.size());
        const std::size_t end = std::min(desiredEnd, notes.size());
        denseSourceScratch_.insert(
            denseSourceScratch_.end(),
            notes.begin() + static_cast<std::ptrdiff_t>(begin),
            notes.begin() + static_cast<std::ptrdiff_t>(end));
    }

    if (denseSourceScratch_.empty())
        return false;

    const int columns = std::max(1, width_ - 1);
    const std::size_t wordsPerPitch =
        (std::size_t(columns) + 63u) / 64u;
    denseCoverage_.assign(
        std::size_t(128) * wordsPerPitch,
        uint64_t(0));
    denseNotes_.clear();
    denseNotes_.reserve(
        std::min<std::size_t>(
            denseSourceScratch_.size(),
            std::size_t(columns) * 128u));

    const double tickSpan = double(viewEnd - viewStart);

    auto snappedColumn = [&](uint32_t tick) -> int {
        const double normalized =
            (double(tick) - double(viewStart)) / tickSpan;
        return static_cast<int>(
            std::floor(normalized * double(columns) + 0.5));
    };

    // Reverse source order: later notes are opaque and therefore define which
    // pixels an earlier note could still possibly contribute. We never split a
    // partially visible note; only fully hidden/sub-pixel-equivalent notes are
    // removed, preserving stacking, clipping and any per-note shading.
    for (auto it = denseSourceScratch_.rbegin();
         it != denseSourceScratch_.rend();
         ++it) {
        const VisualNote& note = *it;
        const int pitch = int((note.packedData >> 8) & 0x7f);

        int left = snappedColumn(note.startTick);
        int right = snappedColumn(note.endTick);
        if (right < left)
            std::swap(left, right);

        left = std::clamp(left, 0, columns);
        right = std::clamp(right, 0, columns);

        // Equal snapped endpoints create a zero-area triangle strip in the
        // current shader, so dropping them is exactly raster-equivalent.
        if (right <= left)
            continue;

        const std::size_t rowBase = std::size_t(pitch) * wordsPerPitch;
        const std::size_t firstWord = std::size_t(left) >> 6u;
        const std::size_t lastWord = std::size_t(right - 1) >> 6u;
        bool contributes = false;

        for (std::size_t word = firstWord; word <= lastWord; ++word) {
            const unsigned lo =
                word == firstWord
                    ? unsigned(std::size_t(left) & 63u)
                    : 0u;
            const unsigned hi =
                word == lastWord
                    ? unsigned((std::size_t(right - 1) & 63u) + 1u)
                    : 64u;

            const uint64_t lowMask =
                lo == 0u ? ~uint64_t(0) : (~uint64_t(0) << lo);
            const uint64_t highMask =
                hi == 64u ? ~uint64_t(0) : ((uint64_t(1) << hi) - 1u);
            const uint64_t mask = lowMask & highMask;

            if ((~denseCoverage_[rowBase + word] & mask) != 0u) {
                contributes = true;
                break;
            }
        }

        if (!contributes)
            continue;

        denseNotes_.push_back(note);

        for (std::size_t word = firstWord; word <= lastWord; ++word) {
            const unsigned lo =
                word == firstWord
                    ? unsigned(std::size_t(left) & 63u)
                    : 0u;
            const unsigned hi =
                word == lastWord
                    ? unsigned((std::size_t(right - 1) & 63u) + 1u)
                    : 64u;
            const uint64_t lowMask =
                lo == 0u ? ~uint64_t(0) : (~uint64_t(0) << lo);
            const uint64_t highMask =
                hi == 64u ? ~uint64_t(0) : ((uint64_t(1) << hi) - 1u);
            denseCoverage_[rowBase + word] |= lowMask & highMask;
        }
    }

    std::reverse(denseNotes_.begin(), denseNotes_.end());

    // If a sparse-looking dense range cannot be reduced meaningfully, retain
    // the persistent source ring instead of uploading a nearly identical VBO.
    if (denseNotes_.size() * 10 >= denseSourceScratch_.size() * 9)
        return false;

    uploadDenseDrawList();
    return !denseNotes_.empty();
}

void GLRenderer::uploadDenseDrawList()
{
    if (!initialized_ || !denseVbo_)
        return;

    glBindBuffer(GL_ARRAY_BUFFER, denseVbo_);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(denseNotes_.size() * sizeof(VisualNote)),
        denseNotes_.empty() ? nullptr : denseNotes_.data(),
        GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLRenderer::drawDenseNotes()
{
    if (denseNotes_.empty())
        return;

    glBindVertexArray(denseVao_);
    glDrawArraysInstanced(
        GL_TRIANGLE_STRIP,
        0,
        4,
        static_cast<GLsizei>(denseNotes_.size()));
}

void GLRenderer::allocateRing(std::size_t capacity)
{
    std::size_t rounded = 1;

    while (rounded < capacity)
        rounded <<= 1;

    ringCapacity_ = rounded;
    ringMask_ = ringCapacity_ - 1;
    ring_.resize(ringCapacity_);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        noteVbo_);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            ringCapacity_ *
            sizeof(VisualNote)),
        nullptr,
        GL_DYNAMIC_DRAW);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);

    // Re-upload from the immutable source; no ring-to-ring copy required.
    if (document_ &&
        sourceEnd_ > sourceBegin_) {
        uploadSourceRange(
            sourceBegin_,
            sourceEnd_);
    }
}

void GLRenderer::ensureRingCapacity(std::size_t required)
{
    if (required <= ringCapacity_)
        return;

    std::size_t target =
        std::max<std::size_t>(
            ringCapacity_,
            std::size_t(1) << 18);

    while (target < required)
        target <<= 1;

    allocateRing(target);
}

void GLRenderer::uploadSourceRange(
    std::size_t begin,
    std::size_t end)
{
    if (!document_ ||
        begin >= end ||
        ringCapacity_ == 0) {
        return;
    }

    const auto& notes =
        document_->visualNotes;

    if (end > notes.size())
        end = notes.size();

    glBindBuffer(
        GL_ARRAY_BUFFER,
        noteVbo_);

    std::size_t cursor = begin;

    while (cursor < end) {
        const std::size_t physical =
            cursor & ringMask_;

        const std::size_t count =
            std::min(
                end - cursor,
                ringCapacity_ - physical);

        // Copy only the newly entering contiguous source segment into the CPU
        // mirror and issue one matching contiguous WebGL transfer.
        for (std::size_t i = 0;
             i < count;
             ++i) {
            ring_[physical + i] =
                notes[cursor + i];
        }

        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(
                physical *
                sizeof(VisualNote)),
            static_cast<GLsizeiptr>(
                count *
                sizeof(VisualNote)),
            ring_.data() + physical);

        cursor += count;
    }

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);
}

void GLRenderer::rebuildVisualCache(
    std::size_t begin,
    std::size_t end)
{
    sourceBegin_ = begin;
    sourceEnd_ = end;

    ensureRingCapacity(
        std::max<std::size_t>(
            1,
            sourceEnd_ -
            sourceBegin_));

    uploadSourceRange(
        sourceBegin_,
        sourceEnd_);

    forceCacheReset_ = false;
}

void GLRenderer::uploadCarryCache()
{
    if (!initialized_ || carryVbo_ == 0)
        return;

    glBindBuffer(
        GL_ARRAY_BUFFER,
        carryVbo_);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            std::max<std::size_t>(
                1,
                carryNotes_.size()) *
            sizeof(VisualNote)),
        carryNotes_.empty()
            ? nullptr
            : carryNotes_.data(),
        GL_DYNAMIC_DRAW);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);
}

void GLRenderer::rebuildCarryCache(
    uint32_t viewStart,
    std::size_t desiredBegin)
{
    carryNotes_.clear();

    if (!document_ || desiredBegin == 0) {
        uploadCarryCache();
        return;
    }

    const auto& notes =
        document_->visualNotes;

    const auto& blockMaxEnd =
        document_->visualBlockMaxEnd;

    const std::size_t blockSize =
        MidiDocument::VisualSeekBlockSize;

    const std::size_t lastBlock =
        (desiredBegin + blockSize - 1) /
        blockSize;

    // A seek can land in the middle of a very long note. Search only source
    // blocks whose maximum end tick can still intersect the visible history;
    // this avoids rescanning every old note while preserving exact source order.
    for (std::size_t block = 0;
         block < lastBlock;
         ++block) {
        if (block < blockMaxEnd.size() &&
            blockMaxEnd[block] < viewStart) {
            continue;
        }

        const std::size_t begin =
            block * blockSize;

        const std::size_t end =
            std::min(
                desiredBegin,
                begin + blockSize);

        for (std::size_t i = begin;
             i < end;
             ++i) {
            if (notes[i].endTick >= viewStart)
                carryNotes_.push_back(notes[i]);
        }
    }

    uploadCarryCache();
}

void GLRenderer::advanceCarryCache(
    uint32_t viewStart,
    std::size_t oldBegin,
    std::size_t desiredBegin)
{
    if (!document_) {
        carryNotes_.clear();
        uploadCarryCache();
        return;
    }

    if (desiredBegin < oldBegin) {
        rebuildCarryCache(
            viewStart,
            desiredBegin);
        return;
    }

    bool changed = false;

    const auto oldSize =
        carryNotes_.size();

    carryNotes_.erase(
        std::remove_if(
            carryNotes_.begin(),
            carryNotes_.end(),
            [viewStart](const VisualNote& note) {
                return note.endTick < viewStart;
            }),
        carryNotes_.end());

    changed =
        carryNotes_.size() != oldSize;

    const auto& notes =
        document_->visualNotes;

    const std::size_t appendEnd =
        std::min(
            desiredBegin,
            notes.size());

    for (std::size_t i = oldBegin;
         i < appendEnd;
         ++i) {
        if (notes[i].endTick >= viewStart) {
            carryNotes_.push_back(notes[i]);
            changed = true;
        }
    }

    if (changed)
        uploadCarryCache();
}

void GLRenderer::syncVisualCache(
    uint32_t viewStart,
    uint32_t viewEnd)
{
    if (document_ && document_->remoteIndexed) {
        sourceBegin_ = 0;
        sourceEnd_ = 0;
        if (!carryNotes_.empty()) {
            carryNotes_.clear();
            uploadCarryCache();
        }
        return;
    }

    if (!document_ ||
        document_->visualNotes.empty()) {
        sourceBegin_ = 0;
        sourceEnd_ = 0;
        if (!carryNotes_.empty()) {
            carryNotes_.clear();
            uploadCarryCache();
        }
        return;
    }

    const uint32_t span =
        std::max<uint32_t>(
            1,
            viewEnd - viewStart);

    // This reproduces MPWGL2's _writeStrip search range for a full roll:
    // lowerBound(stripStart - windowTicks - postTicks). With viewStart equal
    // to stripStart and span=(window+post), this is viewStart-span.
    const uint32_t searchStart =
        viewStart > span
            ? viewStart - span
            : 0;

    const std::size_t desiredBegin =
        document_->
            lowerBoundVisualStart(
                double(searchStart));

    const std::size_t desiredEnd =
        document_->
            upperBoundVisualStart(
                double(viewEnd));

    const bool forwardCompatible =
        !forceCacheReset_ &&
        desiredBegin >= sourceBegin_ &&
        desiredEnd >= sourceEnd_ &&
        desiredBegin <= sourceEnd_;

    if (!forwardCompatible) {
        rebuildVisualCache(
            desiredBegin,
            desiredEnd);
        rebuildCarryCache(
            viewStart,
            desiredBegin);
        return;
    }

    advanceCarryCache(
        viewStart,
        sourceBegin_,
        desiredBegin);

    const std::size_t required =
        desiredEnd - desiredBegin;

    if (required > ringCapacity_) {
        sourceBegin_ = desiredBegin;
        sourceEnd_ = desiredEnd;
        ensureRingCapacity(required);
        // allocateRing() already re-uploaded the new source range.
        forceCacheReset_ = false;
        return;
    }

    if (desiredEnd > sourceEnd_) {
        uploadSourceRange(
            sourceEnd_,
            desiredEnd);
    }

    sourceBegin_ = desiredBegin;
    sourceEnd_ = desiredEnd;
}

void GLRenderer::setInstanceBase(
    std::size_t physicalIndex)
{
    const std::size_t base =
        physicalIndex *
        sizeof(VisualNote);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        noteVbo_);

    glVertexAttribIPointer(
        0, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                VisualNote,
                startTick)));

    glVertexAttribIPointer(
        1, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                VisualNote,
                endTick)));

    glVertexAttribIPointer(
        2, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                VisualNote,
                packedData)));
}

void GLRenderer::calculateView(
    uint32_t& currentTick,
    uint32_t& viewStart,
    uint32_t& viewEnd) const
{
    if (!document_) {
        currentTick = viewStart = viewEnd = 0;
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
            document_->durationSeconds,
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
                        document_->maxTick))));

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
}
void GLRenderer::renderRoll()
{
    if (!initialized_ &&
        !initialize()) {
        return;
    }

    glViewport(
        0, 0,
        width_, height_);

    renderBackground();

    if (!document_ ||
        (!document_->remoteIndexed && document_->visualNotes.empty())) {
        return;
    }

    uint32_t currentTick = 0;
    uint32_t viewStart = 0;
    uint32_t viewEnd = 0;

    calculateView(
        currentTick,
        viewStart,
        viewEnd);

    primeVisualPageCache(
        viewStart,
        viewEnd);

    syncVisualCache(
        viewStart,
        viewEnd);

    const uint32_t visibleSpan =
        std::max<uint32_t>(1, viewEnd - viewStart);
    const uint32_t searchStart =
        viewStart > visibleSpan
            ? viewStart - visibleSpan
            : 0;

    std::size_t desiredBegin = 0;
    std::size_t desiredEnd = 0;
    bool denseMode = false;

    if (document_->remoteIndexed) {
        // Keep the last complete geometry resident on the GPU while recovery
        // pages are still being decoded. The shader clips old geometry against
        // the new viewport, so notes leave only after scrolling off-screen
        // instead of disappearing for a frame whenever one cache tile is late.
        const uint32_t pageSpan = std::max<uint32_t>(1, visualPageSpanTicks_);
        const uint32_t drawFirst = searchStart / pageSpan;
        const uint32_t drawLast = viewEnd / pageSpan;
        remoteWantedDrawFirstPage_ = drawFirst;
        remoteWantedDrawLastPage_ = drawLast;

        const bool needsComposite =
            remoteDrawSpan_ != pageSpan ||
            remoteDrawFirstPage_ != drawFirst ||
            remoteDrawLastPage_ != drawLast ||
            remoteDrawRevision_ != visualPageRevision_;

        if (needsComposite) {
            std::vector<VisualNote> pageNotes;
            if (collectCachedPageNotes(searchStart, viewEnd, pageNotes)) {
                denseNotes_ = std::move(pageNotes);
                uploadDenseDrawList();
                remoteDrawSpan_ = pageSpan;
                remoteDrawFirstPage_ = drawFirst;
                remoteDrawLastPage_ = drawLast;
                remoteDrawRevision_ = visualPageRevision_;
            }
        }

        // If a new composite is not ready, keep drawing the previous one. This
        // is the frame-buffer recovery path requested by the player design.
        if (denseNotes_.empty())
            return;
        denseMode = true;
    } else {
        if (sourceEnd_ <= sourceBegin_ && carryNotes_.empty())
            return;

        desiredBegin = document_->lowerBoundVisualStart(double(searchStart));
        desiredEnd = document_->upperBoundVisualStart(double(viewEnd));
        denseMode = buildDenseDrawList(
            viewStart,
            viewEnd,
            desiredBegin,
            desiredEnd);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

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

    glUniform1f(
        viewportWidthUniform_,
        static_cast<float>(
            std::max(1, width_)));

    glUniform1i(
        perTrackUniform_,
        perTrackColors_ ? 1 : 0);

    if (paletteDirty_) {
        GLfloat palette[16 * 3];

        for (int i = 0;
             i < 16;
             ++i) {
            palette[i * 3 + 0] =
                channelColors_[i][0] /
                255.0f;

            palette[i * 3 + 1] =
                channelColors_[i][1] /
                255.0f;

            palette[i * 3 + 2] =
                channelColors_[i][2] /
                255.0f;
        }

        glUniform3fv(
            paletteUniform_,
            16,
            palette);

        paletteDirty_ = false;
    }

    if (denseMode) {
        // One compact draw after the resolution-dependent visibility pass.
        // Source order inside denseNotes_ is unchanged.
        drawDenseNotes();
    } else {
        // Recovery/sparse path: draw the persistent source ring directly. This
        // remains available whenever a background page is unfinished or dense
        // culling would not save enough work to justify a transient upload.
        if (!carryNotes_.empty()) {
            glBindVertexArray(
                carryVao_);

            glDrawArraysInstanced(
                GL_TRIANGLE_STRIP,
                0,
                4,
                static_cast<GLsizei>(
                    carryNotes_.size()));
        }

        if (sourceEnd_ > sourceBegin_) {
            const std::size_t count =
                sourceEnd_ - sourceBegin_;

            const std::size_t physical =
                sourceBegin_ & ringMask_;

            const std::size_t first =
                std::min(
                    count,
                    ringCapacity_ - physical);

            const std::size_t second =
                count - first;

            glBindVertexArray(noteVao_);

            // Exact source/start order is the MPWGL2 parity rule: later-start
            // notes overwrite earlier notes wherever they overlap.
            if (first != 0) {
                setInstanceBase(physical);
                glDrawArraysInstanced(
                    GL_TRIANGLE_STRIP,
                    0,
                    4,
                    static_cast<GLsizei>(first));
            }

            if (second != 0) {
                setInstanceBase(0);
                glDrawArraysInstanced(
                    GL_TRIANGLE_STRIP,
                    0,
                    4,
                    static_cast<GLsizei>(second));
            }
        }
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

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
