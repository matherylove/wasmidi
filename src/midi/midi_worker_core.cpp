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
wasmidi::MidiDocument g_document;
std::vector<uint8_t> g_result;
std::string g_error;

#ifdef __EMSCRIPTEN__
EM_JS(void, wmp_post_progress, (int percent, const char* stage), {
    const stageText = stage ? UTF8ToString(stage) : "Parsing MIDI";
    const absolutePercent = 15 + Math.floor(
        Math.max(0, Math.min(100, percent | 0)) * 0.79);

    // The production parser runs in a browser Worker, while CI executes the
    // exact generated module in Node. Never assume the Worker-only `self`
    // global exists just to report optional progress.
    const root = typeof globalThis !== "undefined"
        ? globalThis
        : (typeof self !== "undefined" ? self : null);

    if (root) {
        root.__wasmidiMidiParserStage = stageText;
        root.__wasmidiMidiParserPercent = absolutePercent;
    }

    if (typeof postMessage === "function") {
        postMessage({
            type: "progress",
            percent: absolutePercent,
            stage: stageText
        });
    }
});

EM_JS(void, wmp_post_absolute_progress, (int percent, const char* stage), {
    const stageText = stage ? UTF8ToString(stage) : "Loading MIDI";
    const absolutePercent = Math.max(0, Math.min(100, percent | 0));

    const root = typeof globalThis !== "undefined"
        ? globalThis
        : (typeof self !== "undefined" ? self : null);

    if (root) {
        root.__wasmidiMidiParserStage = stageText;
        root.__wasmidiMidiParserPercent = absolutePercent;
    }

    if (typeof postMessage === "function") {
        postMessage({
            type: "progress",
            percent: absolutePercent,
            stage: stageText
        });
    }
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
        std::vector<uint8_t>().swap(g_result);
        g_error.clear();
        g_document = wasmidi::MidiDocument{};

        if (!g_parser.parse(
                data,
                size,
                g_document,
                &progressCallback,
                nullptr)) {
            g_error = g_parser.error();
            g_document = wasmidi::MidiDocument{};
            return 0;
        }

        // Packing is intentionally a separate call. The Worker frees the raw
        // MIDI allocation immediately after this function returns, before a
        // potentially large serialized transfer buffer is allocated.
        return 1;
    } catch (const std::bad_alloc&) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error =
            "MIDI parser could not obtain more memory from the browser/OS while building indexes";
        return 0;
    } catch (const std::exception& error) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error = std::string("MIDI parser exception: ") + error.what();
        return 0;
    } catch (...) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error = "Unknown MIDI parser exception";
        return 0;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_pack()
{
    try {
    #ifdef __EMSCRIPTEN__
        wmp_post_absolute_progress(94, "Packing parsed MIDI");
    #endif

        if (!wasmidi::serializeMidiDocument(
                g_document,
                g_result,
                g_error)) {
            return 0;
        }

        // The wire image is now authoritative in this Worker. Release all
        // parsed source vectors before JS copies/transfers the wire buffer.
        g_document = wasmidi::MidiDocument{};

    #ifdef __EMSCRIPTEN__
        wmp_post_absolute_progress(95, "Parsed");
    #endif
        return 1;
    } catch (const std::bad_alloc&) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error =
            "MIDI parser could not obtain more memory from the browser/OS while packing the parsed document";
        return 0;
    } catch (const std::exception& error) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error = std::string("MIDI pack exception: ") + error.what();
        return 0;
    } catch (...) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error = "Unknown MIDI pack exception";
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
void wmp_release_result()
{
    std::vector<uint8_t>().swap(g_result);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* wmp_error_ptr()
{
    return g_error.c_str();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_pointer_bits()
{
    return static_cast<int>(sizeof(void*) * 8u);
}

} // extern "C"

// Keep a real inert entry point even for Emscripten. The Worker asks the
// modularized runtime not to execute it, and NO_EXIT_RUNTIME=1 makes the module
// remain callable even if a generated-glue/runtime variant does invoke main().
int main()
{
    return 0;
}
