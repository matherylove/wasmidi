#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QtWebEngineWidgets/QWebEngineView>

#include "mainwindow.hpp"
#include "pianoroll.hpp"
#include "keyboard.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

int main(int argc, char *argv[]) {
#ifdef __EMSCRIPTEN__
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    
    QGuiApplication app(argc, argv);
    app.setApplicationName("WASMIDI Player");
    app.setOrganizationName("Dekxtopia");
    
    qmlRegisterType<PianoRoll>("Wasmidi");
    qmlRegisterType<Keyboard>("Wasmidi");
    qmlRegisterType<MainWindow>("Wasmidi");
    
    QQmlApplicationEngine engine;
    
    const QUrl url(QStringLiteral("qrc:/wasmidi/qml/MainWindow.qml"));
    
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreated,
        &app,
        [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl)
                QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection
    );
    
    engine.load(url);
    
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop([]() {
        QCoreApplication::processEvents();
    }, 0, 1);
#endif
    
    return app.exec();
}