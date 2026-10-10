// Time-to-ready of a progressive load (HANDOFF sec. 50): header, chunk table, full
// first track (tempo map in format 1) and one page at the start of every track.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char** argv) {
    int fd = open(argv[1], O_RDONLY);
    auto t0 = std::chrono::steady_clock::now();
    uint8_t h[14]; pread(fd, h, 14, 0);
    const int ntrk = (h[10] << 8) | h[11]; uint64_t pos = 14;
    std::vector<uint64_t> begin(ntrk), len(ntrk);
    for (int t = 0; t < ntrk; ++t) { uint8_t c[8]; pread(fd, c, 8, pos); len[t] = (uint32_t(c[4]) << 24) | (c[5] << 16) | (c[6] << 8) | c[7]; begin[t] = pos + 8; pos += 8 + len[t]; }
    const double tableMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::vector<uint8_t> track0(len[0]); pread(fd, track0.data(), len[0], begin[0]);
    long tempos = 0; size_t p = 0; uint8_t run = 0;
    while (p < track0.size()) {
        while (track0[p++] & 0x80) {}
        uint8_t s = track0[p];
        if (s < 0x80) s = run; else { ++p; if (s < 0xf0) run = s; }
        if (s == 0xff) { uint8_t ty = track0[p++]; uint32_t l = 0, c; do { c = track0[p++]; l = (l << 7) | (c & 0x7f); } while (c & 0x80); if (ty == 0x51) ++tempos; p += l; if (ty == 0x2f) break; continue; }
        if (s == 0xf0 || s == 0xf7) { uint32_t l = 0, c; do { c = track0[p++]; l = (l << 7) | (c & 0x7f); } while (c & 0x80); p += l; continue; }
        const uint8_t ty = s & 0xf0; p += (ty == 0xc0 || ty == 0xd0) ? 1 : 2;
    }
    std::vector<uint8_t> page(1 << 20);
    for (int t = 0; t < ntrk; ++t) pread(fd, page.data(), std::min<uint64_t>(page.size(), len[t]), begin[t]);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("tracks %d, track 0: %.2f MB with %ld tempo events | chunk table %.1f ms | ready to play %.1f ms\n", ntrk, len[0] / 1048576.0, tempos, tableMs, ms);
}
