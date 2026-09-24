// Real-MIDI host bench (HANDOFF §23). Args: sf2 mid workers start_s dur_s [wav]. Env: DBG=1, VOICES=n.
// Env: DBG=1 prints ALLOC/BUSY per second.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

int ssw_init_ex(int, int, int, int, int, int, int, int, int, int, int, int, int, int);
int ssw_load_sf2(const char *);
int ssw_warmup(void);
int ssw_queue_events(const uint32_t *, const double *, int);
int ssw_render_queued_into(uintptr_t, int);
void ssw_set_song_time(double);
int ssw_set_debug_metrics(int), ssw_alloc_us(void), ssw_worker_busy_us(void);
int ssw_active_voices(void), ssw_free_voices(void), ssw_steals(void), ssw_dropped_notes(void), ssw_worker_count(void);

static double now(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

typedef struct { const uint8_t *p, *end; uint64_t tick; uint8_t rs; int done; } trk;
static const uint8_t *g_base;
static uint32_t vlq(const uint8_t **p) { uint32_t v = 0; uint8_t c; do { c = *(*p)++; v = (v << 7) | (c & 0x7f); } while (c & 0x80); return v; }
static void trk_advance(trk *t) { if (t->p >= t->end) { t->done = 1; return; } t->tick += vlq(&t->p); }

// Decodes one event at t->p. Returns channel message (0 if none); sets *tempo when a tempo meta is seen.
static uint32_t trk_event(trk *t, uint32_t *tempo) {
    uint8_t s = *t->p;
    if (s < 0x80) s = t->rs; else t->p++;
    if (s >= 0xF0) {
        if (s == 0xFF) {
            uint8_t type = *t->p++; uint32_t len = vlq(&t->p);
            if (type == 0x51 && len == 3) *tempo = (t->p[0] << 16) | (t->p[1] << 8) | t->p[2];
            if (type == 0x2F) t->done = 1;
            t->p += len;
        } else { uint32_t len = vlq(&t->p); t->p += len; }
        return 0;
    }
    t->rs = s;
    uint32_t d1 = *t->p++, d2 = 0;
    if ((s & 0xE0) != 0xC0) d2 = *t->p++;
    return s | (d1 << 8) | (d2 << 16);
}

typedef struct { uint64_t tick; uint32_t us; } tempo_pt;

int main(int argc, char **argv) {
    if (argc < 6) { puts("usage: sf2 mid workers start_s dur_s [wav]"); return 1; }
    const char *sf2 = argv[1], *mid = argv[2];
    int workers = atoi(argv[3]); double start = atof(argv[4]), dur = atof(argv[5]);
    const int SR = 44100, BLOCK = 512;
    int fd = open(mid, O_RDONLY); struct stat st; fstat(fd, &st);
    g_base = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    const uint8_t *p = g_base;
    int ntrk = (p[10] << 8) | p[11], div = (p[12] << 8) | p[13];
    trk *T = calloc(ntrk, sizeof(trk)); p += 14;
    for (int i = 0; i < ntrk; ++i) {
        while (memcmp(p, "MTrk", 4)) { uint32_t l = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7]; p += 8 + l; }
        uint32_t l = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
        T[i].p = p + 8; T[i].end = p + 8 + l; p += 8 + l;
    }
    // Pass 1: tempo map.
    size_t ntp = 0, captp = 1024; tempo_pt *tp = malloc(captp * sizeof(tempo_pt));
    tp[ntp++] = (tempo_pt){0, 500000};
    for (int i = 0; i < ntrk; ++i) {
        trk t = T[i]; t.tick = 0; trk_advance(&t);
        while (!t.done) { uint32_t tempo = 0; trk_event(&t, &tempo);
            if (tempo) { if (ntp == captp) tp = realloc(tp, (captp *= 2) * sizeof(tempo_pt)); tp[ntp++] = (tempo_pt){t.tick, tempo}; }
            if (!t.done) trk_advance(&t); }
    }
    for (size_t a = 1; a < ntp; ++a) for (size_t b = a; b > 0 && tp[b - 1].tick > tp[b].tick; --b) { tempo_pt x = tp[b]; tp[b] = tp[b - 1]; tp[b - 1] = x; }
    for (int i = 0; i < ntrk; ++i) { T[i].tick = 0; trk_advance(&T[i]); }

    if (!ssw_init_ex(SR, 2, 32, BLOCK, 16, 0, getenv("VOICES") ? atoi(getenv("VOICES")) : 8192, 0, workers, 0, 1, 1, 0, 1)) { puts("init fail"); return 1; }
    if (ssw_load_sf2(sf2) <= 0) { puts("sf2 fail"); return 1; }
    ssw_warmup();
    int dbg = getenv("DBG") != NULL;
    const double pre = start > 2.0 ? start - 2.0 : 0.0;
    ssw_set_song_time(pre);

    size_t cap = 1 << 20, cnt = 0; uint32_t *mb = malloc(cap * 4); double *tb = malloc(cap * 8);
    size_t ti = 0; double tsec = 0; uint64_t tlast = 0; uint32_t cur = tp[0].us;
    float *buf = calloc(BLOCK * 2, sizeof(float));
    FILE *wf = argc > 6 ? fopen(argv[6], "wb") : NULL; if (wf) fseek(wf, 44, SEEK_SET);
    long frames_out = 0; double end_s = start + dur;
    double sec_wall = 0, sec_cpu = 0, sec_alloc = 0, sec_busy = 0, sec_max = 0; int sec_blocks = 0; long sec_notes = 0, notes_pending = 0;
    int steals0 = ssw_steals(), drops0 = ssw_dropped_notes(); double sec_mark = start;
    double tot_wall = 0; int tot_blocks = 0;
    if (dbg) ssw_set_debug_metrics(1);
    for (double bt = pre; bt < end_s; bt += (double)BLOCK / SR) {
        double bend = bt + (double)BLOCK / SR;
        cnt = 0;
        for (;;) {
            int best = -1;
            for (int i = 0; i < ntrk; ++i) if (!T[i].done && (best < 0 || T[i].tick < T[best].tick)) best = i;
            if (best < 0) break;
            uint64_t tk = T[best].tick;
            while (ti + 1 < ntp && tp[ti + 1].tick <= tk) { tsec += (double)(tp[ti + 1].tick - tlast) * cur / 1e6 / div; tlast = tp[ti + 1].tick; cur = tp[++ti].us; }
            double s = tsec + (double)(tk - tlast) * cur / 1e6 / div;
            if (s >= bend) break;
            uint32_t tempo = 0, m = trk_event(&T[best], &tempo);
            if (!T[best].done) trk_advance(&T[best]);
            if (!m) continue;
            uint32_t cmd = m & 0xF0;
            if (s < pre && (cmd == 0x80 || cmd == 0x90)) continue;
            if (cnt == cap) { cap *= 2; mb = realloc(mb, cap * 4); tb = realloc(tb, cap * 8); }
            mb[cnt] = m; tb[cnt] = s < pre ? pre : s; ++cnt;
            if (cmd == 0x90 && (m >> 16)) ++notes_pending;
        }
        double w0 = now(CLOCK_MONOTONIC), c0 = now(CLOCK_PROCESS_CPUTIME_ID);
        if (cnt) ssw_queue_events(mb, tb, (int)cnt);
        ssw_render_queued_into((uintptr_t)buf, BLOCK);
        double w = now(CLOCK_MONOTONIC) - w0, c = now(CLOCK_PROCESS_CPUTIME_ID) - c0;
        if (wf) for (int k = 0; k < BLOCK * 2; ++k) { float v = buf[k]; v = v > 1 ? 1 : v < -1 ? -1 : v; int16_t q = (int16_t)(v * 32767); fwrite(&q, 2, 1, wf); }
        frames_out += BLOCK;
        if (bt < start) { notes_pending = 0; continue; }
        sec_wall += w; sec_cpu += c; if (w > sec_max) sec_max = w; ++sec_blocks; tot_wall += w; ++tot_blocks;
        sec_notes += notes_pending; notes_pending = 0;
        if (dbg) { sec_alloc += ssw_alloc_us() / 1000.0; sec_busy += ssw_worker_busy_us() / 1000.0; }
        if (bend >= sec_mark + 1.0 || bend >= end_s) {
            double span = bend - sec_mark;
            printf("t=%6.1fs NPS %8.0f | block avg %6.2f max %6.2f ms (budget 11.61) cpu %6.2f", bend, sec_notes / span,
                   sec_wall / sec_blocks, sec_max, sec_cpu / sec_blocks);
            if (dbg) printf(" ALLOC %6.2f BUSY %6.2f", sec_alloc / sec_blocks, sec_busy / sec_blocks);
            printf(" | act %4d free %4d steals/s %6.0f drops/s %6.0f\n", ssw_active_voices(), ssw_free_voices(),
                   (ssw_steals() - steals0) / span, (ssw_dropped_notes() - drops0) / span);
            fflush(stdout);
            steals0 = ssw_steals(); drops0 = ssw_dropped_notes(); sec_mark = bend;
            sec_wall = sec_cpu = sec_alloc = sec_busy = sec_max = 0; sec_blocks = 0; sec_notes = 0;
        }
    }

    printf("workers=%d  avg block %.2f ms over %.1fs  => %.2fx realtime cost\n", ssw_worker_count(),
           tot_wall / tot_blocks, dur, (tot_wall / tot_blocks) / (BLOCK * 1000.0 / SR));
    if (wf) {
        uint32_t data = (uint32_t)frames_out * 4, riff = data + 36, fs = 16, sr = SR, br = SR * 4; uint16_t fmt = 1, chn = 2, ba = 4, bps = 16;
        fseek(wf, 0, SEEK_SET); fwrite("RIFF", 1, 4, wf); fwrite(&riff, 4, 1, wf); fwrite("WAVEfmt ", 1, 8, wf);
        fwrite(&fs, 4, 1, wf); fwrite(&fmt, 2, 1, wf); fwrite(&chn, 2, 1, wf); fwrite(&sr, 4, 1, wf); fwrite(&br, 4, 1, wf);
        fwrite(&ba, 2, 1, wf); fwrite(&bps, 2, 1, wf); fwrite("data", 1, 4, wf); fwrite(&data, 4, 1, wf); fclose(wf);
    }
    return 0;
}
