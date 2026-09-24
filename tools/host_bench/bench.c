// Host bench (see HANDOFF §22). Args: sf2 workers seconds K D cc wav. DBG=1 prints ALLOC/BUSY.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

int ssw_init_ex(int, int, int, int, int, int, int, int, int, int, int, int, int, int);
int ssw_load_sf2(const char *);
int ssw_warmup(void);
int ssw_queue_events(const uint32_t *, const double *, int);
int ssw_render_queued_into(uintptr_t, int);
void ssw_set_song_time(double);
int ssw_set_debug_metrics(int), ssw_alloc_us(void), ssw_worker_busy_us(void);
int ssw_active_voices(void), ssw_free_voices(void), ssw_steals(void), ssw_dropped_notes(void), ssw_worker_count(void);

static double now(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

typedef struct { double t; uint32_t m; uint32_t seq; } ev;
static int cmp(const void *a, const void *b) {
    const ev *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t ? 1 : (x->seq < y->seq ? -1 : x->seq > y->seq);
}

int main(int argc, char **argv) {
    const char *sf2 = argc > 1 ? argv[1] : "bench.sf2";
    int workers = argc > 2 ? atoi(argv[2]) : 1;
    double seconds = argc > 3 ? atof(argv[3]) : 2.0;
    int K = argc > 4 ? atoi(argv[4]) : 24;       // distinct keys per channel per tick
    int D = argc > 5 ? atoi(argv[5]) : 56;       // duplicate tracks (VOR)
    int cc_on = argc > 6 ? atoi(argv[6]) : 1;
    const char *wav = argc > 7 ? argv[7] : NULL;
    const int SR = 44100, BLOCK = 512, S = 172;
    const double L = 0.02;

    if (!ssw_init_ex(SR, 2, 32, BLOCK, 16, 0, 8192, 0, workers, 0, 1, 1, 0, 1)) { puts("init fail"); return 1; }
    if (ssw_load_sf2(sf2) <= 0) { puts("sf2 fail"); return 1; }
    ssw_warmup();
    ssw_set_song_time(0.0);

    // Build the event list: per tick, per channel, per duplicate track: CC7 then K note-ons.
    size_t cap = (size_t)(seconds * S + 2) * 16 * D * (2 * K + 1) + 16;
    ev *e = malloc(cap * sizeof(ev)); size_t n = 0;
    for (int tick = 0; tick < (int)(seconds * S); ++tick) {
        double t = tick / (double)S + 0.001;
        for (int ch = 0; ch < 16; ++ch)
            for (int d = 0; d < D; ++d) {
                if (cc_on) e[n] = (ev){t, 0xB0u | ch | (7u << 8) | ((uint32_t)(64 + (tick % 60)) << 16), (uint32_t)n}; ++n;
                for (int k = 0; k < K; ++k) {
                    int key = (ch * 7 + tick * 5 + k * 5) % 120 + 4;
                    e[n] = (ev){t, 0x90u | ch | ((uint32_t)key << 8) | (100u << 16), (uint32_t)n}; ++n;
                    e[n] = (ev){t + L, 0x80u | ch | ((uint32_t)key << 8), (uint32_t)n}; ++n;
                }
            }
    }
    qsort(e, n, sizeof(ev), cmp);
    size_t i = 0;
    float *buf = calloc(BLOCK * 2, sizeof(float));
    FILE *wf = NULL;
    if (wav) { wf = fopen(wav, "wb"); fseek(wf, 44, SEEK_SET); }
    long total_frames = 0;
    uint32_t *mb = malloc(n * sizeof(uint32_t)); double *tb = malloc(n * sizeof(double));
    int blocks = (int)(seconds * SR / BLOCK);
    double wall_sum = 0, cpu_sum = 0, wall_max = 0, alloc_sum = 0, busy_sum = 0;
    if (getenv("DBG")) ssw_set_debug_metrics(1);
    int steals0 = ssw_steals();
    for (int b = 0; b < blocks; ++b) {
        double end_t = (b + 1) * (double)BLOCK / SR;
        int cnt = 0;
        while (i < n && e[i].t < end_t) { mb[cnt] = e[i].m; tb[cnt] = e[i].t; ++cnt; ++i; }
        double w0 = now(CLOCK_MONOTONIC), c0 = now(CLOCK_PROCESS_CPUTIME_ID);
        if (cnt) ssw_queue_events(mb, tb, cnt);
        ssw_render_queued_into((uintptr_t)buf, BLOCK);
        double w = now(CLOCK_MONOTONIC) - w0, c = now(CLOCK_PROCESS_CPUTIME_ID) - c0;
        if (b >= blocks / 4) { alloc_sum += ssw_alloc_us() / 1000.0; busy_sum += ssw_worker_busy_us() / 1000.0; wall_sum += w; cpu_sum += c; if (w > wall_max) wall_max = w; }
        if (wf) { for (int s = 0; s < BLOCK * 2; ++s) { float v = buf[s]; v = v > 1 ? 1 : v < -1 ? -1 : v; int16_t q = (int16_t)(v * 32767); fwrite(&q, 2, 1, wf); } }
        total_frames += BLOCK;
    }
    int measured = blocks - blocks / 4;
    printf("workers=%d K=%d D=%d cc=%d events=%zu  NPS=%.0f\n", ssw_worker_count(), K, D, cc_on, n, 16.0 * K * D * S);
    printf("per block (budget %.2f ms): wall avg %.2f max %.2f | cpu avg %.2f ms\n",
           BLOCK * 1000.0 / SR, wall_sum / measured, wall_max, cpu_sum / measured);
    if (getenv("DBG")) printf("ALLOC avg %.2f ms  BUSY avg %.2f ms (last render call per block, summed over workers)\n", alloc_sum / measured, busy_sum / measured);
    printf("active=%d free=%d steals/s=%.0f dropped=%d\n", ssw_active_voices(), ssw_free_voices(),
           (ssw_steals() - steals0) / seconds, ssw_dropped_notes());
    if (wf) {
        uint32_t data = (uint32_t)total_frames * 4, riff = data + 36;
        fseek(wf, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wf); fwrite(&riff, 4, 1, wf); fwrite("WAVEfmt ", 1, 8, wf);
        uint32_t fs = 16, sr = SR, br = SR * 4; uint16_t fmt = 1, chn = 2, ba = 4, bps = 16;
        fwrite(&fs, 4, 1, wf); fwrite(&fmt, 2, 1, wf); fwrite(&chn, 2, 1, wf); fwrite(&sr, 4, 1, wf);
        fwrite(&br, 4, 1, wf); fwrite(&ba, 2, 1, wf); fwrite(&bps, 2, 1, wf); fwrite("data", 1, 4, wf); fwrite(&data, 4, 1, wf);
        fclose(wf);
    }
    return 0;
}
