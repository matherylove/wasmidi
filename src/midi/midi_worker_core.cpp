#include "midi_document_codec.hpp"
#include "midi_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

wasmidi::MidiParser g_parser;
std::vector<uint8_t> g_result;
std::string g_error;

#ifdef __EMSCRIPTEN__
EM_JS(void, wmp_post_progress, (int percent, const char* stage), {
    const stageText = stage ? UTF8ToString(stage) : "Parsing MIDI";
    const absolutePercent = 15 + Math.floor(
        Math.max(0, Math.min(100, percent | 0)) * 0.79);
    self.__wasmidiMidiParserStage = stageText;
    self.__wasmidiMidiParserPercent = absolutePercent;
    postMessage({
        type: "progress",
        percent: absolutePercent,
        stage: stageText
    });
});

EM_JS(void, wmp_post_absolute_progress, (int percent, const char* stage), {
    const stageText = stage ? UTF8ToString(stage) : "Loading MIDI";
    const absolutePercent = Math.max(0, Math.min(100, percent | 0));
    self.__wasmidiMidiParserStage = stageText;
    self.__wasmidiMidiParserPercent = absolutePercent;
    postMessage({
        type: "progress",
        percent: absolutePercent,
        stage: stageText
    });
});
#endif

void progressCallback(void*, int percent, const char* stage)
{
#ifdef __EMSCRIPTEN__
    wmp_post_progress(percent, stage);
#else
    (void)percent;
    (void)stage;
#endif
}

} // namespace

extern "C" {

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_parse(const uint8_t* data, std::size_t size)
{
    try {
        g_result.clear();
        g_error.clear();

        wasmidi::MidiDocument document;
        if (!g_parser.parse(
                data,
                size,
                document,
                &progressCallback,
                nullptr)) {
            g_error = g_parser.error();
            return 0;
        }

    #ifdef __EMSCRIPTEN__
        wmp_post_absolute_progress(94, "Packing parsed MIDI");
    #endif

        if (!wasmidi::serializeMidiDocument(
                document,
                g_result,
                g_error)) {
            return 0;
        }

    #ifdef __EMSCRIPTEN__
        wmp_post_absolute_progress(95, "Parsed");
    #endif
        return 1;
    } catch (const std::bad_alloc&) {
        g_result.clear();
        g_error =
            "MIDI parser ran out of WebAssembly memory while building indexes";
        return 0;
    } catch (const std::exception& error) {
        g_result.clear();
        g_error = std::string("MIDI parser exception: ") + error.what();
        return 0;
    } catch (...) {
        g_result.clear();
        g_error = "Unknown MIDI parser exception";
        return 0;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const uint8_t* wmp_result_ptr()
{
    return g_result.empty() ? nullptr : g_result.data();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
std::size_t wmp_result_size()
{
    return g_result.size();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* wmp_error_ptr()
{
    return g_error.c_str();
}

} // extern "C"

// Keep a real inert entry point even for Emscripten. The Worker asks the
// modularized runtime not to execute it, and NO_EXIT_RUNTIME=1 makes the module
// remain callable even if a generated-glue/runtime variant does invoke main().
int main()
{
    return 0;
}
