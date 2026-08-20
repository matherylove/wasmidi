#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <chrono>
#include <limits>
#include <vector>

namespace {

class PianoRollRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    QOpenGLFramebufferObject*
    createFramebufferObject(const QSize& size) override
    {
        return new QOpenGLFramebufferObject(size);
    }

    void synchronize(QQuickFramebufferObject* item) override
    {
        auto* roll = static_cast<PianoRoll*>(item);
        auto* controller =
            qobject_cast<MainWindow*>(roll->controller());

        if (!controller)
            return;

        renderer_.resize(
            static_cast<int>(roll->width()),
            static_cast<int>(roll->height()));

        renderer_.setNoteSpeed(
            controller->noteSpeed());

        renderer_.setPostBuffer(
            controller->postBuffer());

        renderer_.setPerTrackColors(
            controller->perTrackColors());

        const auto& colors =
            controller->channelColors();

        for (int i = 0;
             i < colors.size() && i < 16;
             ++i) {
            renderer_.setChannelColor(
                static_cast<uint8_t>(i),
                static_cast<uint8_t>(colors[i].red()),
                static_cast<uint8_t>(colors[i].green()),
                static_cast<uint8_t>(colors[i].blue()));
        }

        /*
         * Keep a render-thread playback anchor.
         *
         * Qt/WASM was observed to occasionally render the same synchronized
         * controller time repeatedly until a window expose/resize event. The
         * old MPWGL2 renderer does not depend on DOM/state synchronization per
         * frame: requestAnimationFrame extrapolates time from a monotonic
         * clock. Do the same here.
         */
        syncedTimeSeconds_ = controller->currentTime();
        syncedPlaying_ = controller->isPlaying();
        syncWallClock_ = Clock::now();

        if (revision_ != controller->documentRevision()) {
            revision_ = controller->documentRevision();

            const auto& document =
                controller->document();

            std::vector<wasmidi::NoteInstance> notes;
            notes.reserve(document.notes.size());

            for (const auto& note : document.notes) {
                notes.push_back({
                    note.startTick,
                    note.endTick,
                    note.pitch,
                    note.channel,
                    note.velocity,
                    note.track
                });
            }

            std::vector<wasmidi::TempoPoint> tempo;
            tempo.reserve(document.tempoMap.size());

            for (const auto& point : document.tempoMap) {
                tempo.push_back({
                    point.tick,
                    point.microsecondsPerBeat
                });
            }

            renderer_.setTempoMap(
                tempo,
                document.ticksPerBeat);

            renderer_.setActiveChannelMasks(
                document.activeChannelMasks);

            renderer_.setNotes(notes);
        }
    }

    void render() override
    {
        float renderTime = syncedTimeSeconds_;

        if (syncedPlaying_) {
            const auto now = Clock::now();
            const std::chrono::duration<float> elapsed =
                now - syncWallClock_;
            renderTime += elapsed.count();
        }

        renderer_.setCurrentTime(renderTime);
        renderer_.renderRoll();

        /*
         * QQuickFramebufferObject shares the OpenGL context with Qt Quick.
         * Qt explicitly recommends resetting custom GL state before returning.
         * This is particularly important here because the MPWGL2 renderer
         * binds private FBOs, textures, programs and VAOs every frame.
         */
        QQuickOpenGLUtils::resetOpenGLState();

        /*
         * Match MPWGL2's requestAnimationFrame loop. The renderer advances its
         * monotonic playback clock even if Qt/WASM delays a GUI-side
         * synchronize() call.
         */
        if (syncedPlaying_)
            update();
    }

private:
    using Clock = std::chrono::steady_clock;

    wasmidi::GLRenderer renderer_;

    quint64 revision_ =
        std::numeric_limits<quint64>::max();

    float syncedTimeSeconds_ = 0.0f;
    bool syncedPlaying_ = false;
    Clock::time_point syncWallClock_ = Clock::now();
};

} // namespace

PianoRoll::PianoRoll(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
}

void PianoRoll::setController(QObject* controller)
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
            /*
             * Mark the FBO item dirty AND explicitly request a window frame.
             * The latter removes any dependency on platform-specific Qt/WASM
             * scene-graph update coalescing.
             */
            update();
            if (window())
                window()->update();
        };

        connect(
            player, &MainWindow::currentTimeChanged,
            this, requestFrame);

        connect(
            player, &MainWindow::documentRevisionChanged,
            this, requestFrame);

        connect(
            player, &MainWindow::noteSpeedChanged,
            this, requestFrame);

        connect(
            player, &MainWindow::postBufferChanged,
            this, requestFrame);

        connect(
            player, &MainWindow::perTrackColorsChanged,
            this, requestFrame);

        connect(
            player, &MainWindow::channelColorsChanged,
            this, requestFrame);

        connect(
            player, &MainWindow::playingChanged,
            this, requestFrame);
    }

    emit controllerChanged();

    update();
    if (window())
        window()->update();
}

QQuickFramebufferObject::Renderer*
PianoRoll::createRenderer() const
{
    return new PianoRollRenderer();
}
