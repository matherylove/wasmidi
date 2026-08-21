#include "midi_document_codec.hpp"
#include "midi_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <limits>
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
EM_JS(void, wmp_post_progress, (int percent, double stageAddress), {
    // Memory64 C/C++ pointers are i64 and enter JavaScript as BigInt. Do not
    // pass a raw const char* into UTF8ToString(): Emscripten 3.1.56 asserts
    // that UTF8ToString receives a Number. C++ legalizes the pointer to f64
    // first; the current <=16 GiB Memory64 address range is exactly representable.
    const stage = Number(stageAddress);
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

EM_JS(void, wmp_post_absolute_progress, (int percent, double stageAddress), {
    const stage = Number(stageAddress);
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
    const double stageAddress = stage
        ? static_cast<double>(reinterpret_cast<std::uintptr_t>(stage))
        : 0.0;
    wmp_post_progress(percent, stageAddress);
#else
    (void)percent;
    (void)stage;
#endif
}

} // namespace

namespace {

constexpr double kJsExactIntegerLimit = 9007199254740991.0; // 2^53 - 1

bool jsExactNonNegativeInteger(double value)
{
    return std::isfinite(value) &&
           value >= 0.0 &&
           value <= kJsExactIntegerLimit &&
           std::floor(value) == value;
}

bool jsAddressToPointer(double value, uint8_t*& pointer)
{
    if (!jsExactNonNegativeInteger(value) ||
        value > static_cast<double>(std::numeric_limits<std::uintptr_t>::max())) {
        pointer = nullptr;
        return false;
    }

    pointer = reinterpret_cast<uint8_t*>(
        static_cast<std::uintptr_t>(value));
    return true;
}

bool jsSizeToNative(double value, std::size_t& size)
{
    if (!jsExactNonNegativeInteger(value) ||
        value > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        size = 0;
        return false;
    }

    size = static_cast<std::size_t>(value);
    return true;
}

double pointerToJsAddress(const void* pointer)
{
    return static_cast<double>(
        reinterpret_cast<std::uintptr_t>(pointer));
}

int parseDocument(const uint8_t* data, std::size_t size)
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

int packDocument()
{
    try {
    #ifdef __EMSCRIPTEN__
        wmp_post_absolute_progress(
            94,
            static_cast<double>(reinterpret_cast<std::uintptr_t>(
                "Packing parsed MIDI")));
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
        wmp_post_absolute_progress(
            95,
            static_cast<double>(reinterpret_cast<std::uintptr_t>(
                "Parsed")));
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

} // namespace

extern "C" {

// MEMORY64 raw pointer exports use JS BigInt, but Emscripten's generated helper
// wrappers are not guaranteed to legalize every pointer/size export in the same
// way. Keep the JavaScript boundary deliberately Number-only instead. Every
// address used by Chromium's current <=16 GiB Memory64 implementation is far
// below 2^53 and is therefore exactly representable as a JavaScript Number.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_alloc_js(double bytes)
{
    std::size_t size = 0;
    if (!jsSizeToNative(bytes, size)) {
        g_error = "Invalid parser allocation size from JavaScript";
        return 0.0;
    }

    if (size == 0)
        size = 1;

    void* pointer = std::malloc(size);
    if (!pointer)
        return 0.0;

    return pointerToJsAddress(pointer);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_free_js(double address)
{
    if (address == 0.0)
        return;

    uint8_t* pointer = nullptr;
    if (!jsAddressToPointer(address, pointer))
        return;

    std::free(pointer);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_parse_js(double address, double byteCount)
{
    uint8_t* data = nullptr;
    std::size_t size = 0;

    if (!jsAddressToPointer(address, data) ||
        !jsSizeToNative(byteCount, size) ||
        (!data && size != 0)) {
        g_error = "Invalid MIDI pointer/size passed from JavaScript";
        return 0;
    }

    return parseDocument(data, size);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_pack()
{
    return packDocument();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_result_ptr_js()
{
    return g_result.empty()
        ? 0.0
        : pointerToJsAddress(g_result.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_result_size_js()
{
    return static_cast<double>(g_result.size());
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
double wmp_error_ptr_js()
{
    return pointerToJsAddress(g_error.c_str());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_error_size_js()
{
    return static_cast<double>(g_error.size());
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
