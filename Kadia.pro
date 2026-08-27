QT += core gui widgets
CONFIG += c++11
TARGET = Kadia
TEMPLATE = app

# ---------------------------------------------------------------------------
# Windows XP target: same Qt/toolchain lineage as the supplied Sightline repo.
# ---------------------------------------------------------------------------
win32-msvc* {
    QMAKE_LFLAGS_WINDOWS += /SUBSYSTEM:WINDOWS,5.01
    QMAKE_CXXFLAGS += /utf-8
    DEFINES += _WIN32_WINNT=0x0501 WINVER=0x0501
}

win32-g++ {
    QMAKE_LFLAGS += -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1
    DEFINES += _WIN32_WINNT=0x0501 WINVER=0x0501
}

DEFINES += QT_DEPRECATED_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN

# ---------------------------------------------------------------------------
# FFmpeg dependency copied verbatim from the supplied Sightline project.
# The current mockup mirror only probes the runtime, but the libraries are
# linked and staged so media playback can be implemented without changing the
# XP toolchain/dependency layout later.
# ---------------------------------------------------------------------------
FFMPEG_DIR = $$PWD/third_party/ffmpeg
INCLUDEPATH += $$FFMPEG_DIR/include

win32-msvc* {
    LIBS += -L$$FFMPEG_DIR/lib \
        -lavcodec -lavformat -lavutil -lswscale -lswresample \
        -ld3d9 -lole32 -luser32 -lgdi32 -lgdiplus -lwininet -lshell32 -ladvapi32
}

win32-g++ {
    LIBS += -L$$FFMPEG_DIR/lib \
        -lavcodec -lavformat -lavutil -lswscale -lswresample \
        -ld3d9 -lole32 -luser32 -lgdi32 -lgdiplus -lwininet -lshell32 -ladvapi32
}

SOURCES += \
    src/main.cpp \
    src/kadia_window.cpp \
    src/kadia_scene.cpp \
    src/d3d9_renderer.cpp \
    src/input_manager.cpp \
    src/ffmpeg_runtime.cpp \
    src/klite_bootstrap.cpp \
    src/windspro_bootstrap.cpp \
    src/store_detector.cpp \
    src/background_settings.cpp \
    src/rom_scanner.cpp \
    src/rom_header_detector.cpp \
    src/libretro_metadata.cpp \
    src/screenscraper_client.cpp \
    src/ui_model.cpp

HEADERS += \
    src/kadia_window.h \
    src/kadia_scene.h \
    src/d3d9_renderer.h \
    src/input_manager.h \
    src/ffmpeg_runtime.h \
    src/klite_bootstrap.h \
    src/windspro_bootstrap.h \
    src/store_detector.h \
    src/background_settings.h \
    src/rom_scanner.h \
    src/rom_header_detector.h \
    src/libretro_metadata.h \
    src/screenscraper_client.h \
    src/ui_model.h

RESOURCES += resources.qrc
win32:RC_FILE = kadia.rc
