#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace {

class PianoRollRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    PianoRollRenderer(
        qreal devicePixelRatio,
        std::atomic<int>* fps)
        : devicePixelRatio_(
              std::max<qreal>(
                  1.0,
                  devicePixelRatio)),
          fps_(fps)
    {
    }

    QOpenGLFramebufferObject*
    createFramebufferObject(
        const QSize& physicalSize) override
    {
        /*
         * MPWGL2.html deliberately sets rollCanvas.width/height to the CSS
         * bounding rectangle, without multiplying by devicePixelRatio.
         *
         * Qt hands us DPR-scaled physicalSize. Divide it back to CSS pixels.
         * This avoids rendering/scrolling/compositing 2.25x-4x as many pixels
         * on 150%-200% Windows scaling while keeping the same raster density
         * as the original player.
         */
        QSize cssSize(
            std::max(
                64,
                qRound(
                    physicalSize.width() /
                    devicePixelRatio_)),
            std::max(
                64,
                qRound(
                    physicalSize.height() /
                    devicePixelRatio_)));

        return new QOpenGLFramebufferObject(
            cssSize);
    }

    void synchronize(
        QQuickFramebufferObject* item) override
    {
        auto* roll =
            static_cast<PianoRoll*>(item);

        auto* controller =
            qobject_cast<MainWindow*>(
                roll->controller());

        if (!controller)
            return;

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
                static_cast<uint8_t>(
                    colors[i].red()),
                static_cast<uint8_t>(
                    colors[i].green()),
                static_cast<uint8_t>(
                    colors[i].blue()));
        }

        syncedTimeSeconds_ =
            controller->currentTime();

        syncedPlaying_ =
            controller->isPlaying();

        syncWallClock_ =
            Clock::now();

        if (revision_ !=
            controller->documentRevision()) {
            revision_ =
                controller->documentRevision();

            const auto& document =
                controller->document();

            std::vector<wasmidi::NoteInstance>
                notes;

            notes.reserve(
                document.notes.size());

            for (const auto& note :
                 document.notes) {
                notes.push_back({
                    note.startTick,
                    note.endTick,
                    note.pitch,
                    note.channel,
                    note.velocity,
                    note.track
                });
            }

            std::vector<wasmidi::TempoPoint>
                tempo;

            tempo.reserve(
                document.tempoMap.size());

            for (const auto& point :
                 document.tempoMap) {
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
        if (auto* fbo =
                framebufferObject()) {
            const QSize size =
                fbo->size();

            renderer_.resize(
                size.width(),
                size.height());
        }

        float renderTime =
            syncedTimeSeconds_;

        if (syncedPlaying_) {
            const auto now =
                Clock::now();

            const std::chrono::duration<float>
                elapsed =
                    now - syncWallClock_;

            renderTime +=
                elapsed.count();
        }

        renderer_.setCurrentTime(
            renderTime);

        renderer_.renderRoll();

        QQuickOpenGLUtils::
            resetOpenGLState();

        updateFps();

        // Single continuous scheduling source for the roll.
        if (syncedPlaying_)
            update();
    }

private:
    using Clock =
        std::chrono::steady_clock;

    void updateFps()
    {
        ++fpsFrames_;

        const auto now =
            Clock::now();

        const std::chrono::duration<double>
            elapsed =
                now - fpsWindowStart_;

        if (elapsed.count() >= 0.5) {
            const int fps =
                static_cast<int>(
                    std::lround(
                        double(fpsFrames_) /
                        elapsed.count()));

            if (fps_)
                fps_->store(
                    fps,
                    std::memory_order_relaxed);

            fpsFrames_ = 0;
            fpsWindowStart_ = now;
        }
    }

    qreal devicePixelRatio_ = 1.0;
    std::atomic<int>* fps_ = nullptr;

    wasmidi::GLRenderer renderer_;

    quint64 revision_ =
        std::numeric_limits<
            quint64>::max();

    float syncedTimeSeconds_ = 0.0f;
    bool syncedPlaying_ = false;

    Clock::time_point syncWallClock_ =
        Clock::now();

    Clock::time_point fpsWindowStart_ =
        Clock::now();

    int fpsFrames_ = 0;
};

} // namespace

PianoRoll::PianoRoll(
    QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
    setTextureFollowsItemSize(true);
}

void PianoRoll::setController(
    QObject* controller)
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
        auto requestSync =
            [this]() {
                update();
            };

        connect(
            player,
            &MainWindow::
                documentRevisionChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::
                noteSpeedChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::
                postBufferChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::
                perTrackColorsChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::
                channelColorsChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::
                playingChanged,
            this,
            requestSync);

        // Synchronize paused seeks / large discontinuities only.
        auto lastTime =
            std::make_shared<float>(
                player->currentTime());

        connect(
            player,
            &MainWindow::
                currentTimeChanged,
            this,
            [this,
             player,
             lastTime]() {
                const float now =
                    player->currentTime();

                const float delta =
                    std::fabs(
                        now - *lastTime);

                *lastTime = now;

                if (!player->isPlaying() ||
                    delta > 0.10f) {
                    update();
                }
            });
    }

    emit controllerChanged();
    update();
}

QQuickFramebufferObject::Renderer*
PianoRoll::createRenderer() const
{
    const qreal dpr =
        window()
            ? window()->
                devicePixelRatio()
            : 1.0;

    return new PianoRollRenderer(
        dpr,
        &renderFps_);
}
