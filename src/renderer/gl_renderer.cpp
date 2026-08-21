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
    std::size_t(1) << 18; // 262,144 visible notes ~= 3 MiB CPU + 3 MiB GPU

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

    activeNoteId_.fill(-1);
}

GLRenderer::~GLRenderer()
{
    destroy();
}

bool GLRenderer::createProgram()
{
    static const char* vertex = R"GLSL(#version 300 es
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
    int endTick = aEndTick > 0
        ? aEndTick
        : int(uViewEnd);

    uint vertexId = uint(gl_VertexID);
    float useEnd = float(vertexId & 1u);
    float useTop = float((vertexId >> 1u) & 1u);

    float rangeTicks = max(1.0, uViewEnd - uViewStart);

    float startX =
        (float(aStartTick) - uViewStart) /
        rangeTicks * 2.0 - 1.0;

    float endX =
        (float(endTick) - uViewStart) /
        rangeTicks * 2.0 - 1.0;

    float x = mix(startX, endX, useEnd);

    uint pitch = (aPackedData >> 8u) & 0xffu;
    uint colorIndex = (aPackedData >> 16u) & 0x0fu;

    float yBottom =
        -1.0 +
        float(pitch) / 128.0 * 2.0;

    float yTop =
        -1.0 +
        float(pitch + 1u) / 128.0 * 2.0;

    float y = mix(yBottom, yTop, useTop);

    vColor = uPalette[int(colorIndex)];

    vActive =
        (uCurrentTick >= float(aStartTick) &&
         uCurrentTick <= float(endTick))
            ? 1.0
            : 0.0;

    gl_Position = vec4(x, y, 0.0, 1.0);
}
)GLSL";

    static const char* fragment = R"GLSL(#version 300 es
precision mediump float;

flat in vec3 vColor;
flat in float vActive;

out vec4 fragColor;

void main()
{
    vec3 color = vColor;

    if (vActive > 0.5)
        color = min(color * 1.55 + vec3(0.12), vec3(1.0));

    fragColor = vec4(color, 1.0);
}
)GLSL";

    program_ = linkProgram(vertex, fragment);

    if (!program_)
        return false;

    viewStartUniform_ =
        glGetUniformLocation(program_, "uViewStart");

    viewEndUniform_ =
        glGetUniformLocation(program_, "uViewEnd");

    currentTickUniform_ =
        glGetUniformLocation(program_, "uCurrentTick");

    paletteUniform_ =
        glGetUniformLocation(program_, "uPalette[0]");

    return true;
}

bool GLRenderer::initialize()
{
    if (initialized_)
        return true;

    if (!createProgram())
        return false;

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &instanceVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    allocateRing(InitialRingCapacity);

    initialized_ = true;
    return true;
}

void GLRenderer::destroy()
{
    if (instanceVbo_)
        glDeleteBuffers(1, &instanceVbo_);

    if (vao_)
        glDeleteVertexArrays(1, &vao_);

    if (program_)
        glDeleteProgram(program_);

    instanceVbo_ = 0;
    vao_ = 0;
    program_ = 0;

    ring_.clear();
    ringCapacity_ = 0;
    ringMask_ = 0;

    initialized_ = false;
}

void GLRenderer::resize(int width, int height)
{
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void GLRenderer::setDocument(const MidiDocument* document)
{
    // MainWindow owns one stable MidiDocument object and move-assigns new
    // parses into it, so pointer equality does NOT mean the MIDI is unchanged.
    document_ = document;
    resetSweep();
}

void GLRenderer::setCurrentTime(float seconds)
{
    currentTime_ = std::max(0.0f, seconds);
}

void GLRenderer::setNoteSpeed(float secondsPerWindow)
{
    const float value =
        std::clamp(secondsPerWindow, 0.1f, 60.0f);

    if (std::abs(noteSpeed_ - value) < 0.00001f)
        return;

    noteSpeed_ = value;
    forceSweepReset_ = true;
}

void GLRenderer::setPostBuffer(float seconds)
{
    const float value =
        std::clamp(seconds, 0.0f, 10.0f);

    if (std::abs(postBuffer_ - value) < 0.00001f)
        return;

    postBuffer_ = value;
    forceSweepReset_ = true;
}

void GLRenderer::setPerTrackColors(bool enabled)
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
    if (channel >= channelColors_.size())
        return;

    const std::array<uint8_t, 4> value =
        {r, g, b, 255};

    if (channelColors_[channel] == value)
        return;

    channelColors_[channel] = value;
    paletteDirty_ = true;
}

void GLRenderer::allocateRing(std::size_t capacity)
{
    // Capacity must be a power of two.
    std::size_t rounded = 1;

    while (rounded < capacity)
        rounded <<= 1;

    const std::size_t oldCapacity =
        ringCapacity_;

    const std::size_t oldMask =
        ringMask_;

    std::vector<RenderNote> oldRing;
    oldRing.swap(ring_);

    ringCapacity_ = rounded;
    ringMask_ = ringCapacity_ - 1;
    ring_.resize(ringCapacity_);

    if (!oldRing.empty() && oldCapacity != 0) {
        for (int64_t id = tail_;
             id < head_;
             ++id) {
            ring_[static_cast<std::size_t>(id) & ringMask_] =
                oldRing[
                    static_cast<std::size_t>(id) & oldMask];
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            ringCapacity_ * sizeof(RenderNote)),
        nullptr,
        GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (head_ > tail_)
        uploadAbsoluteRange(tail_, head_);

    // A resize remaps every absolute ID to a new physical slot and the full
    // active range above already contains all EndTick edits made so far.
    dirtyEndIndices_.clear();
}

void GLRenderer::ensureRingSpace()
{
    if (ringCapacity_ == 0) {
        allocateRing(InitialRingCapacity);
        return;
    }

    if (head_ - tail_ <
        static_cast<int64_t>(ringCapacity_ - 1)) {
        return;
    }

    allocateRing(ringCapacity_ * 2);
}

void GLRenderer::resetSweep()
{
    head_ = 0;
    tail_ = 0;

    activeCount_.fill(0);
    activeColor_.fill(0);
    activeNoteId_.fill(-1);

    dirtyEndIndices_.clear();

    lastSweepEnd_ = -1;
    lastViewSpan_ = 0;
    forceSweepReset_ = true;
}

uint8_t GLRenderer::eventColor(
    const CompactEvent& event) const
{
    if (!document_)
        return 0;

    return document_->colorIndex(
        event,
        perTrackColors_);
}

void GLRenderer::appendNote(
    uint32_t tick,
    uint8_t pitch,
    uint8_t velocity,
    uint8_t color)
{
    ensureRingSpace();

    const std::size_t physical =
        static_cast<std::size_t>(head_) &
        ringMask_;

    ring_[physical] = {
        static_cast<int32_t>(
            std::min<uint32_t>(
                tick,
                uint32_t(
                    std::numeric_limits<int32_t>::max()))),
        0,
        uint32_t(velocity) |
        (uint32_t(pitch) << 8) |
        (uint32_t(color & 0x0f) << 16)
    };

    ++head_;
}

void GLRenderer::closeActiveNote(
    std::size_t stateIndex,
    uint32_t tick)
{
    const int64_t id =
        activeNoteId_[stateIndex];

    if (id < tail_ || id >= head_)
        return;

    const std::size_t physical =
        static_cast<std::size_t>(id) &
        ringMask_;

    ring_[physical].endTick =
        static_cast<int32_t>(
            std::min<uint32_t>(
                tick,
                uint32_t(
                    std::numeric_limits<int32_t>::max())));

    dirtyEndIndices_.push_back(
        static_cast<uint32_t>(physical));
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
        std::size_t(channel) * 128u +
        std::size_t(pitch);

    const uint8_t color =
        eventColor(event);

    if (command == 0x90 &&
        event.data2 != 0) {
        uint32_t& count =
            activeCount_[stateIndex];

        // SharpMIDI-style color ownership: when a different track/color begins
        // the same channel+pitch while it is held, close the old visual segment
        // and start a new colored segment instead of creating ambiguous layers.
        if (count != 0 &&
            activeColor_[stateIndex] != color) {
            closeActiveNote(
                stateIndex,
                tick);

            count = 0;
        }

        if (count == 0) {
            activeNoteId_[stateIndex] =
                head_;

            appendNote(
                tick,
                pitch,
                event.data2,
                color);

            activeColor_[stateIndex] =
                color;
        }

        ++count;
        return;
    }

    uint32_t& count =
        activeCount_[stateIndex];

    if (count == 0)
        return;

    // In per-track color mode an older track's NoteOff must not terminate a
    // newer colored segment that reused the same channel+pitch.
    if (activeColor_[stateIndex] != color)
        return;

    --count;

    if (count == 0) {
        closeActiveNote(
            stateIndex,
            tick);

        activeNoteId_[stateIndex] = -1;
    }
}

void GLRenderer::uploadAbsoluteRange(
    int64_t begin,
    int64_t end)
{
    if (end <= begin ||
        ringCapacity_ == 0)
        return;

    glBindBuffer(
        GL_ARRAY_BUFFER,
        instanceVbo_);

    int64_t cursor = begin;

    while (cursor < end) {
        const std::size_t physical =
            static_cast<std::size_t>(cursor) &
            ringMask_;

        const std::size_t available =
            ringCapacity_ - physical;

        const std::size_t count =
            static_cast<std::size_t>(
                std::min<int64_t>(
                    end - cursor,
                    static_cast<int64_t>(available)));

        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(
                physical *
                sizeof(RenderNote)),
            static_cast<GLsizeiptr>(
                count *
                sizeof(RenderNote)),
            ring_.data() + physical);

        cursor +=
            static_cast<int64_t>(count);
    }

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);
}

void GLRenderer::flushEndTickUpdates()
{
    if (dirtyEndIndices_.empty())
        return;

    std::sort(
        dirtyEndIndices_.begin(),
        dirtyEndIndices_.end());

    dirtyEndIndices_.erase(
        std::unique(
            dirtyEndIndices_.begin(),
            dirtyEndIndices_.end()),
        dirtyEndIndices_.end());

    glBindBuffer(
        GL_ARRAY_BUFFER,
        instanceVbo_);

    std::size_t i = 0;

    while (i < dirtyEndIndices_.size()) {
        std::size_t first =
            dirtyEndIndices_[i];

        std::size_t last = first;
        ++i;

        // Small gaps are cheaper to upload than issuing another WebGL call.
        while (i < dirtyEndIndices_.size() &&
               dirtyEndIndices_[i] <= last + 16) {
            last = dirtyEndIndices_[i];
            ++i;
        }

        const std::size_t count =
            last - first + 1;

        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(
                first *
                sizeof(RenderNote)),
            static_cast<GLsizeiptr>(
                count *
                sizeof(RenderNote)),
            ring_.data() + first);
    }

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);

    dirtyEndIndices_.clear();
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

    const std::size_t firstGroup =
        document_->lowerBoundGroup(fromTick);

    const std::size_t lastGroup =
        document_->upperBoundGroup(toTick);

    const int64_t appendBegin =
        head_;

    for (std::size_t groupIndex = firstGroup;
         groupIndex < lastGroup;
         ++groupIndex) {
        const TickGroup& group =
            document_->tickGroups[groupIndex];

        const std::size_t begin =
            group.eventOffset;

        const std::size_t end =
            begin + group.eventCount;

        for (std::size_t eventIndex = begin;
             eventIndex < end;
             ++eventIndex) {
            processEvent(
                document_->events[eventIndex],
                group.tick);
        }
    }

    // New notes are contiguous in absolute ring ID space and therefore need
    // at most two WebGL uploads when the physical ring wraps.
    uploadAbsoluteRange(
        appendBegin,
        head_);

    // NoteOffs modify older instances. Batch/coalesce those edits.
    flushEndTickUpdates();
}

void GLRenderer::advanceTail(uint32_t viewStart)
{
    if (ringCapacity_ == 0)
        return;

    const int64_t safeTail =
        head_ -
        static_cast<int64_t>(ringCapacity_);

    if (tail_ < safeTail)
        tail_ = safeTail;

    while (tail_ < head_) {
        const RenderNote& note =
            ring_[
                static_cast<std::size_t>(tail_) &
                ringMask_];

        const bool open =
            note.endTick == 0;

        if (!open &&
            note.endTick <
                static_cast<int32_t>(viewStart)) {
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
        currentTick = viewStart =
            viewEnd = sweepEnd = 0;
        return;
    }

    currentTick =
        static_cast<uint32_t>(
            std::clamp<double>(
                std::floor(
                    document_->secondsToTick(
                        currentTime_)),
                0.0,
                document_->maxTick));

    // Keep the current visual time-span semantics, but derive tick width at
    // the *current* tempo instead of assuming tempo at t=0.
    const double rightSeconds =
        std::min<double>(
            document_->durationSeconds,
            double(currentTime_) +
            double(noteSpeed_));

    uint32_t rightTick =
        static_cast<uint32_t>(
            std::clamp<double>(
                std::ceil(
                    document_->secondsToTick(
                        rightSeconds)),
                currentTick + 1.0,
                double(
                    std::max<uint32_t>(
                        currentTick + 1,
                        document_->maxTick))));

    // Auto mode historically keeps the impact line near 18% of the roll.
    // If the user explicitly selects Post Buffer, use that exact seconds span.
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
                    document_->secondsToTick(
                        leftSeconds))));

    viewEnd =
        std::max(
            viewStart + 1,
            rightTick);

    const uint32_t span =
        viewEnd - viewStart;

    const uint32_t lookahead =
        std::max<uint32_t>(
            1,
            std::min<uint32_t>(
                span / 2,
                uint32_t(
                    std::max<uint16_t>(
                        1,
                        document_->ticksPerBeat)) *
                    4u));

    sweepEnd =
        std::min(
            document_->maxTick,
            viewEnd + lookahead);
}

void GLRenderer::setInstanceBase(
    std::size_t physicalIndex)
{
    const std::size_t base =
        physicalIndex *
        sizeof(RenderNote);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        instanceVbo_);

    glVertexAttribIPointer(
        0,
        1,
        GL_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            base +
            offsetof(RenderNote, startTick)));

    glVertexAttribIPointer(
        1,
        1,
        GL_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            base +
            offsetof(RenderNote, endTick)));

    glVertexAttribIPointer(
        2,
        1,
        GL_UNSIGNED_INT,
        sizeof(RenderNote),
        reinterpret_cast<void*>(
            base +
            offsetof(RenderNote, packedData)));
}

void GLRenderer::renderRoll()
{
    if (!initialized_ &&
        !initialize()) {
        return;
    }

    glViewport(
        0,
        0,
        width_,
        height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glClearColor(
        0.0f,
        0.0f,
        0.0f,
        0.0f);

    glClear(GL_COLOR_BUFFER_BIT);

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
            viewEnd - viewStart);

    const bool incremental =
        !forceSweepReset_ &&
        lastSweepEnd_ >= 0 &&
        sweepEnd >=
            static_cast<uint32_t>(lastSweepEnd_) &&
        sweepEnd -
            static_cast<uint32_t>(lastSweepEnd_) <
            std::max<uint32_t>(
                1,
                viewSpan);

    if (!incremental) {
        head_ = 0;
        tail_ = 0;

        activeCount_.fill(0);
        activeColor_.fill(0);
        activeNoteId_.fill(-1);
        dirtyEndIndices_.clear();

        // Same philosophy as SharpMIDI's non-incremental sweep: reconstruct
        // enough history to preserve sustained notes near the visible window
        // without replaying an entire 100M-event MIDI on every seek.
        const uint32_t lookbehind =
            std::min(
                viewStart,
                std::max<uint32_t>(
                    viewSpan,
                    uint32_t(
                        std::max<uint16_t>(
                            1,
                            document_->ticksPerBeat)) *
                        16u));

        sweepRange(
            viewStart - lookbehind,
            sweepEnd);
    } else if (
        sweepEnd >
        static_cast<uint32_t>(lastSweepEnd_)) {
        sweepRange(
            static_cast<uint32_t>(
                lastSweepEnd_) + 1,
            sweepEnd);
    }

    lastSweepEnd_ =
        static_cast<int64_t>(
            sweepEnd);

    lastViewSpan_ =
        viewSpan;

    forceSweepReset_ = false;

    advanceTail(viewStart);

    glUseProgram(program_);

    glUniform1f(
        viewStartUniform_,
        static_cast<float>(viewStart));

    glUniform1f(
        viewEndUniform_,
        static_cast<float>(viewEnd));

    glUniform1f(
        currentTickUniform_,
        static_cast<float>(currentTick));

    if (paletteDirty_) {
        GLfloat palette[16 * 3];

        for (int i = 0; i < 16; ++i) {
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

    const int64_t visible64 =
        std::max<int64_t>(
            0,
            head_ - tail_);

    if (visible64 > 0 &&
        ringCapacity_ != 0) {
        const std::size_t visible =
            static_cast<std::size_t>(
                std::min<int64_t>(
                    visible64,
                    static_cast<int64_t>(
                        ringCapacity_)));

        const std::size_t start =
            static_cast<std::size_t>(
                tail_) &
            ringMask_;

        const std::size_t first =
            std::min(
                visible,
                ringCapacity_ - start);

        const std::size_t second =
            visible - first;

        glBindVertexArray(vao_);

        if (first != 0) {
            setInstanceBase(start);

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

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glUseProgram(0);

    // Draw the same impact/playhead line without another shader or QML item.
    const float playheadFraction =
        std::clamp(
            float(currentTick - viewStart) /
            float(std::max<uint32_t>(
                1,
                viewEnd - viewStart)),
            0.0f,
            1.0f);

    const int lineX =
        std::clamp(
            static_cast<int>(
                std::lround(
                    playheadFraction *
                    float(width_))),
            0,
            std::max(0, width_ - 1));

    glEnable(GL_SCISSOR_TEST);

    glScissor(
        lineX,
        0,
        std::max(1, width_ / 900),
        height_);

    glClearColor(
        0.63f,
        0.54f,
        0.98f,
        0.92f);

    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_SCISSOR_TEST);
}

} // namespace wasmidi
