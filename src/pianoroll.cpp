#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickWindow>

#include <vector>
#include <limits>

namespace {

class PianoRollRenderer final : public QQuickFramebufferObject::Renderer {
public:
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject* item) override
    {
        auto *roll = static_cast<PianoRoll*>(item);
        auto *controller = qobject_cast<MainWindow*>(roll->controller());
        if (!controller)
            return;

        renderer_.resize(static_cast<int>(roll->width()), static_cast<int>(roll->height()));
        renderer_.setCurrentTime(controller->currentTime());
        renderer_.setNoteSpeed(controller->noteSpeed());
        renderer_.setPostBuffer(controller->postBuffer());
        renderer_.setPerTrackColors(controller->perTrackColors());

        const auto& colors = controller->channelColors();
        for (int i = 0; i < colors.size() && i < 16; ++i)
            renderer_.setChannelColor(static_cast<uint8_t>(i),
                                      static_cast<uint8_t>(colors[i].red()),
                                      static_cast<uint8_t>(colors[i].green()),
                                      static_cast<uint8_t>(colors[i].blue()));

        if (revision_ != controller->documentRevision()) {
            revision_ = controller->documentRevision();
            std::vector<wasmidi::NoteInstance> notes;
            notes.reserve(controller->document().notes.size());
            for (const auto& note : controller->document().notes) {
                notes.push_back({note.startTime, note.endTime,
                                 static_cast<float>(note.pitch),
                                 static_cast<float>(note.channel),
                                 static_cast<float>(note.track),
                                 static_cast<float>(note.velocity) / 127.0f});
            }
            renderer_.setNotes(notes);
        }
    }

    void render() override
    {
        renderer_.renderRoll();
        update();
    }

private:
    wasmidi::GLRenderer renderer_;
    quint64 revision_ = std::numeric_limits<quint64>::max();
};

} // namespace

PianoRoll::PianoRoll(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
}

void PianoRoll::setController(QObject* controller)
{
    if (controller_ == controller)
        return;
    controller_ = controller;
    emit controllerChanged();
    update();
}

QQuickFramebufferObject::Renderer* PianoRoll::createRenderer() const
{
    return new PianoRollRenderer();
}
