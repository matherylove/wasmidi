#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickWindow>

#include <limits>
#include <vector>

namespace {

class PianoRollRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    QOpenGLFramebufferObject*
    createFramebufferObject(
        const QSize& size) override
    {
        // No depth/stencil is needed for MPWGL2's texture-scroll renderer.
        return new QOpenGLFramebufferObject(size);
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
                static_cast<uint8_t>(
                    colors[i].red()),
                static_cast<uint8_t>(
                    colors[i].green()),
                static_cast<uint8_t>(
                    colors[i].blue()));
        }

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
        renderer_.renderRoll();

        // Continuous render loop, equivalent to MPWGL2's requestAnimationFrame
        // GL loop. synchronize() refreshes currentTime before every frame.
        update();
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

void PianoRoll::setController(
    QObject* controller)
{
    if (controller_ == controller)
        return;

    controller_ = controller;
    emit controllerChanged();
    update();
}

QQuickFramebufferObject::Renderer*
PianoRoll::createRenderer() const
{
    return new PianoRollRenderer();
}
