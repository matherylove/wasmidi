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

    // Reverse index order is front-to-back under BPFA's layering, since the
    // array is sorted by startTick. No sort, and the first writer of a cell is
    // final.
    for (std::size_t step = 0; step < noteCount; ++step) {
        const std::size_t index = noteCount - 1u - step;
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
