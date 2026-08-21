#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

namespace {

class PianoRollRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    explicit PianoRollRenderer(qreal dpr)
        : dpr_(std::max<qreal>(1.0, dpr))
    {
    }

    QOpenGLFramebufferObject*
    createFramebufferObject(const QSize& size) override
    {
        // Match the original HTML canvas raster density. Qt supplies a
        // DPR-scaled requested size, so divide once when creating the FBO.
        const QSize cssSize(
            std::max(
                64,
                qRound(
                    size.width() / dpr_)),
            std::max(
                64,
                qRound(
                    size.height() / dpr_)));

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

            renderer_.setDocument(
                &controller->document());
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

        // Single animation source for the roll.
        if (syncedPlaying_)
            update();
    }

private:
    using Clock =
        std::chrono::steady_clock;

    qreal dpr_ = 1.0;
    wasmidi::GLRenderer renderer_;

    quint64 revision_ =
        std::numeric_limits<
            quint64>::max();

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

        // Playback frames are render-thread driven. Synchronize only paused
        // seeks or large discontinuities so Qt does not schedule a second loop.
        auto lastControllerTime =
            std::make_shared<float>(
                player->currentTime());

        connect(
            player,
            &MainWindow::currentTimeChanged,
            this,
            [this,
             player,
             lastControllerTime]() {
                const float now =
                    player->currentTime();

                const float delta =
                    std::fabs(
                        now -
                        *lastControllerTime);

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
    const qreal dpr =
        window()
            ? window()->devicePixelRatio()
            : 1.0;

    return new PianoRollRenderer(dpr);
}
