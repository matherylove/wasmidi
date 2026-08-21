#pragma once

#include <QPointer>
#include <QQuickFramebufferObject>
#include <QQmlEngine>

#include <atomic>

class PianoRoll : public QQuickFramebufferObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(PianoRollSurface)
    Q_PROPERTY(QObject* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(int renderFps READ renderFps)

public:
    explicit PianoRoll(QQuickItem* parent = nullptr);

    QObject* controller() const { return controller_.data(); }
    void setController(QObject* controller);

    int renderFps() const {
        return renderFps_.load(std::memory_order_relaxed);
    }

    Renderer* createRenderer() const override;

signals:
    void controllerChanged();

private:
    QPointer<QObject> controller_;
    mutable std::atomic<int> renderFps_{0};
};
