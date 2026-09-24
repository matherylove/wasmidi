// Drives the real feeder worker file over a MessagePort like the synth worker does.
import fs from 'fs';
import { MessageChannel } from 'worker_threads';
const file = process.argv[2], start = +process.argv[3], dur = +process.argv[4], outPath = process.argv[5];
const buf = fs.readFileSync(file);
globalThis.self = globalThis;
globalThis.importScripts = () => {};
globalThis.FileReaderSync = class { readAsArrayBuffer(f) { return f; } };
const posted = [];
globalThis.postMessage = (m) => posted.push(m);
new Function(fs.readFileSync(new URL('../../web/synth-feeder-worker.js', import.meta.url), 'utf8'))();
const ch = new MessageChannel();
self.onmessage({ data: { type: 'port', port: ch.port1 } });
self.onmessage({ data: { type: 'load', file: buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) } });
console.error(posted[0]);
const fd = fs.openSync(outPath, 'w'); const OB = Buffer.alloc(1 << 24); let op = 0;
const flush = () => { fs.writeSync(fd, OB, 0, op); op = 0; };
const W = (ty, t, v) => { if (op + 13 > OB.length) flush(); OB[op] = ty; OB.writeDoubleLE(t, op + 1); OB.writeUInt32LE(v >>> 0, op + 9); op += 13; };
let generation = 5, safeUntil = start, batches = 0;
ch.port2.on('message', (d) => {
  if (d.generation !== generation) return;
  ++batches;
  for (let i = 0; i < d.sysexTimes.length; ++i) {
    const off = d.sysexMeta[i * 3 + 1], len = d.sysexMeta[i * 3 + 2];
    W(2, d.sysexTimes[i], len); if (op + len > OB.length) flush(); OB.set(d.sysexData.subarray(off, off + len), op); op += len;
  }
  for (let i = 0; i < d.messages.length; ++i) {
    const m = d.messages[i], cmd = m & 0xf0, d2 = (m >>> 16) & 127;
    const c = (cmd === 0x90 && d2 !== 0) ? ((m >>> 24) & 255) + 1 : 1;
    for (let k = 0; k < c; ++k) W(1, d.times[i], m & 0xffffff);
  }
  safeUntil = Math.max(safeUntil, d.safeUntil);
  if (safeUntil < start + dur) ch.port2.postMessage({ type: 'fill', generation, target: safeUntil + 1.5, velocityFloor: 0 });
  else { flush(); fs.closeSync(fd); console.error(`batches ${batches}`); process.exit(0); }
});
ch.port2.postMessage({ type: 'fill', generation: 4, target: 99 });          // stale generation, must be ignored
ch.port2.postMessage({ type: 'reset', generation, time: start });
ch.port2.postMessage({ type: 'fill', generation, target: start + 1.5, velocityFloor: 0 });
