#pragma once
#include <QPointer>
#include <QQuickFramebufferObject>
#include <QQmlEngine>

class Keyboard : public QQuickFramebufferObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(KeyboardSurface)
    Q_PROPERTY(QObject* controller READ controller WRITE setController NOTIFY controllerChanged)
public:
    explicit Keyboard(QQuickItem* parent = nullptr);
    QObject* controller() const { return controller_.data(); }
    void setController(QObject* controller);
    Renderer* createRenderer() const override;
signals:
    void controllerChanged();
private:
    QPointer<QObject> controller_;
};
