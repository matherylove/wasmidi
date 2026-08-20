cmake_minimum_required(VERSION 3.14)
project(wasmidi VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Quick)

qt_standard_project_setup()

qt_add_executable(wasmidi
    WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/mainwindow.cpp
    src/pianoroll.cpp
    src/keyboard.cpp
    src/midi/midi_parser.cpp
    src/midi/scheduler.cpp
)

qt_add_qml_module(wasmidi
    URI wasmidi
    VERSION 1.0
    QML_FILES
        src/qml/MainWindow.qml
        src/qml/PianoRoll.qml
        src/qml/Keyboard.qml
        src/qml/Controls.qml
        src/qml/StatsPanel.qml
)

target_link_libraries(wasmidi PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Quick
)

if(EMSCRIPTEN)
    set_target_properties(wasmidi PROPERTIES
        SUFFIX ".html"
    )
    
    target_link_options(wasmidi PRIVATE
        "-sUSE_WEBGL2=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_FUNCTIONS=[\"_main\",\"_loadMidiFile\",\"_play\",\"_pause\",\"_stop\",\"_seek\",\"_malloc\",\"_free\"]"
        "-sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\"]"
        "-sFULL_ES3=1"
        "-sMAX_WEBGL_VERSION=2"
        "-sMIN_WEBGL_VERSION=2"
        "-sASYNCIFY=1"
        "-sASYNCIFY_IMPORTS=[\"audio_create_context\",\"audio_play\",\"midi_request_access\",\"midi_send\"]"
        "-O3"
    )
endif()