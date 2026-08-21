#include "midi_document_codec.hpp"
#include "midi_parser.hpp"

#include <cstddef>
#include <cstdint>
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
    postMessage({
        type: "progress",
        percent: 15 + Math.floor(
            Math.max(0, Math.min(100, percent | 0)) * 0.79),
        stage: stage ? UTF8ToString(stage) : "Parsing MIDI"
    });
});

EM_JS(void, wmp_post_absolute_progress, (int percent, const char* stage), {
    postMessage({
        type: "progress",
        percent: Math.max(0, Math.min(100, percent | 0)),
        stage: stage ? UTF8ToString(stage) : "Loading MIDI"
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

int main()
{
    return 0;
}
