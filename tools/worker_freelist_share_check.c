/* Fair-share refill check: simulates workers draining the global pool and
 * verifies every worker can be seeded, which the fixed 512 batch could not do. */
#include <stdio.h>
#define WORKER_FREELIST_REFILL 512
static int max_voices;
static int refill_batch(int workers){
    int share;
    if (workers < 1) workers = 1;
    share = max_voices / (workers * 2);
    if (share > WORKER_FREELIST_REFILL) share = WORKER_FREELIST_REFILL;
    if (share < 1) share = 1;
    return share;
}
/* round-robin drain until the global pool is empty; report how many workers
 * ended up with zero voices (those are the ones that silently drop notes) */
static int starved(int workers, int batch){
    int pool = max_voices, owned[256] = {0}, i, progress = 1;
    while (pool > 0 && progress) {
        progress = 0;
        for (i = 0; i < workers && pool > 0; ++i) {
            int take = batch < pool ? batch : pool;
            owned[i] += take; pool -= take; progress = 1;
        }
    }
    int dead = 0;
    for (i = 0; i < workers; ++i) if (owned[i] == 0) ++dead;
    return dead;
}
int main(void){
    int fails = 0;
    const int caps[] = {1024, 2048, 4096, 8192, 16384};
    const int wks[]  = {1, 8, 16, 24};
    printf("%-7s %-8s | %-12s %-12s | %-12s %-12s\n",
        "Voices","Workers","lote antes","muertos","lote ahora","muertos");
    printf("----------------+--------------------------+--------------------------\n");
    for (unsigned c=0;c<sizeof(caps)/sizeof(*caps);++c){
        for (unsigned w=0;w<sizeof(wks)/sizeof(*wks);++w){
            max_voices = caps[c];
            int nw = wks[w];
            int before = starved(nw, WORKER_FREELIST_REFILL);
            int batch = refill_batch(nw);
            int after = starved(nw, batch);
            printf("%-7d %-8d | %-12d %-12d | %-12d %-12d\n",
                caps[c], nw, WORKER_FREELIST_REFILL, before, batch, after);
            if (after != 0) { printf("  FAIL: %d workers starved\n", after); ++fails; }
            if (batch > WORKER_FREELIST_REFILL) { printf("  FAIL: batch grew\n"); ++fails; }
        }
    }
    printf("\n%s (%d)\n", fails?"FALLOS":"TODO OK", fails);
    return fails?1:0;
}
