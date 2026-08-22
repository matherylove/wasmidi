#include "midi_document_codec.hpp"

#include <cstring>
#include <limits>
#include <type_traits>

namespace wasmidi {
namespace {

constexpr uint32_t WireMagic = 0x444d5357u; // "WSMD" on little-endian wasm
constexpr uint32_t WireVersion = 4;

template <typename T>
void appendPod(std::vector<uint8_t>& out, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

void appendBytes(
    std::vector<uint8_t>& out,
    const void* data,
    std::size_t size)
{
    if (size == 0)
        return;
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

template <typename T>
bool appendVector(
    std::vector<uint8_t>& out,
    const std::vector<T>& values,
    std::string& error)
{
    static_assert(std::is_trivially_copyable_v<T>);

    if (values.size() >
        static_cast<std::size_t>(
            std::numeric_limits<uint32_t>::max())) {
        error = "MIDI document section exceeds wasm32 wire limits";
        return false;
    }

    const uint32_t count =
        static_cast<uint32_t>(values.size());

    appendPod(out, count);
    appendBytes(
        out,
        values.data(),
        values.size() * sizeof(T));
    return true;
}

class Reader {
public:
    Reader(const uint8_t* data, std::size_t size)
        : cursor_(data), end_(data ? data + size : nullptr)
    {
    }

    template <typename T>
    bool pod(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!take(&value, sizeof(T)))
            return false;
        return true;
    }

    bool take(void* destination, std::size_t bytes)
    {
        if (!cursor_ || !end_ ||
            cursor_ > end_ ||
            bytes > static_cast<std::size_t>(end_ - cursor_)) {
            return false;
        }

        if (bytes != 0 && destination)
            std::memcpy(destination, cursor_, bytes);

        cursor_ += bytes;
        return true;
    }

    template <typename T>
    bool vector(std::vector<T>& values)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        uint32_t count = 0;
        if (!pod(count))
            return false;

        const std::size_t n = count;
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            return false;

        const std::size_t bytes = n * sizeof(T);
        if (!cursor_ || !end_ ||
            cursor_ > end_ ||
            bytes > static_cast<std::size_t>(end_ - cursor_)) {
            return false;
        }

        values.resize(n);
        return take(values.data(), bytes);
    }

    std::size_t remaining() const
    {
        return cursor_ && end_ && cursor_ <= end_
            ? static_cast<std::size_t>(end_ - cursor_)
            : 0;
    }

private:
    const uint8_t* cursor_ = nullptr;
    const uint8_t* end_ = nullptr;
};

} // namespace

bool serializeMidiDocument(
    const MidiDocument& document,
    std::vector<uint8_t>& bytes,
    std::string& error)
{
    bytes.clear();
    error.clear();

    // Reserve a conservative lower bound. This does not need to be exact and
    // merely avoids repeated reallocations for multi-million-note files.
    const std::size_t approximate =
        128 +
        document.events.size() * sizeof(CompactEvent) +
        document.tickGroups.size() * sizeof(TickGroup) +
        document.visualNotes.size() * sizeof(VisualNote) +
        document.visualBlockMaxEnd.size() * sizeof(uint32_t) +
        document.visualKeyStarts.size() * sizeof(VisualKeyEvent) +
        document.visualKeyEnds.size() * sizeof(VisualKeyEvent) +
        document.visualKeyOwners.size() * sizeof(VisualKeyOwner) +
        document.derivedNpsTimeline.size() * sizeof(uint32_t);

    bytes.reserve(approximate);

    appendPod(bytes, WireMagic);
    appendPod(bytes, WireVersion);
    appendPod(bytes, document.format);
    appendPod(bytes, document.trackCount);
    appendPod(bytes, document.ticksPerBeat);
    appendPod(bytes, document._pad);
    const uint8_t remoteIndexed = document.remoteIndexed ? 1u : 0u;
    appendPod(bytes, remoteIndexed);
    const uint8_t remotePad[3] = {0, 0, 0};
    appendBytes(bytes, remotePad, sizeof(remotePad));
    appendPod(bytes, document.maxTick);
    appendPod(bytes, document.durationSeconds);
    appendPod(bytes, document.noteCount);
    appendPod(bytes, document.controlEventCount);
    appendPod(bytes, document.minPitch);
    appendPod(bytes, document.maxPitch);

    const uint8_t hasPitch = document.hasPitch ? 1u : 0u;
    appendPod(bytes, hasPitch);
    const uint8_t derivedStatsReady = document.derivedStatsReady ? 1u : 0u;
    appendPod(bytes, derivedStatsReady);
    appendPod(bytes, document.derivedPeakNps);
    appendPod(bytes, document.derivedPeakNpsTime);
    appendPod(bytes, document.derivedPeakPolyphony);

    if (!appendVector(bytes, document.events, error) ||
        !appendVector(bytes, document.tickGroups, error) ||
        !appendVector(bytes, document.visualNotes, error) ||
        !appendVector(bytes, document.visualBlockMaxEnd, error) ||
        !appendVector(bytes, document.visualKeyStarts, error) ||
        !appendVector(bytes, document.visualKeyEnds, error) ||
        !appendVector(bytes, document.visualKeyOwners, error) ||
        !appendVector(bytes, document.derivedNpsTimeline, error) ||
        !appendVector(bytes, document.tempoMap, error) ||
        !appendVector(bytes, document.tempoSeconds, error) ||
        !appendVector(bytes, document.activeChannelMasks, error)) {
        bytes.clear();
        return false;
    }

    if (document.sysEx.size() >
        static_cast<std::size_t>(
            std::numeric_limits<uint32_t>::max())) {
        error = "Too many SysEx events for wasm32 wire format";
        bytes.clear();
        return false;
    }

    const uint32_t sysexCount =
        static_cast<uint32_t>(document.sysEx.size());
    appendPod(bytes, sysexCount);

    for (const SysExEvent& event : document.sysEx) {
        if (event.data.size() >
            static_cast<std::size_t>(
                std::numeric_limits<uint32_t>::max())) {
            error = "SysEx event exceeds wasm32 wire limits";
            bytes.clear();
            return false;
        }

        appendPod(bytes, event.tick);
        const uint32_t length =
            static_cast<uint32_t>(event.data.size());
        appendPod(bytes, length);
        appendBytes(bytes, event.data.data(), event.data.size());
    }

    return true;
}

bool deserializeMidiDocument(
    const uint8_t* bytes,
    std::size_t size,
    MidiDocument& document,
    std::string& error)
{
    document = MidiDocument{};
    error.clear();

    Reader reader(bytes, size);
    uint32_t magic = 0;
    uint32_t version = 0;

    if (!reader.pod(magic) ||
        !reader.pod(version) ||
        magic != WireMagic ||
        version != WireVersion) {
        error = "Invalid MIDI worker document header";
        return false;
    }

    uint8_t hasPitch = 0;
    uint8_t derivedStatsReady = 0;
    uint8_t remoteIndexed = 0;
    uint8_t remotePad[3]{};

    if (!reader.pod(document.format) ||
        !reader.pod(document.trackCount) ||
        !reader.pod(document.ticksPerBeat) ||
        !reader.pod(document._pad) ||
        !reader.pod(remoteIndexed) ||
        !reader.take(remotePad, sizeof(remotePad)) ||
        !reader.pod(document.maxTick) ||
        !reader.pod(document.durationSeconds) ||
        !reader.pod(document.noteCount) ||
        !reader.pod(document.controlEventCount) ||
        !reader.pod(document.minPitch) ||
        !reader.pod(document.maxPitch) ||
        !reader.pod(hasPitch) ||
        !reader.pod(derivedStatsReady) ||
        !reader.pod(document.derivedPeakNps) ||
        !reader.pod(document.derivedPeakNpsTime) ||
        !reader.pod(document.derivedPeakPolyphony) ||
        !reader.vector(document.events) ||
        !reader.vector(document.tickGroups) ||
        !reader.vector(document.visualNotes) ||
        !reader.vector(document.visualBlockMaxEnd) ||
        !reader.vector(document.visualKeyStarts) ||
        !reader.vector(document.visualKeyEnds) ||
        !reader.vector(document.visualKeyOwners) ||
        !reader.vector(document.derivedNpsTimeline) ||
        !reader.vector(document.tempoMap) ||
        !reader.vector(document.tempoSeconds) ||
        !reader.vector(document.activeChannelMasks)) {
        error = "Truncated MIDI worker document";
        document = MidiDocument{};
        return false;
    }

    document.hasPitch = hasPitch != 0;
    document.derivedStatsReady = derivedStatsReady != 0;
    document.remoteIndexed = remoteIndexed != 0;

    uint32_t sysexCount = 0;
    if (!reader.pod(sysexCount)) {
        error = "Truncated MIDI worker SysEx table";
        document = MidiDocument{};
        return false;
    }

    document.sysEx.resize(sysexCount);

    for (SysExEvent& event : document.sysEx) {
        uint32_t length = 0;
        if (!reader.pod(event.tick) ||
            !reader.pod(length) ||
            length > reader.remaining()) {
            error = "Truncated MIDI worker SysEx event";
            document = MidiDocument{};
            return false;
        }

        event.data.resize(length);
        if (!reader.take(event.data.data(), length)) {
            error = "Truncated MIDI worker SysEx payload";
            document = MidiDocument{};
            return false;
        }
    }

    if (reader.remaining() != 0) {
        error = "Unexpected trailing MIDI worker document data";
        document = MidiDocument{};
        return false;
    }

    return true;
}

} // namespace wasmidi
