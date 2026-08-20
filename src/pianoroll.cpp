#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>

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

        /*
         * synchronize() is the only safe place to transfer state from the
         * Qt GUI thread into the scene-graph/render thread.
         *
         * Pass 6 called Renderer::update() continuously from render(), but
         * the PianoRoll QQuickItem itself was never marked dirty when
         * MainWindow::currentTime changed. As a result Qt kept rendering with
         * the currentTime captured during the last resize/window event.
         */
        renderer_.resize(
            static_cast<int>(roll->width()),
            static_cast<int>(roll->height()));

        renderer_.setCurrentTime(
            controller->currentTime());

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

        if (revision_ !=
            controller->documentRevision()) {
            revision_ =
                controller->documentRevision();

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
        renderer_.renderRoll();

        /*
         * Do NOT call Renderer::update() here.
         *
         * Rendering is now driven from the GUI-side PianoRoll::update()
         * connections below. That guarantees each requested frame first goes
         * through synchronize() and receives the newest playback time.
         */
    }

private:
    wasmidi::GLRenderer renderer_;
    quint64 revision_ =
        std::numeric_limits<quint64>::max();
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

    if (controller_)
        QObject::disconnect(
            controller_.data(), nullptr,
            this, nullptr);

    controller_ = controller;

    if (auto* player =
            qobject_cast<MainWindow*>(
                controller_.data())) {
        /*
         * QQuickFramebufferObject does not automatically become dirty when
         * arbitrary properties of an object stored in `controller` change.
         * Explicitly request a scene-graph frame for every state mutation that
         * changes the roll.
         */
        connect(
            player, &MainWindow::currentTimeChanged,
            this, [this]() { update(); });

        connect(
            player, &MainWindow::documentRevisionChanged,
            this, [this]() { update(); });

        connect(
            player, &MainWindow::noteSpeedChanged,
            this, [this]() { update(); });

        connect(
            player, &MainWindow::postBufferChanged,
            this, [this]() { update(); });

        connect(
            player, &MainWindow::perTrackColorsChanged,
            this, [this]() { update(); });

        connect(
            player, &MainWindow::channelColorsChanged,
            this, [this]() { update(); });

        connect(
            player, &MainWindow::playingChanged,
            this, [this]() { update(); });
    }

    emit controllerChanged();

    // Forces initial synchronize after assigning the controller.
    update();
}

QQuickFramebufferObject::Renderer*
PianoRoll::createRenderer() const
{
    return new PianoRollRenderer();
}
