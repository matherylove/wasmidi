import fs from 'fs';
const src = fs.readFileSync(new URL('../../web/synth-feeder-worker.js', import.meta.url), 'utf8');
const code = src.slice(src.indexOf('// BEGIN SmfSynthSource'), src.indexOf('// END SmfSynthSource'));
const SmfSynthSource = new Function(code + '\nreturn SmfSynthSource;')();
const [file, startS, durS, floorS, outPath] = process.argv.slice(2);
const buf = fs.readFileSync(file);
const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
let t0 = performance.now();
const s = new SmfSynthSource(ab);
console.error(`load ${(performance.now() - t0).toFixed(0)} ms maxTick ${s.maxTick} sel ${s.selN} sysex ${s.sysex.length}`);
const start = +startS, dur = +durS, floor = +floorS;
const tick = s.resetTo(Math.floor(s.secondsToTick(start)));
const fd = fs.openSync(outPath, 'w'); let total = 0;
const OB = Buffer.alloc(1 << 24); let op = 0;
const flush = () => { fs.writeSync(fd, OB, 0, op); op = 0; };
const W = (ty, t, payload) => { if (op + 13 > OB.length) flush(); OB[op] = ty; OB.writeDoubleLE(t, op + 1); OB.writeUInt32LE(payload >>> 0, op + 9); op += 13; };
const WB = (u8) => { if (op + u8.length > OB.length) flush(); OB.set(u8, op); op += u8.length; };
let target = start; t0 = performance.now(); let fillMs = 0;
while (target < start + dur) {
  target += 0.05;
  for (;;) {
    const f0 = performance.now();
    const b = s.fill(target, 262144, floor);
    fillMs += performance.now() - f0;
    for (let i = 0; i < b.sysexTimes.length; ++i) {
      const off = b.sysexMeta[i * 3 + 1], len = b.sysexMeta[i * 3 + 2];
      W(2, b.sysexTimes[i], len); WB(b.sysexData.subarray(off, off + len));
    }
    for (let i = 0; i < b.messages.length; ++i) {
      const m = b.messages[i]; const cmd = m & 0xf0, d2 = (m >>> 16) & 127;
      const count = (cmd === 0x90 && d2 !== 0) ? ((m >>> 24) & 255) + 1 : 1;
      for (let k = 0; k < count; ++k) { W(1, b.times[i], m & 0xffffff); ++total; }
    }
    if (b.complete) break;
  }
}
flush(); fs.closeSync(fd);
console.error(`js events ${total} tick ${tick} fill ${fillMs.toFixed(0)} ms`);
