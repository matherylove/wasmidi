/* Host test for the worker freelist rebalance in voice.c (HANDOFF 7.1).
 *
 * The real rebalance_worker_freelist() and helpers are included from
 * tools/voice_rebalance_logic.inc (extracted by
 * tools/extract_voice_rebalance_logic.py). Everything around them -- the
 * worker_data fields it touches, the global Treiber pool, free_pop/free_push,
 * and the allocation order of alloc_voice() -- is reproduced here as the
 * engine has it.
 *
 * What is asserted:
 *   1. The pre-fix behaviour is reproduced when rebalance is skipped, so the
 *      test is known to detect the bug it guards against.
 *   2. Invariant: a sounding voice is never stolen and a note is never dropped
 *      while any worker still holds a free voice it could not use this cycle.
 *   3. Conservation: every vid is in exactly one place after every cycle.
 *   4. keep() bounds, chain integrity, one-CAS return.
 *   5. Multithreaded stress: 8 pthreads rebalance and refill concurrently
 *      against the single CAS pool; nothing is lost or duplicated.
 *
 *   python3 tools/extract_voice_rebalance_logic.py
 *   cc -O1 -Wall -pthread -o /tmp/b tools/worker_freelist_borrow_check.c && /tmp/b
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <float.h>
#include <pthread.h>

/* ---- engine surface the block depends on ---- */
typedef int32_t LONG;
#define MIDI_CHANNEL_COUNT 16
/* 16 channel queues x 2047 usable slots = 32752 events per worker, enough for
 * the 16384/2 scenario to queue a whole half-pool on one worker. */
#define CH_EVENT_QUEUE_SIZE 2048
#define CH_EVENT_QUEUE_MASK (CH_EVENT_QUEUE_SIZE - 1)

typedef struct { int type, ch, key, value; } ch_event;
typedef struct {
    ch_event events[CH_EVENT_QUEUE_SIZE];
    volatile LONG head;
    volatile LONG tail;
} event_queue;

typedef struct {
    event_queue *queues;
    int *active; int active_count; int active_cap;
    int *free_stack; int free_top; int free_cap;
    LONG steals_local;
    LONG rebalanced_local;
    LONG drops_local;
} worker_data;

static int max_voices;
static volatile LONG g_free_head = -1;
static LONG *g_next_free;
static float *g_steal_score; /* NULL: the block must tolerate no score cache */
static int g_validate_state = 0;

static inline LONG load_relaxed_long(volatile LONG *v) { return *v; }
static inline LONG InterlockedCompareExchange(volatile LONG *v, LONG x, LONG cmp) {
    return __sync_val_compare_and_swap(v, cmp, x);
}

static inline void free_reserve(worker_data *wd, int cap) {
    if (wd->free_cap >= cap) return;
    int new_cap = cap * 2;
    if (new_cap > max_voices) new_cap = max_voices;
    if (new_cap < 16) new_cap = 16;
    if (new_cap < cap) new_cap = cap;
    wd->free_stack = realloc(wd->free_stack, sizeof(int) * (size_t)new_cap);
    wd->free_cap = new_cap;
}
static inline int free_contains(const worker_data *wd, int vid) {
    for (int i = 0; i <= wd->free_top; ++i) if (wd->free_stack[i] == vid) return 1;
    return 0;
}
/* verbatim shape of voice.c free_push/free_pop */
static inline void free_push(worker_data *wd, int vid) {
    if (wd->free_top + 1 >= wd->free_cap) free_reserve(wd, wd->free_top + 2);
    if (wd->free_top + 1 < wd->free_cap && vid >= 0 && vid < max_voices &&
        (!g_validate_state || !free_contains(wd, vid))) {
        wd->free_stack[++wd->free_top] = vid;
    }
}
static inline int free_pop(worker_data *wd) {
    if (wd->free_top < 0) return -1;
    return wd->free_stack[wd->free_top--];
}

#include "voice_rebalance_logic.inc"

/* ---- rest of the allocation path, as alloc_voice() orders it ---- */
static int g_worker_freelist_refill = 512;
static void refill_worker_freelist(worker_data *wd) {
    int want = g_worker_freelist_refill, got = 0;
    { /* engine: batch capped by this worker's remaining events (+1 in flight) */
        int pending = worker_pending_event_count(wd) + 1;
        if (pending < want) want = pending;
    }
    free_reserve(wd, wd->free_top + 1 + want);
    while (got < want) {
        LONG expected = load_relaxed_long(&g_free_head);
        if (expected < 0) break;
        LONG next = load_relaxed_long(&g_next_free[expected]);
        if (InterlockedCompareExchange(&g_free_head, next, expected) != expected) continue;
        if (wd->free_top + 1 < wd->free_cap) wd->free_stack[++wd->free_top] = (int)expected;
        ++got;
    }
}
static int alloc_voice_lockfree(void) {
    LONG expected, next;
    do {
        expected = load_relaxed_long(&g_free_head);
        if (expected == -1) return -1;
        next = load_relaxed_long(&g_next_free[expected]);
    } while (InterlockedCompareExchange(&g_free_head, next, expected) != expected);
    return expected;
}
/* Returns vid >= 0 from a free source, or -1 meaning "would have to steal". */
static int alloc_voice(worker_data *wd) {
    int vid = free_pop(wd);
    if (vid < 0) {
        if (load_relaxed_long(&g_free_head) >= 0) {
            refill_worker_freelist(wd);
            vid = free_pop(wd);
        }
    }
    if (vid < 0) vid = alloc_voice_lockfree();
    return vid;
}
/* ---- harness ---- */
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static worker_data *W;
static int NW;

static void pool_init(int voices, int workers) {
    if (W) { for (int w = 0; w < NW; ++w) { free(W[w].queues); free(W[w].active); free(W[w].free_stack); } }
    max_voices = voices; NW = workers;
    free(g_next_free); g_next_free = malloc(sizeof(LONG) * (size_t)voices);
    for (int i = 0; i < voices - 1; ++i) g_next_free[i] = i + 1;
    g_next_free[voices - 1] = -1;
    g_free_head = 0;
    free(W); W = calloc((size_t)workers, sizeof(worker_data));
    for (int w = 0; w < workers; ++w) {
        W[w].queues = calloc(MIDI_CHANNEL_COUNT, sizeof(event_queue));
        W[w].active = malloc(sizeof(int) * (size_t)voices);
        W[w].free_top = -1;
    }
}
static long pool_count(void) {
    long n = 0;
    for (LONG h = g_free_head; h >= 0; h = g_next_free[h]) { ++n; if (n > max_voices) return -1; }
    return n;
}
static long worker_free_total(void) {
    long n = 0; for (int w = 0; w < NW; ++w) n += W[w].free_top + 1; return n;
}
static long active_total(void) {
    long n = 0; for (int w = 0; w < NW; ++w) n += W[w].active_count; return n;
}
/* every vid exactly once across pool / worker free stacks / active lists */
static void check_conservation(const char *where) {
    unsigned char *seen = calloc((size_t)max_voices, 1);
    for (LONG h = g_free_head; h >= 0; h = g_next_free[h]) { CHECK(!seen[h], "%s: vid %d twice (pool)", where, h); seen[h] = 1; }
    for (int w = 0; w < NW; ++w) {
        for (int i = 0; i <= W[w].free_top; ++i) { int v = W[w].free_stack[i]; CHECK(!seen[v], "%s: vid %d twice (free w%d)", where, v, w); seen[v] = 1; }
        for (int i = 0; i < W[w].active_count; ++i) { int v = W[w].active[i]; CHECK(!seen[v], "%s: vid %d twice (active w%d)", where, v, w); seen[v] = 1; }
    }
    for (int v = 0; v < max_voices; ++v) CHECK(seen[v], "%s: vid %d lost", where, v);
    free(seen);
}

/* queue n fake note-ons on worker w, spread over the 16 channel queues */
static void queue_events(int w, int n) {
    for (int i = 0; i < n; ++i) {
        event_queue *q = &W[w].queues[i % MIDI_CHANNEL_COUNT];
        q->events[q->head] = (ch_event){0, i % 16, 60, 100};
        q->head = (q->head + 1) & CH_EVENT_QUEUE_MASK;
    }
}
/* worker w consumes its queued events: each takes a voice via alloc_voice;
 * counts how many had to fall through to steal/drop */
static int consume_events(int w, int *stole_or_dropped) {
    int admitted = 0;
    for (int ch = 0; ch < MIDI_CHANNEL_COUNT; ++ch) {
        event_queue *q = &W[w].queues[ch];
        while (q->tail != q->head) {
            q->tail = (q->tail + 1) & CH_EVENT_QUEUE_MASK;
            int vid = alloc_voice(&W[w]);
            if (vid < 0) { ++*stole_or_dropped; ++W[w].drops_local; continue; }
            W[w].active[W[w].active_count++] = vid;
            ++admitted;
        }
    }
    return admitted;
}
/* worker w releases k of its active voices back to its local stack */
static void release_voices(int w, int k) {
    while (k-- > 0 && W[w].active_count > 0) {
        int vid = W[w].active[--W[w].active_count];
        free_push(&W[w], vid);
    }
}
/* One render cycle as the worker thread orders it: rebalance, then events. */
static int run_cycle(int do_rebalance, int *stole_or_dropped) {
    int admitted = 0;
    for (int w = 0; w < NW; ++w) if (do_rebalance) rebalance_worker_freelist(&W[w]);
    for (int w = 0; w < NW; ++w) admitted += consume_events(w, stole_or_dropped);
    return admitted;
}
/* ---- scenario: idle workers hoard, one busy worker starves ---- */
static void scenario_hoarder(int voices, int workers, int do_rebalance, int *out_fell_through) {
    pool_init(voices, workers);
    g_worker_freelist_refill = voices / (workers * 2);
    if (g_worker_freelist_refill < 1) g_worker_freelist_refill = 1;
    if (g_worker_freelist_refill > 512) g_worker_freelist_refill = 512;
    int bad = 0;

    /* Phase 1: every worker takes an equal share and plays it. */
    for (int w = 0; w < workers; ++w) queue_events(w, voices / workers);
    run_cycle(do_rebalance, &bad);
    CHECK(bad == 0, "phase1: %d fell through with %ld in pool", bad, pool_count());
    check_conservation("phase1");

    /* Phase 2: workers 1..N-1 release everything (chord ends) -> voices sit in
     * THEIR local stacks. Worker 0 keeps playing and receives a burst. */
    for (int w = 1; w < workers; ++w) release_voices(w, W[w].active_count);
    /* Releases go to the OWNER's local stack, never to the pool: this is the
     * hoarding the fix is about. Anything already in the pool at this point
     * is a phase-1 leftover and is not what worker 0 is starved of. */
    long idle_free = worker_free_total();
    CHECK(idle_free > 0, "phase2: idle workers hold nothing, scenario is vacuous");
    int burst = (int)((idle_free + pool_count()) / 2);
    int qcap = (CH_EVENT_QUEUE_SIZE - 1) * MIDI_CHANNEL_COUNT;
    if (burst > qcap) burst = qcap;
    queue_events(0, burst);
    int before = bad;
    run_cycle(do_rebalance, &bad);
    int fell_through = bad - before;
    check_conservation("phase2");
    *out_fell_through = fell_through;

    if (do_rebalance) {
        /* THE invariant: free voices existed on idle workers, so worker 0 must
         * not have had to steal or drop a single note. */
        CHECK(fell_through == 0,
              "invariant: %d notes stole/dropped while %ld free voices sat idle on other workers",
              fell_through, idle_free);
        /* and what idle workers kept is bounded by their (zero) pending events */
        for (int w = 1; w < workers; ++w)
            CHECK(W[w].free_top + 1 <= worker_freelist_keep(&W[w]),
                  "worker %d kept %d free with %d pending", w, W[w].free_top + 1,
                  worker_pending_event_count(&W[w]));
    }
    CHECK(worker_free_total() + active_total() + pool_count() == voices, "conservation total");
}

/* ---- multithreaded stress on the single CAS pool ---- */
#define STRESS_ITERS 20000
static void *stress_thread(void *arg) {
    int w = (int)(intptr_t)arg;
    unsigned seed = 12345u + (unsigned)w;
    for (int it = 0; it < STRESS_ITERS; ++it) {
        seed = seed * 1103515245u + 12345u;
        int n = (int)((seed >> 16) % 8);
        for (int i = 0; i < n; ++i) { int v = alloc_voice(&W[w]); if (v >= 0) W[w].active[W[w].active_count++] = v; }
        seed = seed * 1103515245u + 12345u;
        int r = (int)((seed >> 16) % 8);
        while (r-- > 0 && W[w].active_count > 0) free_push(&W[w], W[w].active[--W[w].active_count]);
        /* pending events vary so keep() varies */
        seed = seed * 1103515245u + 12345u;
        W[w].queues[0].head = (LONG)((seed >> 16) % CH_EVENT_QUEUE_SIZE);
        W[w].queues[0].tail = 0;
        rebalance_worker_freelist(&W[w]);
    }
    return NULL;
}
int main(void) {
    printf("worker freelist rebalance -- host check\n\n");

    printf("1. bug reproduction (rebalance OFF): starving worker must steal/drop\n");
    {
        int bad = 0;
        scenario_hoarder(4096, 8, 0, &bad);
        printf("   4096 voices / 8 workers, no rebalance: %d note-ons fell through to steal/drop\n", bad);
        CHECK(bad > 0, "test cannot see the bug it guards against");
    }

    printf("2. invariant (rebalance ON): no steal while any worker has an unusable free voice\n");
    {
        const int caps[] = {1024, 2048, 4096, 8192, 16384};
        const int wks[] = {2, 8, 16, 24};
        for (unsigned c = 0; c < sizeof caps / sizeof *caps; ++c)
            for (unsigned k = 0; k < sizeof wks / sizeof *wks; ++k) {
                int bad = -1, before = fails;
                scenario_hoarder(caps[c], wks[k], 1, &bad);
                printf("   %-6d voices %-3d workers: fell through %d %s\n", caps[c], wks[k], bad,
                       fails == before ? "ok" : "FAIL");
            }
    }

    printf("3. keep() is bounded by pending events and by WORKER_FREELIST_KEEP_MAX\n");
    {
        pool_init(1024, 2);
        queue_events(0, 5);
        CHECK(worker_freelist_keep(&W[0]) == 5, "keep with 5 pending = %d", worker_freelist_keep(&W[0]));
        CHECK(worker_freelist_keep(&W[1]) == 0, "keep with 0 pending = %d", worker_freelist_keep(&W[1]));
        queue_events(0, 900);
        CHECK(worker_freelist_keep(&W[0]) == WORKER_FREELIST_KEEP_MAX, "keep cap = %d", worker_freelist_keep(&W[0]));
        while (alloc_voice_lockfree() >= 0) {} /* drain pool so vids are unique below */
        for (int i = 0; i < 300; ++i) free_push(&W[1], i);
        queue_events(1, 10);
        int moved = rebalance_worker_freelist(&W[1]);
        CHECK(moved == 290, "moved %d, expected 290", moved);
        CHECK(W[1].free_top + 1 == 10, "kept %d, expected 10", W[1].free_top + 1);
        CHECK(W[1].rebalanced_local == 290, "rebalanced_local %d", (int)W[1].rebalanced_local);
        moved = rebalance_worker_freelist(&W[1]);
        CHECK(moved == 0, "second pass moved %d (should be a no-op)", moved);
    }

    printf("4. returned chain is intact; refill sees every voice again\n");
    {
        pool_init(64, 1);
        while (alloc_voice_lockfree() >= 0) {}
        for (int i = 0; i < 64; ++i) free_push(&W[0], i);
        CHECK(pool_count() == 0, "pool not drained");
        int moved = rebalance_worker_freelist(&W[0]);
        CHECK(moved == 64, "moved %d", moved);
        CHECK(pool_count() == 64, "pool has %ld after return", pool_count());
        check_conservation("chain");
        g_worker_freelist_refill = 512;
        /* refill is bounded by pending events (+1 in flight): with none queued
         * it takes exactly one; with 63 queued it takes the whole chain back */
        refill_worker_freelist(&W[0]);
        CHECK(W[0].free_top + 1 == 1, "refill with no events got %d, expected 1", W[0].free_top + 1);
        queue_events(0, 63);
        refill_worker_freelist(&W[0]);
        CHECK(W[0].free_top + 1 == 64, "refill got %d", W[0].free_top + 1);
        CHECK(pool_count() == 0, "pool after refill %ld", pool_count());
        check_conservation("refill");
    }

    printf("5. multithreaded stress: %d iterations x 8 threads against one CAS pool\n", STRESS_ITERS);
    {
        pool_init(8192, 8);
        g_worker_freelist_refill = 512;
        pthread_t th[8];
        for (int w = 0; w < 8; ++w) pthread_create(&th[w], NULL, stress_thread, (void *)(intptr_t)w);
        for (int w = 0; w < 8; ++w) pthread_join(th[w], NULL);
        for (int w = 0; w < 8; ++w) W[w].queues[0].head = W[w].queues[0].tail = 0;
        check_conservation("stress");
        long total = worker_free_total() + active_total() + pool_count();
        CHECK(total == 8192, "stress conservation: %ld", total);
        long reb = 0; for (int w = 0; w < 8; ++w) reb += W[w].rebalanced_local;
        printf("   rebalanced total %ld, pool now %ld, worker-free %ld, active %ld\n",
               reb, pool_count(), worker_free_total(), active_total());
        CHECK(reb > 0, "stress never rebalanced");
    }

    printf("\n%s (%d)\n", fails ? "FALLOS" : "TODO OK", fails);
    return fails ? 1 : 0;
}
