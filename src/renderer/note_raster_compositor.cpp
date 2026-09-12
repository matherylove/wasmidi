#include "note_raster_compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wasmidi {
namespace {

static_assert(sizeof(CompositorNote) == 12,
    "CompositorNote must match VisualNote in src/midi/midi_parser.hpp");

inline int PopCount64(std::uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(value);
#else
    value = value - ((value >> 1) & 0x5555555555555555ull);
    value = (value & 0x3333333333333333ull) +
            ((value >> 2) & 0x3333333333333333ull);
    value = (value + (value >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return static_cast<int>((value * 0x0101010101010101ull) >> 56);
#endif
}

// Stable LSD radix sort of note indices by duration ascending, 4 passes of 8
// bits. Only needed for ShortestOnTop; LatestStartOnTop gets its front-to-back
// order for free by walking the array backwards.
void SortByDuration(const std::uint32_t* durations, std::size_t count,
                    CompositorScratch& scratch) {
    scratch.order.resize(count);
    scratch.scratchOrder.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        scratch.order[i] = static_cast<std::uint32_t>(i);
    scratch.counts.resize(256);

    std::uint32_t* from = scratch.order.data();
    std::uint32_t* to = scratch.scratchOrder.data();
    for (int shift = 0; shift < 32; shift += 8) {
        std::memset(scratch.counts.data(), 0, 256 * sizeof(std::uint32_t));
        for (std::size_t i = 0; i < count; ++i)
            ++scratch.counts[(durations[from[i]] >> shift) & 0xffu];
        std::uint32_t total = 0;
        for (int bucket = 0; bucket < 256; ++bucket) {
            const std::uint32_t here = scratch.counts[bucket];
            scratch.counts[bucket] = total;
            total += here;
        }
        for (std::size_t i = 0; i < count; ++i)
            to[scratch.counts[(durations[from[i]] >> shift) & 0xffu]++] = from[i];
        std::swap(from, to);
    }
    // Four passes is an even number, so the result is back in scratch.order.
    if (from != scratch.order.data())
        scratch.order.assign(from, from + count);
}

}  // namespace

void CullViewport(
    const CompositorNote* notes,
    std::size_t noteCount,
    const CompositorSettings& settings,
    CompositorScratch& scratch,
    CompositorResult& out) {

    out.notes.clear();
    out.visitedNotes = noteCount;
    out.culledNotes = 0;
    out.rejectedByFullRow = 0;

    const int width = (std::max)(1, settings.rasterWidth);
    const int keyCount = (std::max)(0, settings.keyCount);
    if (keyCount == 0 || notes == nullptr || noteCount == 0)
        return;

    // One occupancy bit per cell. For 128 keys at 1920 columns this is 30 KB
    // and stays in cache, where the previous owner-per-cell array was 3.9 MB.
    const std::size_t wordsPerRow = (static_cast<std::size_t>(width) + 63u) / 64u;
    scratch.occupancy.assign(
        wordsPerRow * static_cast<std::size_t>(keyCount), 0ull);
    scratch.uncovered.assign(static_cast<std::size_t>(keyCount),
        static_cast<std::uint32_t>(width));

    const std::int64_t viewStart = static_cast<std::int64_t>(settings.startTick);
    const std::int64_t viewEnd = settings.viewEndTick != 0
        ? static_cast<std::int64_t>(settings.viewEndTick)
        : viewStart + static_cast<std::int64_t>(settings.spanTicks);
    const double span =
        static_cast<double>((std::max<std::uint32_t>)(1u, settings.spanTicks));
    const double widthAsDouble = static_cast<double>(width);

    std::vector<std::uint8_t> keep(noteCount, 0u);

    // Front-to-back order. LatestStartOnTop is reverse index order and needs no
    // sort at all; ShortestOnTop needs a duration sort first.
    std::vector<std::uint32_t> durations;
    const std::uint32_t* sorted = nullptr;
    if (settings.layering == Layering::ShortestOnTop) {
        durations.resize(noteCount);
        for (std::size_t i = 0; i < noteCount; ++i) {
            const std::int64_t s = notes[i].startTick;
            const std::int64_t e = notes[i].endTick > 0u
                ? static_cast<std::int64_t>(notes[i].endTick) : viewEnd;
            durations[i] = static_cast<std::uint32_t>(
                (std::max<std::int64_t>)(0, e - s));
        }
        SortByDuration(durations.data(), noteCount, scratch);
        sorted = scratch.order.data();
    }

    for (std::size_t step = 0; step < noteCount; ++step) {
        const std::size_t index =
            sorted ? sorted[step] : (noteCount - 1u - step);
        const CompositorNote& note = notes[index];

        const int pitch = static_cast<int>((note.packedData >> 8) & 0xffu);
        const int row = pitch - settings.firstKey;
        if (row < 0 || row >= keyCount)
            continue;

        // Whole row already covered: reject without touching a cell.
        std::uint32_t& uncovered = scratch.uncovered[static_cast<std::size_t>(row)];
        if (uncovered == 0u) {
            ++out.rejectedByFullRow;
            continue;
        }

        const std::int64_t noteStart = static_cast<std::int64_t>(note.startTick);
        const std::int64_t noteEnd = note.endTick > 0u
            ? static_cast<std::int64_t>(note.endTick)
            : viewEnd;
        if (noteEnd <= viewStart || noteStart >= viewEnd)
            continue;

        // This arithmetic must match the renderer's own mapping exactly,
        // including the order of the operations. A faster fixed-point form was
        // tried and rounded differently, which let the culler drop notes that
        // did own a pixel. Keep it as written.
        int left = static_cast<int>(std::floor(
            widthAsDouble * (static_cast<double>(noteStart - viewStart) / span)));
        int right = static_cast<int>(std::ceil(
            widthAsDouble * (static_cast<double>(noteEnd - viewStart) / span)));
        // A note thinner than a cell still covers the cell it falls in; the
        // renderer draws it, so culling it would remove a visible pixel.
        if (right <= left) right = left + 1;
        if (left < 0) left = 0;
        if (right > width) right = width;
        if (right <= left)
            continue;

        std::uint64_t* rowWords =
            scratch.occupancy.data() + static_cast<std::size_t>(row) * wordsPerRow;
        const int firstWord = left >> 6;
        const int lastWord = (right - 1) >> 6;
        int claimed = 0;

        for (int word = firstWord; word <= lastWord; ++word) {
            const int lo = (word == firstWord) ? (left & 63) : 0;
            const int hi = (word == lastWord) ? ((right - 1) & 63) : 63;
            const std::uint64_t mask = (hi - lo) == 63
                ? ~0ull
                : (((1ull << (hi - lo + 1)) - 1ull) << lo);
            const std::uint64_t current = rowWords[word];
            const std::uint64_t free = ~current & mask;
            if (!free)
                continue;
            claimed += PopCount64(free);
            rowWords[word] = current | mask;
        }

        if (claimed) {
            keep[index] = 1u;
            uncovered -= static_cast<std::uint32_t>(claimed);
        }
    }

    out.notes.reserve(noteCount / 4u + 16u);
    for (std::size_t index = 0; index < noteCount; ++index)
        if (keep[index])
            out.notes.push_back(notes[index]);
    out.culledNotes = noteCount - out.notes.size();
}

}  // namespace wasmidi
