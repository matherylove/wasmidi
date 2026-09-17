/* BEGIN worker freelist rebalance (host-tested by tools/worker_freelist_borrow_check.c) */
/*
 * Cross-worker sharing of free voices.
 *
 * enqueue_event() routes a note-on to g_note_worker_map[ch][key]; that worker
 * allocates from its LOCAL free stack, refills from the global CAS pool when the
 * stack is dry, and otherwise steals a SOUNDING voice it owns. free_push()
 * returns a finished voice to the stack of the worker that owned it, never to
 * the global pool, so once the pool has been drained the free voices stay
 * wherever they happened to end. A worker whose keys are busy can then sit at
 * zero while a neighbour idles with hundreds: the note is dropped (nothing to
 * steal either) or a voice that did not need to die is stolen. Both present
 * as "notes vanish with many workers" and both look like a stealer bug.
 *
 * Fix: at the start of every cycle each worker returns to the global pool
 * whatever it holds above what it can use THIS cycle. The pool is the Treiber
 * stack the engine already has (g_free_head / g_next_free), and the surplus is
 * linked into one chain first so the whole return costs a single CAS however
 * many voices go back. The hot path (free_pop, refill batch) is untouched.
 *
 * What a worker keeps is bounded by its queued events: a voice stays local
 * only if there is a pending event that could consume it in this cycle. So a
 * sounding voice is stolen only when the global pool is empty AND every free
 * voice left anywhere is reserved by a worker about to use it -- which is the
 * "no active voice is stolen while a free one exists" invariant, stated per
 * cycle. The host test checks exactly that.
 */
#ifndef WORKER_FREELIST_KEEP_MAX
#define WORKER_FREELIST_KEEP_MAX 128 /* same figure as the per-cycle pre-refill */
#endif

/* Upper bound on the note-ons this worker can admit this cycle: every event
 * still queued for it. Note-offs and CCs inflate the bound, which only makes
 * the worker keep a little more; the noteoff overflow queue holds no note-ons
 * and is not counted. */
static inline int worker_pending_event_count(const worker_data *wd) {
    int total = 0;
    if (!wd || !wd->queues) return 0;
    for (int ch = 0; ch < MIDI_CHANNEL_COUNT; ++ch) {
        const event_queue *q = &wd->queues[ch];
        total += (int)((q->head - q->tail) & CH_EVENT_QUEUE_MASK);
    }
    return total;
}

/* How many free voices this worker should keep local for the coming cycle. */
static inline int worker_freelist_keep(const worker_data *wd) {
    int keep = worker_pending_event_count(wd);
    if (keep > WORKER_FREELIST_KEEP_MAX) keep = WORKER_FREELIST_KEEP_MAX;
    return keep;
}

/* Splice a chain of vids (already linked through g_next_free, ending in -1)
 * onto the global pool with one CAS. Owner thread of the chain only. */
static inline void global_pool_push_chain(int first, int last) {
    if (first < 0 || last < 0) return;
    LONG expected;
    do {
        expected = load_relaxed_long(&g_free_head);
        g_next_free[last] = expected;
    } while (InterlockedCompareExchange(&g_free_head, (LONG)first, expected) != expected);
}

/* Return this worker's surplus free voices to the global pool. Returns how
 * many went back. Called by the worker thread at the top of its cycle, before
 * it processes events, so the stack is owner-only here. */
static int rebalance_worker_freelist(worker_data *wd) {
    if (!wd || !g_next_free) return 0;
    int have = wd->free_top + 1;
    int keep = worker_freelist_keep(wd);
    int give = have - keep;
    if (give <= 0) return 0;

    int first = -1, last = -1, moved = 0;
    while (moved < give) {
        int vid = free_pop(wd);
        if (vid < 0) break;
        if (g_steal_score) g_steal_score[vid] = FLT_MAX;
        g_next_free[vid] = (LONG)first;
        if (last < 0) last = vid;
        first = vid;
        ++moved;
    }
    if (moved > 0) {
        global_pool_push_chain(first, last);
        wd->rebalanced_local += (LONG)moved;
    }
    return moved;
}
/* END worker freelist rebalance */
