/* Simulates the pump against the measured numbers: 512-frame blocks at 44.1 kHz,
 * ~7.5 ms to render one, a 48x512 ring, and the worklet draining in realtime.
 * Compares the old demand-driven loop with the sliced target-fill loop. */
const SR = 44100, BLOCK = 512, BUFS = 48;
const CAP = BLOCK * BUFS;
const BLOCK_MS = BLOCK * 1000 / SR;

function run(mode, renderMs, seconds) {
  let available = 0, now = 0, underruns = 0, produced = 0;
  let maxBurstMs = 0, pumps = 0, fillSum = 0, fillSamples = 0;
  const endMs = seconds * 1000;
  let nextDrain = BLOCK_MS;
  while (now < endMs) {
    // worklet consumes one block of realtime
    if (now >= nextDrain) {
      if (available >= BLOCK) available -= BLOCK;
      else ++underruns;
      nextDrain += BLOCK_MS;
      fillSum += available / CAP; ++fillSamples;
    }
    // pump invited whenever there is room
    const target = Math.floor(CAP * 0.75);
    const wants = mode === "old" ? available < CAP - BLOCK : available < target;
    if (wants) {
      ++pumps;
      const sliceStart = now;
      let blocks = 0;
      const cap = mode === "old" ? 16 : 64;
      while (blocks < cap) {
        if (CAP - available < BLOCK) break;
        if (mode !== "old" && blocks > 0 && available >= target) break;
        if (mode !== "old" && blocks > 0 && now - sliceStart >= 12) break;
        const cost = renderMs();
        now += cost; available += BLOCK; ++blocks; produced += BLOCK;
        // drain keeps running during a long synchronous burst
        while (now >= nextDrain) {
          if (available >= BLOCK) available -= BLOCK; else ++underruns;
          nextDrain += BLOCK_MS;
          fillSum += available / CAP; ++fillSamples;
        }
      }
      maxBurstMs = Math.max(maxBurstMs, now - sliceStart);
    } else {
      now += 1;
    }
  }
  return { underruns, maxBurstMs, pumps,
           avgFill: fillSamples ? fillSum / fillSamples : 0 };
}

// measured: 5-8.3 ms typical, occasional 12.8-13.3 ms spikes
let n = 0;
const cost = () => {
  ++n;
  return (n % 37 === 0) ? 13.0 : 5 + (n % 7) * 0.55;
};

for (const mode of ["old", "sliced"]) {
  n = 0;
  const r = run(mode, cost, 60);
  console.log(
    (mode === "old" ? "actual (demanda, tope 16)" : "nuevo (objetivo + slice)")
      .padEnd(28) +
    " underruns " + String(r.underruns).padStart(5) +
    "  rafaga max " + r.maxBurstMs.toFixed(0).padStart(4) + " ms" +
    "  ring medio " + (r.avgFill * 100).toFixed(0) + "%");
}
