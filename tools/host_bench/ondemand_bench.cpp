// Provisional benchmark (HANDOFF sec. 50): compressed source vs on-demand reads.
#include "bpfa_midi_store.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
using namespace wasmidi;
static int g_fd; static uint64_t g_reads = 0, g_bytes = 0;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) {
    ++g_reads; g_bytes += n;
    std::size_t done = 0;
    while (done < n) { ssize_t r = pread(g_fd, dst + done, n - done, off + done); if (r <= 0) return false; done += r; }
    return true;
}
static double ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }
int main(int argc, char** argv) {
    g_fd = open(argv[1], O_RDONLY);
    const bool onDemand = argc > 2 && std::strcmp(argv[2], "ondemand") == 0;
    const std::size_t pageKb = argc > 3 ? atoi(argv[3]) : 1024, pages = argc > 4 ? atoi(argv[4]) : 64;
    BpfaMidiStore store; MidiDocument meta;
    if (onDemand) store.setOnDemandSource(true, pageKb * 1024, pages); else store.setOnDemandSource(false);
    auto t0 = std::chrono::steady_clock::now();
    if (!store.index(lseek(g_fd, 0, SEEK_END), readAt, nullptr, meta)) { printf("index failed: %s\n", store.error()); return 1; }
    const double loadMs = ms(t0);
    const uint64_t loadBytes = g_bytes;
    const MidiDocument& md = store.metadata();
    // dense playback: synth batches + renderer sweep for 101.5..103.5 s in 50 ms steps
    std::vector<BpfaMidiStore::EventWord> batch; std::vector<BpfaMidiStore::SysExBatchEvent> sx; std::vector<uint8_t> sxb;
    std::vector<VisualNote> app; std::vector<BpfaMidiStore::RenderClose> cl;
    bool complete, hasNext; uint32_t next, base; long events = 0;
    const uint64_t readsBefore = g_reads, bytesBefore = g_bytes;
    t0 = std::chrono::steady_clock::now();
    store.resetEventCursor(uint32_t(md.secondsToTick(101.5)));
    store.resetRenderCursor(uint32_t(md.secondsToTick(101.5)), true);
    for (double t = 101.55; t <= 103.5; t += 0.05) {
        const uint32_t end = uint32_t(std::ceil(md.secondsToTick(t)));
        do { store.buildEventBatch(end, 262144, batch, sx, sxb, complete, next, hasNext); events += batch.size(); } while (!complete);
        do { store.buildRenderSweep(end, 262144, app, cl, base, complete, next, hasNext); } while (!complete);
    }
    const double playMs = ms(t0);
    const uint64_t playReads = g_reads - readsBefore, playBytes = g_bytes - bytesBefore;
    double seek[3]; int i = 0;
    for (double sec : {101.5, 60.0, 125.0}) {
        MidiMappedStore::LiveSnapshot s; t0 = std::chrono::steady_clock::now();
        store.buildLiveSnapshot(md.secondsToTick(sec), md.secondsToTick(sec - 0.25), md.secondsToTick(sec - 1), s, true);
        seek[i++] = ms(t0);
    }
    rusage ru; getrusage(RUSAGE_SELF, &ru);
    printf("%-26s load %6.0f ms (read %5.0f MB) | store %5.0f MB | peak RSS %5.0f MB | dense 2 s: %6.0f ms, %6.0f MB read in %llu calls | seeks %4.0f/%4.0f/%4.0f ms\n",
        onDemand ? "on-demand" : "compressed (rev. 57)", loadMs, loadBytes / 1048576.0, store.memoryBytes() / 1048576.0, ru.ru_maxrss / 1024.0,
        playMs, playBytes / 1048576.0, (unsigned long long)playReads, seek[0], seek[1], seek[2]);
}
