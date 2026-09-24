/*
 * rev. 40 smoke test: worker_thread() -> worker_cycle() split.
 *
 * Links the real engine natively (no Emscripten), creates 24 SSv2 workers,
 * renders 200 blocks, then shuts the pool down and recreates it three times
 * and clears the soundfonts. This exercises every loop-level exit that was
 * mapped in the split: the idle "active_count == 0" continue, the normal
 * end-of-cycle continue, and the quit path used by voice_shutdown().
 *
 * A worker that fails to see the quit request never exits. The POSIX shim's
 * thread join ignores the timeout, so that shows up as a HANG, not a FAIL:
 * always run it under `timeout` (verified by mutating the quit mapping, which
 * hangs). A slow-but-finishing run is also reported as FAIL. No soundfont is
 * loaded; this checks thread control flow, not audio.
 *
 * From the repo root:
 *   S=third_party/snappysynthv2
 *   cc -O2 -I$S -DSNAPPYSYNTH_WASM=1 -DSSW_WORKER_CYCLE_TRAMPOLINE=1 -pthread \
 *      -o /tmp/wc tools/worker_cycle_smoke.c $S/snappy_wasm_core.c \
 *      $S/Voice/voice.c $S/Parser/sf2_parser.c $S/Parser/sfz_parser.c \
 *      $S/Parser/wav_loader.c $S/wasm_stubs.c -lm && timeout 30 /tmp/wc
 * Repeat with -DSSW_WORKER_CYCLE_TRAMPOLINE=0 to check the rollback shape.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

int ssw_init_ex(int, int, int, int, int, int, int, int, int, int, int, int, int, int);
int ssw_render_queued_into(uintptr_t, int);
int ssw_worker_count(void);
int ssw_queue_events(const uint32_t *, const double *, int);
void ssw_clear_soundfonts(void);

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
    static float buf[512 * 2];
    const double t0 = now_s();
    for (int round = 0; round < 3; ++round) {
        if (!ssw_init_ex(44100, 2, 32, 512, 16, 0, 8192, 0, 24, 0, 1, 1, 0, 1)) {
            puts("FAIL: ssw_init_ex");
            return 1;
        }
        if (ssw_worker_count() != 24) {
            printf("FAIL: expected 24 workers, got %d\n", ssw_worker_count());
            return 1;
        }
        const uint32_t msgs[2] = {0x7F3C90u, 0x003C80u};
        const double times[2] = {0.001, 0.02};
        ssw_queue_events(msgs, times, 2);
        for (int i = 0; i < 200; ++i) {
            if (!ssw_render_queued_into((uintptr_t)buf, 512)) {
                printf("FAIL: render block %d in round %d\n", i, round);
                return 1;
            }
        }
    }
    ssw_clear_soundfonts();
    const double elapsed = now_s() - t0;
    if (elapsed > 3.0) {
        printf("FAIL: %.2f s -- a worker probably missed the quit request\n", elapsed);
        return 1;
    }
    printf("OK worker cycle split (%.3f s)\n", elapsed);
    return 0;
}
