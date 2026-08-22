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
    src/midi/midi_document_codec.cpp
    src/midi/midi_document_codec.hpp
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
    # Emscripten 3.1.56 MEMORY64 has a growMemory() bug: its generated JS can
    # hand BigInt() a fractional page count. Patch the installed JS runtime
    # template before any project link step so the corrected helper is emitted
    # inside MODULARIZE's private factory scope. The patch is idempotent and
    # recognizes newer Emscripten versions that already contain the fix.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_emscripten_memory64_growth.py"
            "${CMAKE_CXX_COMPILER}"
        RESULT_VARIABLE WASMIDI_MEMORY64_PATCH_RESULT
        OUTPUT_VARIABLE WASMIDI_MEMORY64_PATCH_OUTPUT
        ERROR_VARIABLE WASMIDI_MEMORY64_PATCH_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT WASMIDI_MEMORY64_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Could not apply the Emscripten Memory64 growth fix: "
            "${WASMIDI_MEMORY64_PATCH_ERROR}")
    endif()
    message(STATUS "${WASMIDI_MEMORY64_PATCH_OUTPUT}")
    # Parse large MIDI files in a dedicated Worker-owned WASM instance. The Qt
    # module receives only the compact finalized MidiDocument wire image, so the
    # browser UI/render thread never performs the two full MIDI parse passes.
    add_executable(midi_parser_core
        src/midi/midi_parser.cpp
        src/midi/midi_document_codec.cpp
        src/midi/midi_mapped_store.cpp
        src/midi/midi_worker_core.cpp
    )

    set_target_properties(midi_parser_core PROPERTIES
        OUTPUT_NAME "wasmidi-midi-parser"
        SUFFIX ".js"
    )

    target_include_directories(midi_parser_core PRIVATE src)
    target_compile_options(midi_parser_core PRIVATE
        -O3
        -flto
        -fexceptions
        # Chrome 133+ ships WebAssembly Memory64. The MIDI parser is isolated
        # from Qt, so it can use 64-bit pointers without changing the Qt wasm32
        # renderer/player ABI. This removes the artificial 4 GiB parser heap.
        "-sMEMORY64=1"
    )
    target_link_options(midi_parser_core PRIVATE
        -O3
        -flto
        -fexceptions
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=WasmidiMidiParserCore"
        # Keep a conventional Emscripten application lifecycle, but never tear
        # the runtime down. The Worker normally passes noInitialRun=true; if a
        # browser/generated-glue variant still executes main(), the inert main
        # simply returns and NO_EXIT_RUNTIME keeps the exported parser alive.
        "-sNO_EXIT_RUNTIME=1"
        # Package the parser's WASM payload into its JS glue. This removes the
        # possibility of a cached/new .js file being paired with a stale .wasm
        # file after a Pages deployment.
        "-sSINGLE_FILE=1"
        # Node support is build/CI-only. The same generated module runs as a
        # classic browser Worker in production and is executed in Node during
        # CI so a startup abort cannot reach Pages unnoticed again.
        "-sENVIRONMENT=worker,node"
        # Full wasm64 parser. Memory is committed only as allocations require it;
        # the browser/OS may refuse growth earlier under real memory pressure.
        # Current Chromium/V8 Memory64 has a 16 GiB per-memory hard ceiling, so
        # this is the highest useful declaration rather than an app-imposed RAM
        # budget.
        "-sMEMORY64=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=67108864"
        "-sMAXIMUM_MEMORY=17179869184"
        # Do not reserve Emscripten's default +20% geometric slack while growing
        # gigantic black MIDIs. Request only the pages actually needed.
        "-sMEMORY_GROWTH_GEOMETRIC_STEP=0"
        "-sSTACK_SIZE=4194304"
        "-sFILESYSTEM=0"
        # Parser-only assertions keep fatal startup/runtime diagnostics useful;
        # they do not affect the real-time renderer or SnappySynth hot paths.
        "-sASSERTIONS=1"
        "-sINCOMING_MODULE_JS_API=['noInitialRun','noExitRuntime','onAbort','print','printErr']"
        "-sEXPORTED_FUNCTIONS=['_wmp_parse_file_js','_wmp_pack','_wmp_result_ptr_js','_wmp_result_size_js','_wmp_release_result','_wmp_build_visual_page_js','_wmp_visual_page_ptr_js','_wmp_visual_page_count_js','_wmp_build_key_snapshot_js','_wmp_key_snapshot_ptr_js','_wmp_key_snapshot_word_count_js','_wmp_reset_event_cursor_js','_wmp_build_event_batch_js','_wmp_event_batch_ptr_js','_wmp_event_batch_count_js','_wmp_event_batch_complete_js','_wmp_tick_to_seconds_js','_wmp_error_ptr_js','_wmp_error_size_js','_wmp_pointer_bits']"
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString']"
    )

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
        "-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"
        "-sMAXIMUM_MEMORY=2147483648"
        "-sSTACK_SIZE=8388608"
        "-sFILESYSTEM=1"
        "-sFORCE_FILESYSTEM=1"
        "-sEXPORTED_FUNCTIONS=['_ssw_init','_ssw_init_ex','_ssw_load_sf2','_ssw_clear_soundfonts','_ssw_set_volume','_ssw_set_vor_mode','_ssw_set_soft_clip','_ssw_reset','_ssw_render','_ssw_render_into','_ssw_send_sysex','_ssw_active_voices','_ssw_free_voices','_ssw_steals','_ssw_layer_count','_ssw_region_count','_ssw_channels','_ssw_sample_rate','_ssw_bits_per_sample','_ssw_num_buffers','_ssw_worker_count','_ssw_shutdown','_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['FS','WORKERFS','lengthBytesUTF8','stringToUTF8']"
        "-lworkerfs.js"
    )

    add_dependencies(wasmidi midi_parser_core snappysynth_core)

    # Qt for WebAssembly owns the generated HTML/JS/WASM packaging.
    target_link_options(wasmidi PRIVATE
        # Qt remains wasm32, but remove its historical 2 GiB default ceiling.
        # Parsed-document installation can therefore use the full 4 GiB wasm32
        # address space when the browser has enough memory.
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=4294967296"
        "-sMEMORY_GROWTH_GEOMETRIC_STEP=0"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
    )
endif()
