/*
 * midi_parser.c — WASM MIDI Parser for MPWGL2
 *
 * Compiled with:
 *   emcc midi_parser.c -O3 -o midi_parser.wasm \
 *     -s WASM=1 -s EXPORTED_FUNCTIONS='["_midi_parse","_get_result_ptr","_get_note_count","_get_cc_count","_malloc","_free"]' \
 *     -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]' \
 *     -s ALLOW_MEMORY_GROWTH=1 \
 *     -s INITIAL_MEMORY=67108864 \
 *     --no-entry
 *
 * Or with wasi-sdk / clang direct to wasm32:
 *   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry \
 *     -Wl,--export=midi_parse \
 *     -Wl,--export=get_result_ptr \
 *     -Wl,--export=get_note_count \
 *     -Wl,--export=get_cc_count \
 *     -Wl,--export=malloc \
 *     -Wl,--export=free \
 *     -o midi_parser.wasm midi_parser.c
 *
 * Memory layout (all managed by WASM linear memory):
 *   - Input:  caller writes raw MIDI bytes at address returned by malloc(fileSize)
 *   - Output: allNotes  = Float32Array stride-6 [start,end,note,ch,vel/127,trackIdx]
 *             ccEvents  = struct array [time(f32), type(u8), ch(u8), d1(u8), d2(u8)]
 *   - Returned via get_result_ptr() → pointer to ResultHeader
 *
 * ResultHeader layout (bytes):
 *   0..3   uint32  noteCount
 *   4..7   uint32  ccCount
 *   8..11  uint32  fmt
 *   12..15 uint32  numTracks
 *   16..19 uint32  ppq
 *   20..23 float   fileDuration
 *   24..27 uint32  notes_ptr   (pointer to Float32[noteCount*6] sorted by startTime)
 *   28..31 uint32  cc_ptr      (pointer to CC events packed array)
 *   32..35 uint32  tempoMap_ptr
 *   36..39 uint32  tempoMap_count
 *   40..43 uint32  activeChannelMasks_ptr  (uint32[numTracks])
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ─── WASM exports ─── */
#ifdef __wasm__
#  define EXPORT __attribute__((visibility("default")))
#else
#  define EXPORT
#endif

/* ─── Constants ─── */
#define MAX_TEMPO_CHANGES   4096
#define NOTES_INIT_CAP      524288
#define CC_CAP              262144
#define MAX_TRACKS          512
#define PENDING_HASH_SIZE   65536   /* must be power of 2 */
#define PENDING_HASH_MASK   (PENDING_HASH_SIZE - 1)

/* ─── Structures ─── */
typedef struct {
    uint32_t tick;
    uint32_t uspb;   /* microseconds per beat */
} TempoEntry;

typedef struct {
    uint32_t key;    /* (trackIdx<<11)|(ch<<7)|note */
    uint32_t tick;
    uint8_t  vel;
    uint8_t  used;
    uint8_t  _pad[2];
    uint32_t next;   /* linked list for collision, 0xFFFFFFFF = end */
} PendingSlot;

typedef struct {
    float    time;
    uint8_t  type;
    uint8_t  ch;
    uint8_t  d1;
    uint8_t  d2;
} CCEvent;

typedef struct {
    uint32_t noteCount;
    uint32_t ccCount;
    uint32_t fmt;
    uint32_t numTracks;
    uint32_t ppq;
    float    fileDuration;
    uint32_t notes_ptr;
    uint32_t cc_ptr;
    uint32_t tempoMap_ptr;
    uint32_t tempoMap_count;
    uint32_t activeChannelMasks_ptr;
} ResultHeader;

/* ─── Globals (reset per call) ─── */
static TempoEntry   g_tempoMap[MAX_TEMPO_CHANGES];
static int          g_tempoCount;

/* Precomputed tempo index (double precision) */
static double g_tk[MAX_TEMPO_CHANGES];  /* tick */
static double g_sc[MAX_TEMPO_CHANGES];  /* seconds */
static double g_us[MAX_TEMPO_CHANGES];  /* uspb */
static int    g_tmN;

/* Note buffer */
static float   *g_notes      = NULL;
static uint32_t g_notesCap   = 0;
static uint32_t g_notePos    = 0;

/* CC buffer */
static CCEvent  *g_cc        = NULL;
static uint32_t  g_ccCount   = 0;

/* Pending NoteOn hash table */
static PendingSlot *g_pending  = NULL;
static uint32_t    *g_hashHead = NULL;  /* PENDING_HASH_SIZE entries */
static uint32_t     g_pendingCap;
static uint32_t     g_pendingSize;

/* Result header */
static ResultHeader g_result;

/* Track active channel masks */
static uint32_t g_activeChMasks[MAX_TRACKS];

/* ─── varlen decode ─── */
static inline uint32_t varlen(const uint8_t *d, uint32_t *pos) {
    uint32_t v = 0;
    uint8_t  b;
    do {
        b = d[(*pos)++];
        v = (v << 7) | (b & 0x7F);
    } while (b & 0x80);
    return v;
}

/* ─── Big-endian reads ─── */
static inline uint32_t r32(const uint8_t *d, uint32_t *pos) {
    uint32_t v = ((uint32_t)d[*pos]<<24)|((uint32_t)d[*pos+1]<<16)
               |((uint32_t)d[*pos+2]<<8)|(uint32_t)d[*pos+3];
    *pos += 4; return v;
}
static inline uint16_t r16(const uint8_t *d, uint32_t *pos) {
    uint16_t v = (uint16_t)(d[*pos]<<8) | d[*pos+1];
    *pos += 2; return v;
}

/* ─── tickToSec (binary search in tempo index) ─── */
static inline double tickToSec(uint32_t tick) {
    int lo = 0, hi = g_tmN - 1;
    while (lo < hi) {
        int m = (lo + hi + 1) >> 1;
        if (g_tk[m] <= (double)tick) lo = m; else hi = m - 1;
    }
    return g_sc[lo] + ((double)tick - g_tk[lo]) / (double)g_ppq_global * (g_us[lo] / 1e6);
}
static uint32_t g_ppq_global = 480;

/* ─── Note buffer growth ─── */
static int grow_notes(void) {
    uint32_t newCap = g_notesCap * 2;
    float *n = (float *)realloc(g_notes, (size_t)newCap * 6 * sizeof(float));
    if (!n) return 0;
    g_notes = n; g_notesCap = newCap;
    return 1;
}

/* ─── Emit a note ─── */
static void emit_note(double s, double e, uint8_t note, uint8_t ch, uint8_t vel, uint16_t trackIdx) {
    if (e <= s) e = s + 0.015;
    if (g_notePos >= g_notesCap && !grow_notes()) return;
    uint32_t b = g_notePos * 6;
    g_notes[b+0] = (float)s;
    g_notes[b+1] = (float)e;
    g_notes[b+2] = (float)note;
    g_notes[b+3] = (float)ch;
    g_notes[b+4] = (float)vel / 127.0f;
    g_notes[b+5] = (float)trackIdx;
    g_notePos++;
}

/* ─── Pending hash helpers ─── */
static void pending_init(uint32_t cap) {
    g_pendingCap  = cap;
    g_pendingSize = 0;
    g_pending     = (PendingSlot *)malloc(cap * sizeof(PendingSlot));
    g_hashHead    = (uint32_t *)malloc(PENDING_HASH_SIZE * sizeof(uint32_t));
    memset(g_hashHead, 0xFF, PENDING_HASH_SIZE * sizeof(uint32_t));
}
static void pending_free(void) {
    free(g_pending);  g_pending  = NULL;
    free(g_hashHead); g_hashHead = NULL;
    g_pendingSize = 0;
}

static void pending_push(uint32_t key, uint32_t tick, uint8_t vel) {
    /* grow if needed */
    if (g_pendingSize >= g_pendingCap) {
        uint32_t newCap = g_pendingCap * 2;
        PendingSlot *np = (PendingSlot *)realloc(g_pending, newCap * sizeof(PendingSlot));
        if (!np) return;
        g_pending = np; g_pendingCap = newCap;
    }
    uint32_t idx = g_pendingSize++;
    g_pending[idx].key  = key;
    g_pending[idx].tick = tick;
    g_pending[idx].vel  = vel;
    g_pending[idx].used = 1;
    uint32_t h = key & PENDING_HASH_MASK;
    g_pending[idx].next = g_hashHead[h];
    g_hashHead[h] = idx;
}

/* Returns 1 if found, fills out tick+vel, removes entry */
static int pending_pop(uint32_t key, uint32_t *out_tick, uint8_t *out_vel) {
    uint32_t h = key & PENDING_HASH_MASK;
    uint32_t *pp = &g_hashHead[h];
    uint32_t  idx = *pp;
    while (idx != 0xFFFFFFFF) {
        PendingSlot *s = &g_pending[idx];
        if (s->used && s->key == key) {
            *out_tick = s->tick;
            *out_vel  = s->vel;
            s->used   = 0;
            *pp       = s->next;  /* remove from chain */
            return 1;
        }
        pp  = &s->next;
        idx = s->next;
    }
    return 0;
}

/* Flush all remaining pending (note-on without matching note-off) at endSec */
static void pending_flush_all(double endSec) {
    for (uint32_t i = 0; i < g_pendingSize; i++) {
        PendingSlot *s = &g_pending[i];
        if (!s->used) continue;
        uint32_t key   = s->key;
        uint8_t  note  = (uint8_t)(key & 0x7F);
        uint8_t  ch    = (uint8_t)((key >> 7) & 0x0F);
        uint16_t track = (uint16_t)((key >> 11) & 0x1FF);
        double   sSec  = tickToSec(s->tick);
        emit_note(sSec, endSec, note, ch, s->vel, track);
        s->used = 0;
    }
}

/* ─── Radix + native sort (same algorithm as JS worker) ─── */
static int cmp_float_idx(const void *a, const void *b) {
    uint32_t ia = *(const uint32_t *)a;
    uint32_t ib = *(const uint32_t *)b;
    float    fa = g_notes[ia * 6];
    float    fb = g_notes[ib * 6];
    if (fa < fb) return -1;
    if (fa > fb) return  1;
    return 0;
}

static float *sort_notes(uint32_t N) {
    if (N == 0) return g_notes;

    /* find maxSec */
    float maxSec = 0.0f;
    for (uint32_t i = 0; i < N; i++) {
        float s = g_notes[i * 6];
        if (s > maxSec) maxSec = s;
    }
    uint32_t B = (uint32_t)(maxSec) + 2;
    if (B > 65536) B = 65536;

    uint32_t *cnt    = (uint32_t *)calloc(B,   sizeof(uint32_t));
    uint32_t *bstart = (uint32_t *)calloc(B,   sizeof(uint32_t));
    uint32_t *idx    = (uint32_t *)malloc (N * sizeof(uint32_t));
    if (!cnt || !bstart || !idx) { free(cnt); free(bstart); free(idx); return g_notes; }

    /* count */
    for (uint32_t i = 0; i < N; i++) {
        uint32_t b = (uint32_t)(g_notes[i * 6]);
        if (b >= B) b = B - 1;
        cnt[b]++;
    }
    /* prefix sums */
    for (uint32_t b = 1; b < B; b++) bstart[b] = bstart[b-1] + cnt[b-1];

    uint32_t *pos2 = (uint32_t *)malloc(B * sizeof(uint32_t));
    if (!pos2) { free(cnt); free(bstart); free(idx); return g_notes; }
    memcpy(pos2, bstart, B * sizeof(uint32_t));

    /* scatter */
    for (uint32_t i = 0; i < N; i++) {
        uint32_t b = (uint32_t)(g_notes[i * 6]);
        if (b >= B) b = B - 1;
        idx[pos2[b]++] = i;
    }
    free(pos2);

    /* sort within each bucket (fine-grained) */
    for (uint32_t b = 0; b < B; b++) {
        uint32_t s = bstart[b], e = s + cnt[b];
        if (e - s > 1) qsort(&idx[s], e - s, sizeof(uint32_t), cmp_float_idx);
    }
    free(cnt); free(bstart);

    /* apply permutation into new buffer */
    float *sorted = (float *)malloc((size_t)N * 6 * sizeof(float));
    if (!sorted) { free(idx); return g_notes; }
    for (uint32_t i = 0; i < N; i++) {
        uint32_t si = idx[i];
        memcpy(&sorted[i*6], &g_notes[si*6], 6 * sizeof(float));
    }
    free(idx);
    free(g_notes);   /* free unsorted buffer */
    return sorted;   /* caller stores in g_notes */
}

/* ════════════════════════════════════════════════════════════
   MAIN EXPORTED FUNCTION
════════════════════════════════════════════════════════════ */

EXPORT int midi_parse(const uint8_t *data, uint32_t dataLen) {
    /* ── Reset state ── */
    g_tempoCount = 0;
    g_notePos    = 0;
    g_ccCount    = 0;
    g_ppq_global = 480;
    memset(g_activeChMasks, 0, sizeof(g_activeChMasks));

    /* ── Alloc note buffer ── */
    if (g_notes) free(g_notes);
    g_notesCap = NOTES_INIT_CAP;
    g_notes    = (float *)malloc((size_t)g_notesCap * 6 * sizeof(float));
    if (!g_notes) return -1;

    /* ── Alloc CC buffer ── */
    if (g_cc) free(g_cc);
    g_cc = (CCEvent *)malloc(CC_CAP * sizeof(CCEvent));
    if (!g_cc) { free(g_notes); g_notes = NULL; return -1; }

    /* ── Parse MIDI header ── */
    uint32_t pos = 0;
    if (dataLen < 14) return -1;
    uint32_t hdr = r32(data, &pos);
    if (hdr != 0x4D546864) return -1;  /* 'MThd' */
    r32(data, &pos);                   /* chunk size (always 6) */
    uint32_t fmt       = r16(data, &pos);
    uint32_t numTracks = r16(data, &pos);
    uint32_t ppq       = r16(data, &pos);
    if (ppq & 0x8000) return -2;       /* SMPTE not supported */
    g_ppq_global = ppq;
    if (numTracks > MAX_TRACKS) numTracks = MAX_TRACKS;

    /* ── Locate track chunks ── */
    typedef struct { const uint8_t *data; uint32_t len; } TrackRef;
    TrackRef *tracks = (TrackRef *)malloc(numTracks * sizeof(TrackRef));
    if (!tracks) return -1;
    uint32_t actualTracks = 0;
    for (uint32_t t = 0; t < numTracks; t++) {
        if (pos + 8 > dataLen) break;
        uint32_t thdr = r32(data, &pos);
        uint32_t tlen = r32(data, &pos);
        if (thdr != 0x4D54726B) { pos += tlen; continue; }  /* 'MTrk' */
        if (pos + tlen > dataLen) tlen = dataLen - pos;
        tracks[actualTracks].data = data + pos;
        tracks[actualTracks].len  = tlen;
        actualTracks++;
        pos += tlen;
    }
    numTracks = actualTracks;

    /* ════ Pass 1: build tempo map ════ */
    g_tempoMap[0].tick = 0;
    g_tempoMap[0].uspb = 500000;
    g_tempoCount = 1;

    for (uint32_t ti = 0; ti < numTracks; ti++) {
        const uint8_t *d = tracks[ti].data;
        uint32_t len2 = tracks[ti].len;
        uint32_t p = 0;
        uint32_t tick = 0;
        while (p < len2) {
            tick += varlen(d, &p);
            uint8_t st = d[p];
            if (st == 0xFF) {
                p++;
                uint8_t mt = d[p++];
                uint32_t ml = varlen(d, &p);
                if (mt == 0x51 && ml >= 3 && g_tempoCount < MAX_TEMPO_CHANGES) {
                    uint32_t uspb = ((uint32_t)d[p] << 16) | ((uint32_t)d[p+1] << 8) | d[p+2];
                    /* Avoid duplicate ticks */
                    if (g_tempoCount == 0 || g_tempoMap[g_tempoCount-1].tick != tick) {
                        g_tempoMap[g_tempoCount].tick = tick;
                        g_tempoMap[g_tempoCount].uspb = uspb;
                        g_tempoCount++;
                    } else {
                        g_tempoMap[g_tempoCount-1].uspb = uspb;
                    }
                }
                if (mt == 0x2F) { p += ml; break; }
                p += ml;
            } else {
                uint8_t hi = st >> 4;
                if (st == 0xF0 || st == 0xF7) { p++; uint32_t ml = varlen(d, &p); p += ml; }
                else if (hi >= 0x8 && hi <= 0xB) p += 3;
                else if (hi == 0xC || hi == 0xD) p += 2;
                else if (hi == 0xE) p += 3;
                else p++;
            }
        }
    }

    /* ── Sort tempo map by tick ── */
    /* Simple insertion sort (small array) */
    for (int i = 1; i < g_tempoCount; i++) {
        TempoEntry key2 = g_tempoMap[i];
        int j = i - 1;
        while (j >= 0 && g_tempoMap[j].tick > key2.tick) {
            g_tempoMap[j+1] = g_tempoMap[j]; j--;
        }
        g_tempoMap[j+1] = key2;
    }

    /* ── Build tempo index arrays ── */
    g_tmN = g_tempoCount;
    {
        double sec = 0.0, prev = 0.0;
        uint32_t uspb = 500000;
        for (int i = 0; i < g_tmN; i++) {
            sec += ((double)g_tempoMap[i].tick - prev) / (double)ppq * (uspb / 1e6);
            g_tk[i] = (double)g_tempoMap[i].tick;
            g_sc[i] = sec;
            g_us[i] = (double)g_tempoMap[i].uspb;
            prev    = (double)g_tempoMap[i].tick;
            uspb    = g_tempoMap[i].uspb;
        }
    }

    /* ════ Pass 2: parse events ════ */
    pending_init(65536);

    for (uint32_t ti = 0; ti < numTracks; ti++) {
        const uint8_t *d = tracks[ti].data;
        uint32_t len2 = tracks[ti].len;
        uint32_t p = 0, tick = 0;
        uint8_t lastSt = 0;

        while (p < len2) {
            tick += varlen(d, &p);
            uint8_t st = d[p];
            if (st >= 0x80) { lastSt = st; p++; } else { st = lastSt; }  /* running status */
            uint8_t hi = st >> 4;
            uint8_t ch = st & 0x0F;

            if (hi == 0x9) {
                /* Note On */
                uint8_t note = d[p++], vel = d[p++];
                if (ti < MAX_TRACKS) g_activeChMasks[ti] |= (1u << ch);
                if (vel > 0) {
                    uint32_t key = ((uint32_t)ti << 11) | ((uint32_t)ch << 7) | note;
                    pending_push(key, tick, vel);
                } else {
                    /* vel=0 → note off */
                    uint32_t key = ((uint32_t)ti << 11) | ((uint32_t)ch << 7) | note;
                    uint32_t onTick; uint8_t onVel;
                    if (pending_pop(key, &onTick, &onVel))
                        emit_note(tickToSec(onTick), tickToSec(tick), note, ch, onVel, (uint16_t)ti);
                }
            } else if (hi == 0x8) {
                /* Note Off */
                uint8_t note = d[p++]; p++; /* skip velocity */
                uint32_t key = ((uint32_t)ti << 11) | ((uint32_t)ch << 7) | note;
                uint32_t onTick; uint8_t onVel;
                if (pending_pop(key, &onTick, &onVel))
                    emit_note(tickToSec(onTick), tickToSec(tick), note, ch, onVel, (uint16_t)ti);
            } else if (hi == 0xB) {
                /* CC */
                uint8_t d1 = d[p++], d2 = d[p++];
                if (g_ccCount < CC_CAP) {
                    float t = (float)tickToSec(tick);
                    g_cc[g_ccCount].time = t;
                    g_cc[g_ccCount].type = 0xB;
                    g_cc[g_ccCount].ch   = ch;
                    g_cc[g_ccCount].d1   = d1;
                    g_cc[g_ccCount].d2   = d2;
                    g_ccCount++;
                }
            } else if (hi == 0xC) {
                uint8_t d1 = d[p++];
                if (g_ccCount < CC_CAP) {
                    g_cc[g_ccCount].time = (float)tickToSec(tick);
                    g_cc[g_ccCount].type = 0xC;
                    g_cc[g_ccCount].ch   = ch;
                    g_cc[g_ccCount].d1   = d1;
                    g_cc[g_ccCount].d2   = 0;
                    g_ccCount++;
                }
            } else if (hi == 0xD) {
                uint8_t d1 = d[p++];
                if (g_ccCount < CC_CAP) {
                    g_cc[g_ccCount].time = (float)tickToSec(tick);
                    g_cc[g_ccCount].type = 0xD;
                    g_cc[g_ccCount].ch   = ch;
                    g_cc[g_ccCount].d1   = d1;
                    g_cc[g_ccCount].d2   = 0;
                    g_ccCount++;
                }
            } else if (hi == 0xA) {
                p += 2; /* aftertouch, skip */
            } else if (hi == 0xE) {
                uint8_t lsb = d[p++], msb = d[p++];
                if (g_ccCount < CC_CAP) {
                    g_cc[g_ccCount].time = (float)tickToSec(tick);
                    g_cc[g_ccCount].type = 0xE;
                    g_cc[g_ccCount].ch   = ch;
                    g_cc[g_ccCount].d1   = lsb;
                    g_cc[g_ccCount].d2   = msb;
                    g_ccCount++;
                }
            } else if (st == 0xFF) {
                uint8_t mt = d[p++];
                uint32_t ml = varlen(d, &p);
                if (mt == 0x2F) {
                    /* End of track: flush remaining pending */
                    double endSec = tickToSec(tick);
                    pending_flush_all(endSec);
                    p += ml; break;
                }
                p += ml;
            } else if (st == 0xF0 || st == 0xF7) {
                lastSt = 0;
                uint32_t ml = varlen(d, &p);
                p += ml;
            }
        }
    }

    pending_free();
    free(tracks);

    /* ════ Sort notes ════ */
    uint32_t N = g_notePos;
    float *sorted = sort_notes(N);
    g_notes = sorted;  /* may be same ptr if N==0 */

    /* ── Compute file duration ── */
    float dur = 0.0f;
    for (uint32_t i = 0; i < N; i++) {
        float e = g_notes[i*6+1];
        if (e > dur) dur = e;
    }

    /* ── Build activeChannelMasks output array ── */
    uint32_t *acm = (uint32_t *)malloc(numTracks * sizeof(uint32_t));
    if (acm) memcpy(acm, g_activeChMasks, numTracks * sizeof(uint32_t));

    /* ── Build tempoMap output (packed: tick(u32)+uspb(u32) per entry) ── */
    uint32_t *tmOut = (uint32_t *)malloc((size_t)g_tempoCount * 2 * sizeof(uint32_t));
    if (tmOut) {
        for (int i = 0; i < g_tempoCount; i++) {
            tmOut[i*2+0] = g_tempoMap[i].tick;
            tmOut[i*2+1] = g_tempoMap[i].uspb;
        }
    }

    /* ── Fill ResultHeader ── */
    g_result.noteCount               = N;
    g_result.ccCount                 = g_ccCount;
    g_result.fmt                     = fmt;
    g_result.numTracks               = numTracks;
    g_result.ppq                     = ppq;
    g_result.fileDuration            = dur;
    g_result.notes_ptr               = (uint32_t)(uintptr_t)g_notes;
    g_result.cc_ptr                  = (uint32_t)(uintptr_t)g_cc;
    g_result.tempoMap_ptr            = (uint32_t)(uintptr_t)tmOut;
    g_result.tempoMap_count          = (uint32_t)g_tempoCount;
    g_result.activeChannelMasks_ptr  = (uint32_t)(uintptr_t)acm;

    return 0;  /* success */
}

EXPORT const ResultHeader *get_result_ptr(void) { return &g_result; }
EXPORT uint32_t get_note_count(void)            { return g_result.noteCount; }
EXPORT uint32_t get_cc_count(void)              { return g_result.ccCount; }
