// Single-thread sketch of Kimrashi's packed-note format, to measure it on a MIDI (HANDOFF sec. 52). Not used by WASMIDI.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
static uint64_t GEAR[256];
static void putv(std::vector<uint8_t>& o, uint64_t v) { int s = 63; while (s > 0 && !(v >> s)) s -= 7; while (s > 0) { o.push_back(uint8_t(((v >> s) & 0x7f) | 0x80)); s -= 7; } o.push_back(uint8_t(v & 0x7f)); }
template <class F> static uint32_t walk(const uint8_t* b, size_t len, F f) {
    size_t c = 0; uint32_t t = 0; uint8_t run = 0;
    while (c < len) {
        uint32_t d = 0; uint8_t x; do { x = b[c++]; d = (d << 7) | (x & 0x7f); } while (x & 0x80); t += d;
        uint8_t s = b[c]; if (s < 0x80) s = run; else { if (s < 0xf0) run = s; ++c; }
        if (s >= 0x80 && s < 0xf0) { const uint8_t ty = s & 0xf0; uint8_t d1 = b[c++] & 0x7f, d2 = 0; if (ty != 0xc0 && ty != 0xd0) d2 = b[c++] & 0x7f; f(t, s, d1, d2); }
        else if (s == 0xf0 || s == 0xf7) { uint32_t l = 0; do { x = b[c++]; l = (l << 7) | (x & 0x7f); } while (x & 0x80); c += l; }
        else if (s == 0xff) { c++; uint32_t l = 0; do { x = b[c++]; l = (l << 7) | (x & 0x7f); } while (x & 0x80); c += l; }
        else break;
    }
    return t;
}
int main(int argc, char** argv) {
    uint64_t x = 0x9E3779B97F4A7C15ull; for (int i = 0; i < 256; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; GEAR[i] = x; }
    int fd = open(argv[1], O_RDONLY); struct stat st; fstat(fd, &st);
    const uint8_t* d = (const uint8_t*)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    for (off_t i = 0; i < st.st_size; i += 4096) (void)*(volatile const uint8_t*)(d + i);
    auto t0 = std::chrono::steady_clock::now();
    int ntrk = (d[10] << 8) | d[11]; uint64_t pos = 14, notes = 0, logical = 0;
    std::unordered_map<std::string, uint32_t> dedup; uint64_t store = 0, refs = 0;
    std::vector<uint32_t> ends, next; std::vector<uint8_t> data; std::vector<size_t> cuts;
    for (int t = 0; t < ntrk; ++t) {
        uint64_t len = (uint32_t(d[pos+4]) << 24) | (d[pos+5] << 16) | (d[pos+6] << 8) | d[pos+7];
        const uint8_t* b = d + pos + 8; pos += 8 + len;
        ends.clear(); next.clear(); data.clear(); cuts.clear();
        std::vector<uint32_t> head(2048, ~0u), tail(2048, ~0u);
        walk(b, len, [&](uint32_t tick, uint8_t s, uint8_t d1, uint8_t d2) {
            const int slot = (s & 15) * 128 + d1; const uint8_t ty = s & 0xf0;
            if (ty == 0x90 && d2) { uint32_t o = ends.size(); ends.push_back(~0u); next.push_back(~0u); if (tail[slot] == ~0u) head[slot] = o; else next[tail[slot]] = o; tail[slot] = o; }
            else if (ty == 0x80 || ty == 0x90) { uint32_t o = head[slot]; if (o != ~0u) { ends[o] = tick; head[slot] = next[o]; if (head[slot] == ~0u) tail[slot] = ~0u; } }
        });
        uint32_t last = 0, rdur = 0; uint8_t run = 0, rvel = 0; size_t ord = 0, chunkStart = 0; uint64_t h = 0;
        auto cut = [&](size_t start) { for (size_t i = start; i < data.size(); ++i) h = (h << 1) + GEAR[data[i]]; size_t l = data.size() - chunkStart; if ((l >= 64 && (h & 255) == 0) || l >= 1024) { cuts.push_back(data.size()); chunkStart = data.size(); h = 0; } };
        walk(b, len, [&](uint32_t tick, uint8_t s, uint8_t d1, uint8_t d2) {
            const uint8_t ty = s & 0xf0; const bool on = ty == 0x90 && d2;
            if (ty == 0x80 || (ty == 0x90 && !d2)) return;
            uint64_t delta = uint64_t(tick - last) << 2; last = tick; size_t start = data.size();
            if (!on) { putv(data, delta); if (s != run) { data.push_back(s); run = s; } data.push_back(d1); if (ty != 0xc0 && ty != 0xd0) data.push_back(d2); cut(start); return; }
            uint32_t e = ends[ord++]; uint32_t dur = e == ~0u ? 0 : e - tick + 1;
            bool nv = d2 != rvel, nd = dur != rdur;
            putv(data, delta | (nv ? 1 : 0) | (nd ? 2 : 0)); if (s != run) { data.push_back(s); run = s; } data.push_back(d1);
            if (nv) { data.push_back(d2); rvel = d2; } if (nd) { putv(data, dur); rdur = dur; }
            ++notes; cut(start);
        });
        if (cuts.empty() || cuts.back() < data.size()) cuts.push_back(data.size());
        size_t s0 = 0; logical += data.size();
        for (size_t c : cuts) { std::string k((const char*)data.data() + s0, c - s0); auto it = dedup.find(k); if (it == dedup.end()) { dedup.emplace(std::move(k), 0); store += c - s0; } ++refs; s0 = c; }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("notes %llu | packed before dedup %.1f MB (%.2f B/note) | after dedup %.1f MB + refs %.1f MB = %.2f B/note | single thread %.0f ms\n",
        (unsigned long long)notes, logical / 1048576.0, double(logical) / notes, store / 1048576.0, refs * 4 / 1048576.0, double(store + refs * 4) / notes, ms);
}
