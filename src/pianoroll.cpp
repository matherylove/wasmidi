#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

namespace {

class PianoRollRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    PianoRollRenderer(
        qreal dpr,
        std::atomic<int>* fps)
        : dpr_(
            std::max<qreal>(
                1.0,
                dpr)),
          fps_(fps)
    {
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

        // Color-only FBO is the proven Qt/WASM composition path used by the
        // earlier working renderer. Depth/stencil must not be required for
        // basic note visibility.
        return new QOpenGLFramebufferObject(
            cssSize);
    }

    void synchronize(
        QQuickFramebufferObject* item) override
    {
        auto* roll =
            static_cast<PianoRoll*>(
                item);

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
            controller->
                perTrackColors());

        renderer_.setNeuralVisual(
            controller->dominantHue(),
            controller->neuralActivity());

        const auto& colors =
            controller->channelColors();

        for (int i = 0;
             i < colors.size() &&
             i < 16;
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
            controller->
                documentRevision()) {
            revision_ =
                controller->
                    documentRevision();

            renderer_.setDocument(
                &controller->
                    document());
        }
    }

    void render() override
    {
        if (auto* fbo =
                framebufferObject()) {
            renderer_.resize(
                fbo->size().width(),
                fbo->size().height());
        }

        float renderTime =
            syncedTimeSeconds_;

        if (syncedPlaying_) {
            const std::chrono::duration<float>
                elapsed =
                    Clock::now() -
                    syncWallClock_;

            renderTime +=
                elapsed.count();
        }

        renderer_.setCurrentTime(
            renderTime);

        renderer_.renderRoll();

        QQuickOpenGLUtils::
            resetOpenGLState();

        updateFps();

        // GPU neural background animates even while paused, replacing the old
        // QML FrameAnimation/Canvas loop with this single scene-graph source.
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
                now -
                fpsWindowStart_;

        if (elapsed.count() >= .5) {
            if (fps_) {
                fps_->store(
                    static_cast<int>(
                        std::lround(
                            double(
                                fpsFrames_) /
                            elapsed.count())),
                    std::memory_order_relaxed);
            }

            fpsFrames_ = 0;
            fpsWindowStart_ = now;
        }
    }

    qreal dpr_ = 1.0;
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
    : QQuickFramebufferObject(
        parent)
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
        auto request =
            [this]() {
                update();
            };

        connect(
            player,
            &MainWindow::
                documentRevisionChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                noteSpeedChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                postBufferChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                perTrackColorsChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                channelColorsChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                playingChanged,
            this,
            request);

        // Normal animation is driven exclusively by Renderer::update().
        // Paused seeks need an immediate synchronization.
        connect(
            player,
            &MainWindow::
                currentTimeChanged,
            this,
            [this, player]() {
                if (!player->isPlaying())
                    update();
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
