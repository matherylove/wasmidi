cmake_minimum_required(VERSION 3.21)
project(wasmidi VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml OpenGL)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(wasmidi
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.hpp
    src/pianoroll.cpp
    src/pianoroll.hpp
    src/keyboard.cpp
    src/keyboard.hpp
    src/midi/midi_parser.cpp
    src/midi/midi_parser.hpp
    src/midi/scheduler.cpp
    src/midi/scheduler.hpp
    src/renderer/gl_renderer.cpp
    src/renderer/gl_renderer.hpp
)

qt_add_qml_module(wasmidi
    URI Wasmidi
    VERSION 1.0
    QML_FILES
        src/qml/MainWindow.qml
        src/qml/PianoRoll.qml
        src/qml/Keyboard.qml
        src/qml/Controls.qml
        src/qml/StatsPanel.qml
)

target_include_directories(wasmidi PRIVATE src)

target_link_libraries(wasmidi PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::OpenGL
)

if(EMSCRIPTEN)
    set_target_properties(wasmidi PROPERTIES SUFFIX ".html")
    target_link_options(wasmidi PRIVATE
        "-sUSE_WEBGL2=1"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=134217728"
        "-sSTACK_SIZE=5242880"
        "-O3"
    )
endif()
