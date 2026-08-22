#include "midi_document_codec.hpp"
#include "midi_parser.hpp"
#include "midi_mapped_store.hpp"

#include <algorithm>
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

wasmidi::MidiMappedStore g_mappedStore;
wasmidi::MidiDocument g_document;
std::vector<wasmidi::VisualNote> g_visualPage;
std::vector<wasmidi::MidiMappedStore::EventWord> g_eventBatch;
std::vector<uint32_t> g_keySnapshotWords;
bool g_eventBatchComplete = false;
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

EM_JS(int, wmp_read_file_slice,
      (double sourceOffset, double byteCount, double destinationAddress), {
    const root = typeof globalThis !== "undefined"
        ? globalThis
        : (typeof self !== "undefined" ? self : null);

    const offset = Number(sourceOffset);
    const count = Number(byteCount);
    const destination = Number(destinationAddress);

    const fail = message => {
        if (root)
            root.__wasmidiMidiParserReadError = String(message || "MIDI source read failed");
        return 0;
    };

    if (!root ||
        !Number.isSafeInteger(offset) || offset < 0 ||
        !Number.isSafeInteger(count) || count < 0 ||
        !Number.isSafeInteger(destination) || destination < 0 ||
        !Number.isSafeInteger(destination + count) ||
        destination + count > HEAPU8.length) {
        return fail("Invalid bounded MIDI source read request");
    }

    if (count === 0)
        return 1;

    try {
        let bytes = null;

        // CI/diagnostic path: the exact generated Memory64 module can provide a
        // synchronous read-at hook without requiring browser FileReaderSync.
        if (typeof root.__wasmidiMidiParserReadAt === "function") {
            const result = root.__wasmidiMidiParserReadAt(offset, count);
            if (result instanceof Uint8Array) {
                bytes = result;
            } else if (result instanceof ArrayBuffer) {
                bytes = new Uint8Array(result);
            } else if (ArrayBuffer.isView(result)) {
                bytes = new Uint8Array(
                    result.buffer,
                    result.byteOffset,
                    result.byteLength);
            }
        } else {
            const file = root.__wasmidiMidiParserFile;
            if (!file || typeof file.slice !== "function")
                return fail("Browser MIDI File source is unavailable");

            if (typeof FileReaderSync !== "function")
                return fail("FileReaderSync is unavailable in the MIDI parser Worker");

            let reader = root.__wasmidiMidiParserFileReader;
            if (!reader) {
                reader = new FileReaderSync();
                root.__wasmidiMidiParserFileReader = reader;
            }

            const buffer = reader.readAsArrayBuffer(
                file.slice(offset, offset + count));
            bytes = new Uint8Array(buffer);
        }

        if (!bytes || bytes.byteLength !== count) {
            return fail(
                "MIDI source returned " +
                (bytes ? bytes.byteLength : 0) +
                " bytes, expected " + count);
        }

        HEAPU8.set(bytes, destination);
        return 1;
    } catch (error) {
        return fail(
            error && error.message
                ? error.message
                : String(error || "MIDI source read failed"));
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


bool browserReadAt(void*,
                   uint64_t offset,
                   uint8_t* destination,
                   std::size_t byteCount)
{
#ifdef __EMSCRIPTEN__
    constexpr uint64_t JsExactIntegerLimit = 9007199254740991ull;

    if ((!destination && byteCount != 0) ||
        offset > JsExactIntegerLimit ||
        uint64_t(byteCount) > JsExactIntegerLimit ||
        reinterpret_cast<std::uintptr_t>(destination) > JsExactIntegerLimit) {
        return false;
    }

    return wmp_read_file_slice(
        static_cast<double>(offset),
        static_cast<double>(byteCount),
        static_cast<double>(reinterpret_cast<std::uintptr_t>(destination))) != 0;
#else
    (void)offset;
    (void)destination;
    (void)byteCount;
    return false;
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

int parseFileDocument(std::size_t size)
{
    try {
        std::vector<uint8_t>().swap(g_result);
        std::vector<wasmidi::VisualNote>().swap(g_visualPage);
        std::vector<wasmidi::MidiMappedStore::EventWord>().swap(g_eventBatch);
        std::vector<uint32_t>().swap(g_keySnapshotWords);
        g_error.clear();
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();

        // Pass 13: browser File/Blob is the memory-mapped backing store. The
        // worker indexes every track and builds bounded state checkpoints, but
        // never materializes one CompactEvent/VisualNote per source event.
        if (!g_mappedStore.index(
                static_cast<uint64_t>(size),
                &browserReadAt,
                nullptr,
                g_document,
                &progressCallback,
                nullptr)) {
            g_error = g_mappedStore.error();
            g_document = wasmidi::MidiDocument{};
            return 0;
        }

        return 1;
    } catch (const std::bad_alloc&) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();
        g_error =
            "Mapped MIDI index could not obtain more memory from the browser/OS";
        return 0;
    } catch (const std::exception& error) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();
        g_error = std::string("Mapped MIDI exception: ") + error.what();
        return 0;
    } catch (...) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();
        g_error = "Unknown mapped MIDI parser exception";
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
int wmp_parse_file_js(double byteCount)
{
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size)) {
        g_error = "Invalid browser MIDI source size";
        return 0;
    }

    return parseFileDocument(size);
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
int wmp_build_visual_page_js(double startTick, double endTick)
{
    if (!jsExactNonNegativeInteger(startTick) ||
        !jsExactNonNegativeInteger(endTick) ||
        startTick > double(std::numeric_limits<uint32_t>::max()) ||
        endTick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid mapped visual-page tick range";
        return 0;
    }
    if (!g_mappedStore.buildVisualPage(
            static_cast<uint32_t>(startTick),
            static_cast<uint32_t>(endTick),
            g_visualPage)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build mapped visual page";
        return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_visual_page_ptr_js()
{
    return g_visualPage.empty() ? 0.0 : pointerToJsAddress(g_visualPage.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_visual_page_count_js()
{
    return static_cast<double>(g_visualPage.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_key_snapshot_js(double tick)
{
    if (!jsExactNonNegativeInteger(tick) ||
        tick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid mapped keyboard tick";
        return 0;
    }
    wasmidi::MidiMappedStore::KeySnapshot snapshot;
    if (!g_mappedStore.buildKeySnapshot(static_cast<uint32_t>(tick), snapshot)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build mapped keyboard state";
        return 0;
    }
    g_keySnapshotWords.resize(128u * 3u);
    for (std::size_t p = 0; p < 128; ++p) {
        g_keySnapshotWords[p] = snapshot.counts[p];
        g_keySnapshotWords[128u + p] = snapshot.globalColors[p];
        g_keySnapshotWords[256u + p] = snapshot.trackColors[p];
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_key_snapshot_ptr_js()
{
    return g_keySnapshotWords.empty() ? 0.0 : pointerToJsAddress(g_keySnapshotWords.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_key_snapshot_word_count_js()
{
    return static_cast<double>(g_keySnapshotWords.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_reset_event_cursor_js(double startTick)
{
    const uint32_t tick =
        jsExactNonNegativeInteger(startTick)
            ? static_cast<uint32_t>(std::min<double>(
                  startTick, double(std::numeric_limits<uint32_t>::max())))
            : 0u;
    g_mappedStore.resetEventCursor(tick);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_event_batch_js(double endTick, double maxEvents)
{
    if (!jsExactNonNegativeInteger(endTick) ||
        !jsExactNonNegativeInteger(maxEvents) ||
        endTick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid mapped event-batch request";
        return 0;
    }
    const std::size_t limit = static_cast<std::size_t>(
        std::clamp<double>(maxEvents, 1.0, 262144.0));
    if (!g_mappedStore.buildEventBatch(
            static_cast<uint32_t>(endTick),
            limit,
            g_eventBatch,
            g_eventBatchComplete)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build mapped event batch";
        return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_event_batch_ptr_js()
{
    return g_eventBatch.empty() ? 0.0 : pointerToJsAddress(g_eventBatch.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_event_batch_count_js()
{
    return static_cast<double>(g_eventBatch.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_event_batch_complete_js()
{
    return g_eventBatchComplete ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_tick_to_seconds_js(double tick)
{
    if (!g_mappedStore.valid() || !std::isfinite(tick))
        return 0.0;
    return g_mappedStore.metadata().tickToSeconds(
        static_cast<uint32_t>(std::clamp<double>(
            tick, 0.0, double(g_mappedStore.metadata().maxTick))));
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
