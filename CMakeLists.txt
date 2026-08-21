cmake_minimum_required(VERSION 3.21)
project(wasmidi VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
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
    # Qt stays single-threaded. SnappySynthV2 is a completely separate WASM
    # module hosted in its own browser Worker and may therefore use pthreads
    # without moving the QML/renderer runtime off Qt's wasm_singlethread build.
    add_executable(snappysynth_core
        third_party/snappysynthv2/snappy_wasm_core.c
        third_party/snappysynthv2/wasm_stubs.c
        third_party/snappysynthv2/Voice/voice.c
        third_party/snappysynthv2/Parser/sf2_parser.c
        third_party/snappysynthv2/Parser/sfz_parser.c
        third_party/snappysynthv2/Parser/wav_loader.c
    )

    set_target_properties(snappysynth_core PROPERTIES
        OUTPUT_NAME "snappysynth-core"
        SUFFIX ".js"
    )

    target_include_directories(snappysynth_core PRIVATE
        third_party/snappysynthv2
    )

    target_compile_definitions(snappysynth_core PRIVATE
        SNAPPYSYNTH_WASM=1
    )

    target_compile_options(snappysynth_core PRIVATE
        -O3
        -flto
        -pthread
        -msimd128
    )

    target_link_options(snappysynth_core PRIVATE
        -O3
        -flto
        -pthread
        -msimd128
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=SnappySynthCore"
        "-sENVIRONMENT=worker"
        "-sPTHREAD_POOL_SIZE=2"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"
        "-sMAXIMUM_MEMORY=2147483648"
        "-sSTACK_SIZE=8388608"
        "-sFILESYSTEM=1"
        "-sFORCE_FILESYSTEM=1"
        "-sEXPORTED_FUNCTIONS=['_ssw_init','_ssw_load_sf2','_ssw_set_volume','_ssw_set_vor_mode','_ssw_reset','_ssw_render','_ssw_active_voices','_ssw_channels','_ssw_sample_rate','_ssw_shutdown','_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['FS','WORKERFS','lengthBytesUTF8','stringToUTF8']"
        "-lworkerfs.js"
    )

    add_dependencies(wasmidi snappysynth_core)

    # Qt for WebAssembly owns the generated HTML/JS/WASM packaging.
    target_link_options(wasmidi PRIVATE
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
    )
endif()
