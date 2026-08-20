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

GLuint linkProgram(const char* vertexSource, const char* fragmentSource)
{
    const GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);

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

} // namespace

GLRenderer::GLRenderer()
{
    const uint8_t defaults[16][4] = {
        {129,140,248,255},{167,139,250,255},{196,181,253,255},{251,146, 60,255},
        { 74,222,128,255},{ 56,189,248,255},{244,114,182,255},{250,204, 21,255},
        {248,113,113,255},{ 52,211,153,255},{ 96,165,250,255},{232,121,249,255},
        {251,113,133,255},{163,230, 53,255},{ 34,211,238,255},{251,191, 36,255}
    };

    for (int i = 0; i < 16; ++i) {
        channelColors_[i] = {
            defaults[i][0], defaults[i][1],
            defaults[i][2], defaults[i][3]
        };
        globalChannelColor_[i] = static_cast<int8_t>(i);
    }

    tempoMap_.push_back({0, 500000});
    rebuildTempoIndex();
    recalcTickWindow();
}

GLRenderer::~GLRenderer()
{
    destroy();
}

bool GLRenderer::initialize()
{
    if (initialized_)
        return true;

    if (!createPrograms())
        return false;

    glGenVertexArrays(1, &emptyVao_);

    initialized_ = true;
    forceFullRedraw_ = true;
    return true;
}

bool GLRenderer::createPrograms()
{
    static const char* fullScreenVertex = R"GLSL(#version 300 es
precision highp float;
void main()
{
    vec2 pos = vec2(
        (gl_VertexID & 1) == 0 ? -1.0 : 1.0,
        (gl_VertexID & 2) == 0 ? -1.0 : 1.0
    );
    gl_Position = vec4(pos, 0.0, 1.0);
}
)GLSL";

    static const char* scrollFragment = R"GLSL(#version 300 es
precision mediump float;
uniform sampler2D uTex;
uniform float uPlayheadX;
uniform float uPlayheadWidth;
out vec4 fragColor;

void main()
{
    ivec2 sizePx = textureSize(uTex, 0);
    vec2 uv = gl_FragCoord.xy / vec2(sizePx);

    // Same vertical texture convention as MPWGL2.html.
    uv.y = 1.0 - uv.y;

    vec4 color = texture(uTex, uv);

    float distanceToPlayhead =
        abs(gl_FragCoord.x / float(sizePx.x) - uPlayheadX);

    if (distanceToPlayhead < uPlayheadWidth) {
        color = mix(
            color,
            vec4(0.63, 0.54, 0.98, 1.0),
            (1.0 - distanceToPlayhead / uPlayheadWidth) * 0.92
        );
    }

    fragColor = color;
}
)GLSL";

    static const char* blitFragment = R"GLSL(#version 300 es
precision mediump float;
uniform sampler2D uSource;
uniform float uOffsetU;
out vec4 fragColor;

void main()
{
    ivec2 sizePx = textureSize(uSource, 0);
    vec2 uv = gl_FragCoord.xy / vec2(sizePx);
    uv.x += uOffsetU;

    fragColor = uv.x > 1.0
        ? vec4(0.0)
        : texture(uSource, uv);
}
)GLSL";

    scrollProgram_ = linkProgram(fullScreenVertex, scrollFragment);
    blitProgram_ = linkProgram(fullScreenVertex, blitFragment);

    if (!scrollProgram_ || !blitProgram_)
        return false;

    scrollTexUniform_ =
        glGetUniformLocation(scrollProgram_, "uTex");
    playheadXUniform_ =
        glGetUniformLocation(scrollProgram_, "uPlayheadX");
    playheadWidthUniform_ =
        glGetUniformLocation(scrollProgram_, "uPlayheadWidth");

    blitSourceUniform_ =
        glGetUniformLocation(blitProgram_, "uSource");
    blitOffsetUniform_ =
        glGetUniformLocation(blitProgram_, "uOffsetU");

    return true;
}

void GLRenderer::destroyTextures()
{
    if (textures_[0] || textures_[1])
        glDeleteTextures(2, textures_);
    if (framebuffers_[0] || framebuffers_[1])
        glDeleteFramebuffers(2, framebuffers_);

    textures_[0] = textures_[1] = 0;
    framebuffers_[0] = framebuffers_[1] = 0;

    textureWidth_ = 0;
    textureHeight_ = 0;
}

void GLRenderer::destroy()
{
    destroyTextures();

    if (emptyVao_)
        glDeleteVertexArrays(1, &emptyVao_);
    if (scrollProgram_)
        glDeleteProgram(scrollProgram_);
    if (blitProgram_)
        glDeleteProgram(blitProgram_);

    emptyVao_ = 0;
    scrollProgram_ = 0;
    blitProgram_ = 0;
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

    std::stable_sort(
        notes_.begin(), notes_.end(),
        [](const NoteInstance& a, const NoteInstance& b) {
            if (a.startTick != b.startTick)
                return a.startTick < b.startTick;
            return a.endTick < b.endTick;
        });

    rebuildColorMaps();
    forceFullRedraw_ = true;
    lastRenderTick_ = -1.0;
}

void GLRenderer::setTempoMap(
    const std::vector<TempoPoint>& tempoMap,
    uint16_t ticksPerBeat)
{
    tempoMap_ = tempoMap;
    if (tempoMap_.empty())
        tempoMap_.push_back({0, 500000});

    ppq_ = std::max<uint16_t>(1, ticksPerBeat);

    rebuildTempoIndex();
    recalcTickWindow();

    forceFullRedraw_ = true;
    lastRenderTick_ = -1.0;
}

void GLRenderer::setActiveChannelMasks(
    const std::vector<uint32_t>& activeChannelMasks)
{
    activeChannelMasks_ = activeChannelMasks;
    rebuildColorMaps();
    forceFullRedraw_ = true;
}

void GLRenderer::setCurrentTime(float seconds)
{
    currentTime_ = std::max(0.0f, seconds);
}

void GLRenderer::setNoteSpeed(float secondsPerWindow)
{
    const float value = std::max(0.1f, secondsPerWindow);
    if (std::abs(noteSpeed_ - value) < 0.00001f)
        return;

    noteSpeed_ = value;
    recalcTickWindow();
    forceFullRedraw_ = true;
    lastRenderTick_ = -1.0;
}

void GLRenderer::setPostBuffer(float seconds)
{
    const float value = std::max(0.0f, seconds);
    if (std::abs(postBuffer_ - value) < 0.00001f)
        return;

    postBuffer_ = value;
    recalcTickWindow();
    forceFullRedraw_ = true;
    lastRenderTick_ = -1.0;
}

void GLRenderer::setPerTrackColors(bool enabled)
{
    if (perTrackColors_ == enabled)
        return;

    perTrackColors_ = enabled;
    forceFullRedraw_ = true;
}

void GLRenderer::setChannelColor(
    uint8_t channel,
    uint8_t r, uint8_t g, uint8_t b)
{
    if (channel >= channelColors_.size())
        return;

    const std::array<uint8_t,4> value = {r,g,b,255};
    if (channelColors_[channel] == value)
        return;

    channelColors_[channel] = value;
    forceFullRedraw_ = true;
}

void GLRenderer::rebuildTempoIndex()
{
    tempoTicks_.clear();
    tempoSeconds_.clear();
    tempoUsPerBeat_.clear();

    double seconds = 0.0;
    uint32_t previousTick = 0;
    uint32_t usPerBeat = 500000;

    for (const auto& tempo : tempoMap_) {
        seconds +=
            (double(tempo.tick - previousTick) / double(ppq_)) *
            (double(usPerBeat) / 1'000'000.0);

        tempoTicks_.push_back(double(tempo.tick));
        tempoSeconds_.push_back(seconds);
        tempoUsPerBeat_.push_back(
            double(tempo.microsecondsPerBeat));

        previousTick = tempo.tick;
        if (tempo.microsecondsPerBeat != 0)
            usPerBeat = tempo.microsecondsPerBeat;
    }

    if (tempoTicks_.empty()) {
        tempoTicks_.push_back(0.0);
        tempoSeconds_.push_back(0.0);
        tempoUsPerBeat_.push_back(500000.0);
    }
}

double GLRenderer::secToTick(double seconds) const
{
    if (tempoSeconds_.empty())
        return seconds * double(ppq_) * 2.0;

    const auto upper =
        std::upper_bound(
            tempoSeconds_.begin(),
            tempoSeconds_.end(),
            seconds);

    std::size_t index = 0;
    if (upper != tempoSeconds_.begin())
        index = static_cast<std::size_t>(
            (upper - tempoSeconds_.begin()) - 1);

    index = std::min(index, tempoTicks_.size() - 1);

    const double us =
        tempoUsPerBeat_[index] > 0.0
            ? tempoUsPerBeat_[index]
            : 500000.0;

    return tempoTicks_[index] +
        (seconds - tempoSeconds_[index]) *
        double(ppq_) / (us / 1'000'000.0);
}

void GLRenderer::recalcTickWindow()
{
    const double base = secToTick(0.0);

    windowTicks_ = std::max(
        1.0,
        std::round(secToTick(noteSpeed_) - base));

    postTicks_ = std::max(
        0.0,
        std::round(secToTick(postBuffer_) - base));
}

double GLRenderer::ticksPerColumn() const
{
    return std::max(
        1.0,
        (windowTicks_ + postTicks_) /
        double(std::max(1, textureWidth_)));
}

std::size_t GLRenderer::lowerBoundTick(double tick) const
{
    return static_cast<std::size_t>(
        std::lower_bound(
            notes_.begin(), notes_.end(), tick,
            [](const NoteInstance& note, double value) {
                return double(note.startTick) < value;
            }) - notes_.begin());
}

std::size_t GLRenderer::upperBoundTick(double tick) const
{
    return static_cast<std::size_t>(
        std::upper_bound(
            notes_.begin(), notes_.end(), tick,
            [](double value, const NoteInstance& note) {
                return value < double(note.startTick);
            }) - notes_.begin());
}

void GLRenderer::rebuildColorMaps()
{
    globalChannelColor_.fill(-1);
    perTrackColor_.clear();

    int counter = 0;

    // Same global mapping algorithm as MPWGL2.
    for (const uint32_t trackMask : activeChannelMasks_) {
        for (int channel = 0; channel < 16; ++channel) {
            if ((trackMask & (1u << channel)) != 0 &&
                globalChannelColor_[channel] < 0) {
                globalChannelColor_[channel] =
                    static_cast<int8_t>(counter & 15);
                ++counter;
            }
        }
    }

    for (int channel = 0; channel < 16; ++channel) {
        if (globalChannelColor_[channel] < 0)
            globalChannelColor_[channel] =
                static_cast<int8_t>(channel);
    }

    counter = 0;
    for (const auto& note : notes_) {
        const uint32_t key =
            uint32_t(note.track) * 16u +
            uint32_t(note.channel & 15);

        if (perTrackColor_.find(key) == perTrackColor_.end()) {
            perTrackColor_[key] =
                static_cast<uint8_t>(counter & 15);
            ++counter;
        }
    }
}

uint8_t GLRenderer::colorIndexFor(
    uint16_t track,
    uint8_t channel) const
{
    channel &= 15;

    if (!perTrackColors_) {
        const int value =
            globalChannelColor_[channel];
        return static_cast<uint8_t>(
            value >= 0 ? value : channel);
    }

    const uint32_t key =
        uint32_t(track) * 16u +
        uint32_t(channel);

    const auto it = perTrackColor_.find(key);
    return it == perTrackColor_.end()
        ? channel
        : it->second;
}

void GLRenderer::initTextures(int width, int height)
{
    destroyTextures();

    textureWidth_ = std::max(1, width);
    textureHeight_ = std::max(1, height);

    glGenTextures(2, textures_);
    glGenFramebuffers(2, framebuffers_);

    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures_[i]);

        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            textureWidth_,
            textureHeight_,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr);

        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MIN_FILTER,
            GL_NEAREST);
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAG_FILTER,
            GL_NEAREST);
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_S,
            GL_CLAMP_TO_EDGE);
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_T,
            GL_CLAMP_TO_EDGE);

        glBindFramebuffer(
            GL_FRAMEBUFFER,
            framebuffers_[i]);

        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            textures_[i],
            0);

        glViewport(
            0, 0,
            textureWidth_,
            textureHeight_);

        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    frontIndex_ = 0;
    forceFullRedraw_ = true;
    lastRenderTick_ = -1.0;

    fullBuffer_.resize(
        static_cast<std::size_t>(
            textureWidth_) *
        static_cast<std::size_t>(
            textureHeight_) * 4u);
}

void GLRenderer::writeStrip(
    std::vector<uint8_t>& buffer,
    int columnStart,
    int columnCount,
    double currentTick)
{
    if (columnCount <= 0 ||
        textureHeight_ <= 0 ||
        notes_.empty()) {
        std::fill(
            buffer.begin(), buffer.end(), 0);
        return;
    }

    const std::size_t required =
        static_cast<std::size_t>(columnCount) *
        static_cast<std::size_t>(textureHeight_) * 4u;

    if (buffer.size() != required)
        buffer.resize(required);

    std::fill(buffer.begin(), buffer.end(), 0);

    const double tpc = ticksPerColumn();

    const double stripTickStart =
        currentTick - postTicks_ +
        double(columnStart) * tpc;

    const double stripTickEnd =
        currentTick - postTicks_ +
        double(columnStart + columnCount) * tpc;

    const std::size_t searchFrom =
        lowerBoundTick(
            stripTickStart -
            windowTicks_ -
            postTicks_);

    const std::size_t searchTo =
        upperBoundTick(stripTickEnd);

    const int rowHeight =
        std::max(
            1,
            static_cast<int>(
                std::floor(
                    double(textureHeight_) / 128.0)));

    for (std::size_t i = searchFrom;
         i < searchTo && i < notes_.size();
         ++i) {
        const NoteInstance& note = notes_[i];

        const double startTick =
            double(note.startTick);
        const double endTick =
            double(note.endTick);

        if (endTick < stripTickStart ||
            startTick > stripTickEnd)
            continue;

        const bool active =
            startTick <= currentTick &&
            endTick >= currentTick;

        const int col0Abs =
            static_cast<int>(std::lround(
                (startTick - currentTick + postTicks_) /
                tpc));

        const int col1Abs =
            static_cast<int>(std::lround(
                (endTick - currentTick + postTicks_) /
                tpc));

        const int c0 =
            std::max(0, col0Abs - columnStart);

        const int c1 =
            std::min(
                columnCount - 1,
                col1Abs - columnStart);

        if (c0 > c1)
            continue;

        const int rowCenter =
            static_cast<int>(std::lround(
                (double(note.pitch) / 127.0) *
                double(textureHeight_ - 1)));

        const int rowTop =
            std::max(
                0,
                rowCenter - rowHeight / 2);

        const int rowBottom =
            std::min(
                textureHeight_ - 1,
                rowTop + rowHeight - 1);

        const uint8_t colorIndex =
            colorIndexFor(
                note.track,
                note.channel);

        int r = channelColors_[colorIndex][0];
        int g = channelColors_[colorIndex][1];
        int b = channelColors_[colorIndex][2];

        if (active) {
            r = std::min(
                255,
                static_cast<int>(
                    std::lround(r * 1.55 + 30.0)));
            g = std::min(
                255,
                static_cast<int>(
                    std::lround(g * 1.55 + 30.0)));
            b = std::min(
                255,
                static_cast<int>(
                    std::lround(b * 1.55 + 30.0)));
        }

        const int brightR =
            std::min(255, r + 50);
        const int brightG =
            std::min(255, g + 50);
        const int brightB =
            std::min(255, b + 50);

        for (int row = rowTop;
             row <= rowBottom;
             ++row) {
            for (int column = c0;
                 column <= c1;
                 ++column) {
                const std::size_t base =
                    (static_cast<std::size_t>(row) *
                         static_cast<std::size_t>(
                             columnCount) +
                     static_cast<std::size_t>(
                         column)) * 4u;

                if (column == c0) {
                    buffer[base] =
                        static_cast<uint8_t>(brightR);
                    buffer[base + 1] =
                        static_cast<uint8_t>(brightG);
                    buffer[base + 2] =
                        static_cast<uint8_t>(brightB);
                } else {
                    buffer[base] =
                        static_cast<uint8_t>(r);
                    buffer[base + 1] =
                        static_cast<uint8_t>(g);
                    buffer[base + 2] =
                        static_cast<uint8_t>(b);
                }

                buffer[base + 3] = 255;
            }
        }
    }
}

void GLRenderer::renderFullTexture(
    double currentTick)
{
    writeStrip(
        fullBuffer_,
        0,
        textureWidth_,
        currentTick);

    glBindTexture(
        GL_TEXTURE_2D,
        textures_[frontIndex_]);

    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0, 0,
        textureWidth_,
        textureHeight_,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        fullBuffer_.data());

    glBindTexture(GL_TEXTURE_2D, 0);

    forceFullRedraw_ = false;
}

void GLRenderer::scrollAndAdvance(
    double currentTick,
    int deltaColumns)
{
    if (deltaColumns <= 0)
        return;

    if (deltaColumns >= textureWidth_) {
        renderFullTexture(currentTick);
        return;
    }

    const int backIndex =
        frontIndex_ == 0 ? 1 : 0;

    glBindFramebuffer(
        GL_FRAMEBUFFER,
        framebuffers_[backIndex]);

    glViewport(
        0, 0,
        textureWidth_,
        textureHeight_);

    glUseProgram(blitProgram_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(
        GL_TEXTURE_2D,
        textures_[frontIndex_]);

    glUniform1i(
        blitSourceUniform_, 0);

    glUniform1f(
        blitOffsetUniform_,
        float(deltaColumns) /
        float(textureWidth_));

    glBindVertexArray(emptyVao_);
    glDrawArrays(
        GL_TRIANGLE_STRIP,
        0, 4);
    glBindVertexArray(0);

    frontIndex_ = backIndex;

    writeStrip(
        stripBuffer_,
        textureWidth_ - deltaColumns,
        deltaColumns,
        currentTick);

    glBindTexture(
        GL_TEXTURE_2D,
        textures_[frontIndex_]);

    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        textureWidth_ - deltaColumns,
        0,
        deltaColumns,
        textureHeight_,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        stripBuffer_.data());

    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLRenderer::drawFrontTexture(
    GLint targetFramebuffer)
{
    glBindFramebuffer(
        GL_FRAMEBUFFER,
        static_cast<GLuint>(
            targetFramebuffer));

    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(scrollProgram_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(
        GL_TEXTURE_2D,
        textures_[frontIndex_]);

    glUniform1i(
        scrollTexUniform_, 0);

    constexpr float PlayheadFraction = 0.18f;

    const float playheadWidth =
        2.0f /
        float(std::max(1, textureWidth_));

    glUniform1f(
        playheadXUniform_,
        PlayheadFraction);

    glUniform1f(
        playheadWidthUniform_,
        playheadWidth);

    glBindVertexArray(emptyVao_);
    glDrawArrays(
        GL_TRIANGLE_STRIP,
        0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLRenderer::renderRoll()
{
    if (!initialized_ && !initialize())
        return;

    GLint targetFramebuffer = 0;
    glGetIntegerv(
        GL_FRAMEBUFFER_BINDING,
        &targetFramebuffer);

    if (textureWidth_ != width_ ||
        textureHeight_ != height_) {
        initTextures(width_, height_);
    }

    if (notes_.empty()) {
        glBindFramebuffer(
            GL_FRAMEBUFFER,
            static_cast<GLuint>(
                targetFramebuffer));
        glViewport(0, 0, width_, height_);
        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    const double currentTick =
        secToTick(currentTime_);

    if (forceFullRedraw_ ||
        lastRenderTick_ < 0.0) {
        renderFullTexture(currentTick);
        lastRenderTick_ = currentTick;
    } else {
        const double tpc =
            ticksPerColumn();

        const int delta =
            static_cast<int>(std::lround(
                (currentTick -
                 lastRenderTick_) / tpc));

        if (delta < 0) {
            renderFullTexture(currentTick);
        } else if (delta > 0) {
            scrollAndAdvance(
                currentTick,
                delta);
        }

        lastRenderTick_ = currentTick;
    }

    drawFrontTexture(targetFramebuffer);
}

} // namespace wasmidi
