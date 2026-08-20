#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace {

class PianoRollRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    explicit PianoRollRenderer(qreal dpr)
        : dpr_(std::max<qreal>(1.0, dpr)) {}

    QOpenGLFramebufferObject*
    createFramebufferObject(const QSize& size) override
    {
        const QSize cssSize(
            std::max(64, qRound(size.width() / dpr_)),
            std::max(64, qRound(size.height() / dpr_)));
        /*
         * IMPORTANT: Qt documents that `size` already includes the device
         * pixel ratio. Returning this exact size gives us the physical-pixel
         * WebGL target (for example Windows 125/150% scaling).
         */
        return new QOpenGLFramebufferObject(cssSize);
    }

    void synchronize(QQuickFramebufferObject* item) override
    {
        auto* roll = static_cast<PianoRoll*>(item);
        auto* controller =
            qobject_cast<MainWindow*>(roll->controller());

        if (!controller)
            return;

        /*
         * Do NOT call renderer_.resize(roll->width(), roll->height()) here.
         * Those are logical QML pixels. The QQuickFramebufferObject itself is
         * DPR-scaled, so doing that made the private MPWGL2 textures lower
         * resolution than the actual FBO and stretched them on presentation.
         */

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

            renderer_.setNotesView(&document.notes);
        }
    }

    void render() override
    {
        /*
         * framebufferObject()->size() is the authoritative physical-pixel
         * resolution. Qt already multiplied it by devicePixelRatio.
         */
        if (auto* fbo = framebufferObject()) {
            const QSize pixelSize = fbo->size();

            renderer_.resize(
                pixelSize.width(),
                pixelSize.height());
        }

        float renderTime =
            syncedTimeSeconds_;

        if (syncedPlaying_) {
            const auto now = Clock::now();

            const std::chrono::duration<float>
                elapsed =
                    now - syncWallClock_;

            renderTime += elapsed.count();
        }

        renderer_.setCurrentTime(renderTime);
        renderer_.renderRoll();

        QQuickOpenGLUtils::resetOpenGLState();

        /*
         * This is the ONE continuous scheduling path for the piano roll.
         * GUI currentTimeChanged no longer requests another window frame every
         * 16 ms at the same time.
         */
        if (syncedPlaying_)
            update();
    }

private:
    using Clock =
        std::chrono::steady_clock;

    qreal dpr_ = 1.0;
    wasmidi::GLRenderer renderer_;

    quint64 revision_ =
        std::numeric_limits<quint64>::max();

    float syncedTimeSeconds_ = 0.0f;
    bool syncedPlaying_ = false;

    Clock::time_point syncWallClock_ =
        Clock::now();
};

} // namespace

PianoRoll::PianoRoll(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
    setTextureFollowsItemSize(true);
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

        auto requestSync = [this]() {
            update();
        };

        /*
         * Real state changes need a synchronize() pass.
         * Normal playback frames are generated exclusively by
         * PianoRollRenderer::update().
         */
        connect(
            player,
            &MainWindow::documentRevisionChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::noteSpeedChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::postBufferChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::perTrackColorsChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::channelColorsChanged,
            this,
            requestSync);

        connect(
            player,
            &MainWindow::playingChanged,
            this,
            requestSync);

        /*
         * currentTimeChanged is emitted by the 16 ms UI clock, but we must not
         * render from every emission as well. Only synchronize a paused seek
         * or an obvious discontinuity/jump while playing.
         */
        auto lastControllerTime =
            std::make_shared<float>(
                player->currentTime());

        connect(
            player,
            &MainWindow::currentTimeChanged,
            this,
            [this, player, lastControllerTime]() {
                const float now =
                    player->currentTime();

                const float delta =
                    std::fabs(
                        now - *lastControllerTime);

                *lastControllerTime = now;

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
    const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
    return new PianoRollRenderer(dpr);
}
