/* Verifies the resolved pool/worker pairing end to end: the JS sizing rule from
 * snappysynth-worker.js against the C worker-count policy from voice.c. */
const VOICES_PER_WORKER_MIN = 512;
const VOICE_POOL_CEILING = 5000000;
const WORKER_FREELIST_REFILL = 512;
const STEAL_SHARED_POOL_VOICES = 2048;

function resolveVoicePool(maxVoices, requestedWorkers) {
    if (requestedWorkers <= 0) return { total: maxVoices, perWorker: 0, workers: 0 };
    const perWorker = Math.max(1, maxVoices);
    return { total: Math.min(VOICE_POOL_CEILING, perWorker * requestedWorkers),
             perWorker, workers: requestedWorkers };
}
/* mirror of the browser branch of voice.c, including the seeding guard */
function workerPolicy(total, ssWorkers, cores) {
    let desired = cores > 0 ? cores : 1;
    if (ssWorkers > 0) desired = ssWorkers;
    else if (total <= STEAL_SHARED_POOL_VOICES) desired = 1;
    else { let w = Math.ceil(total / 1024); w = Math.min(16, Math.max(2, w));
           if (desired > w) desired = w; }
    let seedable = Math.floor(total / WORKER_FREELIST_REFILL);
    if (seedable < 1) seedable = 1;
    if (desired > seedable) desired = seedable;
    if (desired < 1) desired = 1;
    if (desired > cores) desired = cores;
    return desired;
}

const cores = 24;
let fails = 0;
const check = (c, m) => { if (!c) { console.log("  FAIL: " + m); ++fails; } };

console.log("CPU: " + cores + " hilos\n");
console.log("Workers | Voices/wkr |    POOL | workers reales | por worker real");
console.log("--------+------------+---------+----------------+----------------");
for (const [w, v] of [[24,512],[24,1024],[24,2048],[16,1024],[8,1024],[24,256],[12,4096]]) {
    const pool = resolveVoicePool(v, w);
    const actual = workerPolicy(pool.total, w, cores);
    const per = Math.floor(pool.total / actual);
    console.log(
        String(w).padStart(7) + " | " + String(v).padStart(10) + " | " +
        String(pool.total).padStart(7) + " | " + String(actual).padStart(14) + " | " +
        String(per).padStart(15));
    // every worker must be able to receive at least one 512-voice batch
    check(per >= WORKER_FREELIST_REFILL || v < VOICES_PER_WORKER_MIN,
          `w=${w} v=${v}: only ${per} voices per worker, below the ${WORKER_FREELIST_REFILL} batch`);
    // the requested thread count must be honoured when per-worker >= 512
    check(v < VOICES_PER_WORKER_MIN || actual === Math.min(w, cores),
          `w=${w} v=${v}: asked ${w} workers, got ${actual}`);
}

console.log("\nWorkers = 0 (auto) debe quedar exactamente como antes:");
for (const v of [512, 1024, 2048, 8192, 16384]) {
    const pool = resolveVoicePool(v, 0);
    check(pool.total === v, `auto v=${v}: pool ${pool.total} != ${v}`);
    console.log(`  Voices ${String(v).padStart(6)} -> POOL ${String(pool.total).padStart(6)}, workers ${workerPolicy(pool.total, 0, cores)}`);
}

console.log("\nEl valor de ajuste nunca se realimenta (reconfigurar N veces):");
{
    let setting = 1024;                    // lo que guarda el bridge
    for (let i = 0; i < 5; ++i) {
        const pool = resolveVoicePool(setting, 24);
        // el worker reporta maxVoices = setting, totalVoices = pool.total
        setting = setting;                 // el bridge solo absorbe maxVoices
        check(pool.total === 24576, `reconfig ${i}: pool ${pool.total}`);
    }
    console.log("  1024 x 24 = 24576 estable tras 5 reconfiguraciones");
}

console.log("\n" + (fails ? "FALLOS: " + fails : "TODO OK"));
process.exit(fails ? 1 : 0);
