"use strict";
// Dedicated MIDI event source for SnappySynth (HANDOFF §26). Talks to the synth
// worker over a MessagePort; never shares a thread with visual prefetch.

// BEGIN SmfSynthSource
class SmfSynthSource {
    constructor(buffer) {
        const u8 = new Uint8Array(buffer);
        this.u8 = u8;
        const fail = (msg) => { throw new Error(msg); };
        const rd32 = (p) => ((u8[p] << 24) | (u8[p + 1] << 16) | (u8[p + 2] << 8) | u8[p + 3]) >>> 0;
        if (u8.length < 14 || u8[0] !== 0x4d || u8[1] !== 0x54 || u8[2] !== 0x68 || u8[3] !== 0x64)
            fail("Invalid MIDI header");
        const hlen = rd32(4);
        if (hlen < 6 || 8 + hlen > u8.length) fail("Invalid MIDI header");
        const format = (u8[8] << 8) | u8[9];
        const ntrk = (u8[10] << 8) | u8[11];
        const div = (u8[12] << 8) | u8[13];
        if (format > 1) fail("Only MIDI Format 0 and 1 are supported");
        if (div === 0 || (div & 0x8000)) fail("SMPTE timing is not supported");
        this.ppq = div;
        let p = 8 + hlen;
        const tStart = new Uint32Array(ntrk), tEnd = new Uint32Array(ntrk);
        for (let t = 0; t < ntrk; ++t) {
            if (p + 8 > u8.length || u8[p] !== 0x4d || u8[p + 1] !== 0x54 || u8[p + 2] !== 0x72 || u8[p + 3] !== 0x6b)
                fail("Invalid or truncated MTrk chunk");
            const len = rd32(p + 4);
            if (p + 8 + len > u8.length) fail("Truncated MIDI track");
            tStart[t] = p + 8; tEnd[t] = p + 8 + len; p += 8 + len;
        }
        this.ntrk = ntrk; this.tStart = tStart; this.tEnd = tEnd;

        const tempo = [{ tick: 0, us: 500000 }];
        let sel = new Uint32Array(4 * 4096), selN = 0;
        const sysex = [];
        this.cp = [];
        this.trackMaxTick = new Uint32Array(ntrk);
        let maxTick = 0;
        for (let t = 0; t < ntrk; ++t) {
            let q = tStart[t];
            const e = tEnd[t];
            let tick = 0, run = 0, order = 0, n = 0;
            let cpP = new Uint32Array(64), cpT = new Uint32Array(64), cpR = new Uint8Array(64), cpN = 0;
            while (q < e) {
                if ((n & 1023) === 0) {
                    if (cpN === cpP.length) {
                        const np = new Uint32Array(cpN * 2); np.set(cpP); cpP = np;
                        const nt = new Uint32Array(cpN * 2); nt.set(cpT); cpT = nt;
                        const nr = new Uint8Array(cpN * 2); nr.set(cpR); cpR = nr;
                    }
                    cpP[cpN] = q; cpT[cpN] = tick; cpR[cpN] = run; ++cpN;
                }
                let d = 0, c;
                do { if (q >= e) fail("Malformed MIDI event data"); c = u8[q++]; d = d * 128 + (c & 127); } while (c & 128);
                if (d > 0xffffffff - tick) fail("Malformed MIDI event data");
                tick += d;
                if (q >= e) fail("Malformed MIDI event data");
                let s = u8[q], hasFirst = false, first = 0;
                if (s < 0x80) {
                    if (!run) fail("Malformed MIDI event data");
                    hasFirst = true; first = s; s = run; ++q;
                } else {
                    ++q;
                    if (s < 0xf0) run = s;
                    else if (s < 0xf8 || s === 0xff) run = 0;
                }
                ++n;
                if (s === 0xff) {
                    if (q >= e) fail("Malformed MIDI event data");
                    const meta = u8[q++];
                    let len = 0;
                    do { if (q >= e) fail("Malformed MIDI event data"); c = u8[q++]; len = len * 128 + (c & 127); } while (c & 128);
                    if (q + len > e) fail("Malformed MIDI event data");
                    if (meta === 0x51 && len >= 3)
                        tempo.push({ tick, us: ((u8[q] << 16) | (u8[q + 1] << 8) | u8[q + 2]) >>> 0 });
                    q += len;
                    if (meta === 0x2f) break;
                    ++order;
                    continue;
                }
                if (s === 0xf0 || s === 0xf7) {
                    let len = 0;
                    do { if (q >= e) fail("Malformed MIDI event data"); c = u8[q++]; len = len * 128 + (c & 127); } while (c & 128);
                    if (q + len > e) fail("Malformed MIDI event data");
                    const data = new Uint8Array(len + 1);
                    data[0] = s; data.set(u8.subarray(q, q + len), 1);
                    sysex.push({ tick, track: t, order, data });
                    q += len; ++order;
                    continue;
                }
                if (s >= 0xf0) {
                    q += s === 0xf2 ? 2 : (s === 0xf1 || s === 0xf3) ? 1 : 0;
                    if (q > e) fail("Malformed MIDI event data");
                    ++order;
                    continue;
                }
                const cmd = s & 0xf0;
                let d1 = first;
                if (!hasFirst) { if (q >= e) fail("Malformed MIDI event data"); d1 = u8[q++]; }
                let d2 = 0;
                if (cmd !== 0xc0 && cmd !== 0xd0) { if (q >= e) fail("Malformed MIDI event data"); d2 = u8[q++]; }
                if (cmd === 0xc0 || cmd === 0xe0 || (cmd === 0xb0 && (d1 === 0 || d1 === 32))) {
                    if (selN * 4 === sel.length) { const ns = new Uint32Array(sel.length * 2); ns.set(sel); sel = ns; }
                    sel[selN * 4] = tick; sel[selN * 4 + 1] = t; sel[selN * 4 + 2] = order;
                    sel[selN * 4 + 3] = (s | (d1 << 8) | (d2 << 16)) >>> 0;
                    ++selN;
                }
                ++order;
            }
            this.cp.push({ p: cpP.subarray(0, cpN), t: cpT.subarray(0, cpN), r: cpR.subarray(0, cpN) });
            this.trackMaxTick[t] = tick;
            if (tick > maxTick) maxTick = tick;
        }
        this.maxTick = maxTick;

        const stable = tempo.map((v, i) => ({ v, i })).sort((a, b) => a.v.tick - b.v.tick || a.i - b.i).map(x => x.v);
        const dedup = [];
        for (const tc of stable) {
            if (dedup.length && dedup[dedup.length - 1].tick === tc.tick) dedup[dedup.length - 1] = tc;
            else dedup.push(tc);
        }
        if (!dedup.length || dedup[0].tick !== 0) dedup.unshift({ tick: 0, us: 500000 });
        this.tempoTick = new Float64Array(dedup.length);
        this.tempoUs = new Float64Array(dedup.length);
        this.tempoSec = new Float64Array(dedup.length);
        let sec = 0, prevTick = 0, prevUs = 500000;
        for (let i = 0; i < dedup.length; ++i) {
            sec += (dedup[i].tick - prevTick) / this.ppq * (prevUs / 1000000);
            this.tempoTick[i] = dedup[i].tick; this.tempoUs[i] = dedup[i].us; this.tempoSec[i] = sec;
            prevTick = dedup[i].tick;
            if (dedup[i].us) prevUs = dedup[i].us;
        }

        sysex.sort((a, b) => a.tick - b.tick || a.track - b.track || a.order - b.order);
        this.sysex = sysex;
        const idx = new Uint32Array(selN);
        for (let i = 0; i < selN; ++i) idx[i] = i;
        const S = sel;
        const sorted = Array.from(idx).sort((a, b) =>
            (S[a * 4] - S[b * 4]) || (S[a * 4 + 1] - S[b * 4 + 1]) || (S[a * 4 + 2] - S[b * 4 + 2]));
        this.sel = new Uint32Array(selN * 4);
        for (let i = 0; i < selN; ++i) this.sel.set(S.subarray(sorted[i] * 4, sorted[i] * 4 + 4), i * 4);
        this.selN = selN;

        this.curP = new Uint32Array(ntrk); this.curTick = new Float64Array(ntrk);
        this.curRun = new Uint8Array(ntrk); this.curDone = new Uint8Array(ntrk);
        this.evTick = new Float64Array(ntrk); this.evMsg = new Uint32Array(ntrk);
        this.heap = new Int32Array(ntrk); this.heapN = 0;
        this.sysexCursor = 0;
        this.pendingSelectors = null; this.pendingHistorySysex = null; this.historyTime = 0;
        this.resetTo(0);
    }

    tickToSeconds(tick) {
        const T = this.tempoTick;
        let lo = 0, hi = T.length;
        while (lo < hi) { const m = (lo + hi) >> 1; if (tick < T[m]) hi = m; else lo = m + 1; }
        const i = Math.max(0, Math.min(lo - 1, this.tempoSec.length - 1));
        const us = this.tempoUs[i] || 500000;
        return this.tempoSec[i] + (tick - T[i]) / this.ppq * (us / 1000000);
    }

    secondsToTick(seconds) {
        if (seconds <= 0) return 0;
        const S = this.tempoSec;
        let lo = 0, hi = S.length;
        while (lo < hi) { const m = (lo + hi) >> 1; if (seconds < S[m]) hi = m; else lo = m + 1; }
        const i = Math.max(0, Math.min(lo - 1, this.tempoTick.length - 1));
        const us = this.tempoUs[i] || 500000;
        return this.tempoTick[i] + (seconds - S[i]) * this.ppq / (us / 1000000);
    }

    // Advances track T to its next channel event; returns false at end of track.
    _advance(t) {
        const u8 = this.u8, e = this.tEnd[t];
        let q = this.curP[t], tick = this.curTick[t], run = this.curRun[t];
        while (q < e) {
            let d = 0, c;
            do { c = u8[q++]; d = d * 128 + (c & 127); } while (c & 128);
            tick += d;
            let s = u8[q], hasFirst = false, first = 0;
            if (s < 0x80) { hasFirst = true; first = s; s = run; ++q; }
            else { ++q; if (s < 0xf0) run = s; else if (s < 0xf8 || s === 0xff) run = 0; }
            if (s === 0xff) {
                const meta = u8[q++];
                let len = 0;
                do { c = u8[q++]; len = len * 128 + (c & 127); } while (c & 128);
                q += len;
                if (meta === 0x2f) { q = e; break; }
                continue;
            }
            if (s === 0xf0 || s === 0xf7) {
                let len = 0;
                do { c = u8[q++]; len = len * 128 + (c & 127); } while (c & 128);
                q += len;
                continue;
            }
            if (s >= 0xf0) { q += s === 0xf2 ? 2 : (s === 0xf1 || s === 0xf3) ? 1 : 0; continue; }
            const cmd = s & 0xf0;
            const d1 = hasFirst ? first : u8[q++];
            let d2 = 0;
            if (cmd !== 0xc0 && cmd !== 0xd0) d2 = u8[q++];
            let st = s;
            if (cmd === 0x90 && d2 === 0) { st = 0x80 | (s & 0x0f); d2 = 64; }
            this.curP[t] = q; this.curTick[t] = tick; this.curRun[t] = run;
            this.evTick[t] = tick; this.evMsg[t] = (st | (d1 << 8) | (d2 << 16)) >>> 0;
            return true;
        }
        this.curP[t] = e; this.curTick[t] = tick; this.curRun[t] = run; this.curDone[t] = 1;
        return false;
    }

    _less(a, b) {
        const ta = this.evTick[a], tb = this.evTick[b];
        return ta < tb || (ta === tb && a < b);
    }
    _push(t) {
        const h = this.heap;
        let i = this.heapN++;
        h[i] = t;
        while (i > 0) {
            const p = (i - 1) >> 1;
            if (!this._less(h[i], h[p])) break;
            const x = h[i]; h[i] = h[p]; h[p] = x; i = p;
        }
    }
    _popReplace() {
        const h = this.heap, top = h[0];
        if (this._advance(top)) {
            h[0] = top;
        } else {
            h[0] = h[--this.heapN];
            if (this.heapN === 0) return;
        }
        let i = 0;
        for (;;) {
            const l = 2 * i + 1, r = l + 1;
            let m = i;
            if (l < this.heapN && this._less(h[l], h[m])) m = l;
            if (r < this.heapN && this._less(h[r], h[m])) m = r;
            if (m === i) break;
            const x = h[i]; h[i] = h[m]; h[m] = x; i = m;
        }
    }

    resetTo(requestTick) {
        const tick = Math.max(0, Math.min(0xffffffff, Math.floor(requestTick) || 0));
        const start = Math.min(tick, this.maxTick);
        this.heapN = 0;
        for (let t = 0; t < this.ntrk; ++t) {
            const cp = this.cp[t];
            let lo = 0, hi = cp.t.length;
            while (lo < hi) { const m = (lo + hi) >> 1; if (cp.t[m] < start) lo = m + 1; else hi = m; }
            const k = Math.max(0, lo - 1);
            this.curDone[t] = 0;
            if (cp.t.length) { this.curP[t] = cp.p[k]; this.curTick[t] = cp.t[k]; this.curRun[t] = cp.r[k]; }
            else { this.curP[t] = this.tEnd[t]; this.curTick[t] = 0; this.curRun[t] = 0; }
            let ok = this._advance(t);
            while (ok && this.evTick[t] < start) ok = this._advance(t);
            if (ok) this._push(t);
        }
        const sx = this.sysex;
        let lo = 0, hi = sx.length;
        while (lo < hi) { const m = (lo + hi) >> 1; if (sx[m].tick < start) lo = m + 1; else hi = m; }
        this.sysexCursor = lo;
        this.historyTime = this.tickToSeconds(tick);
        const hist = [];
        for (const s of sx) { if (s.tick >= tick) break; if (s.data.length) hist.push(s); }
        this.pendingHistorySysex = hist;
        this.pendingSelectors = this._selectorState(tick);
        return tick;
    }

    _selectorState(startTick) {
        const isReset = (bytes) => {
            let a = 0, n = bytes.length;
            if (n && bytes[0] === 0xf0) { ++a; --n; }
            if (n && bytes[a + n - 1] === 0xf7) --n;
            const d = (i) => bytes[a + i];
            if (n >= 4 && d(0) === 0x7e && d(2) === 0x09 && (d(3) === 0x01 || d(3) === 0x03)) return true;
            if (n >= 8 && d(0) === 0x41 && d(2) === 0x42 && d(3) === 0x12 && d(4) === 0x40 && d(5) === 0x00 && d(6) === 0x7f && d(7) === 0x00) return true;
            return n >= 7 && d(0) === 0x43 && d(2) === 0x4c && d(3) === 0x00 && d(4) === 0x00 && d(5) === 0x7e && d(6) === 0x00;
        };
        let have = false, rt = 0, rtr = 0, ro = 0;
        for (const s of this.sysex) {
            if (s.tick >= startTick) break;
            if (isReset(s.data)) { have = true; rt = s.tick; rtr = s.track; ro = s.order; }
        }
        const st = [];
        for (let c = 0; c < 16; ++c) st.push({ pm: 0, pl: 0, am: 0, al: 0, prog: 0, bend: 8192, spm: false, spl: false, sp: false, sb: false });
        const S = this.sel;
        for (let i = 0; i < this.selN; ++i) {
            const tk = S[i * 4];
            if (tk >= startTick) break;
            if (have) {
                const tr = S[i * 4 + 1], od = S[i * 4 + 2];
                const after = tk !== rt ? tk > rt : (tr !== rtr ? tr > rtr : od > ro);
                if (!after) continue;
            }
            const m = S[i * 4 + 3], cmd = m & 0xf0, cs = st[m & 0x0f];
            const d1 = (m >>> 8) & 0x7f, d2 = (m >>> 16) & 0x7f;
            if (cmd === 0xb0) {
                if (d1 === 0) { cs.pm = d2; cs.spm = true; }
                else if (d1 === 32) { cs.pl = d2; cs.spl = true; }
            } else if (cmd === 0xc0) {
                cs.prog = d1; cs.am = cs.pm; cs.al = cs.pl; cs.sp = true;
            } else if (cmd === 0xe0) {
                cs.bend = d1 | (d2 << 7); cs.sb = true;
            }
        }
        const out = [];
        const emit = (s, a, b) => out.push((s | (a << 8) | (b << 16)) >>> 0);
        for (let c = 0; c < 16; ++c) {
            const cs = st[c];
            if (cs.sp) {
                if (cs.am !== 0) emit(0xb0 | c, 0, cs.am);
                if (cs.al !== 0) emit(0xb0 | c, 32, cs.al);
                emit(0xc0 | c, cs.prog, 0);
                if (cs.spm && cs.pm !== cs.am) emit(0xb0 | c, 0, cs.pm);
                if (cs.spl && cs.pl !== cs.al) emit(0xb0 | c, 32, cs.pl);
            } else {
                if (cs.spm) emit(0xb0 | c, 0, cs.pm);
                if (cs.spl) emit(0xb0 | c, 32, cs.pl);
            }
            if (cs.sb && cs.bend !== 8192) emit(0xe0 | c, cs.bend & 0x7f, (cs.bend >> 7) & 0x7f);
        }
        return out;
    }

    // Mirrors buildEventBatch + the parser Worker's post-processing.
    fill(targetSeconds, maxEvents, velocityFloor) {
        const endTick = Math.min(this.maxTick, Math.max(0, Math.ceil(this.secondsToTick(targetSeconds))));
        const budget = maxEvents * 8;
        let msgs = new Uint32Array(Math.min(maxEvents, 65536)), ticks = new Float64Array(msgs.length);
        let n = 0, consumed = 0, stop = false;
        let lastTick = -1, lastMid = 0, lastCount = 0;
        while (n < maxEvents && consumed < budget) {
            if (this.heapN === 0) { stop = true; break; }
            const t = this.heap[0];
            const tk = this.evTick[t];
            if (tk > endTick) { stop = true; break; }
            const m = this.evMsg[t];
            this._popReplace();
            ++consumed;
            const cmd = m & 0xf0, vel = (m >>> 16) & 0x7f;
            if (cmd === 0x90 && vel !== 0) {
                if (n > 0 && lastTick === tk && lastMid === m && lastCount < 256) {
                    ++lastCount;
                    msgs[n - 1] = (m | ((lastCount - 1) << 24)) >>> 0;
                    continue;
                }
                if (vel < velocityFloor) { lastTick = -1; continue; }
                lastTick = tk; lastMid = m; lastCount = 1;
            } else {
                lastTick = -1;
            }
            if (n === msgs.length) {
                const nm = new Uint32Array(n * 2); nm.set(msgs); msgs = nm;
                const nt = new Float64Array(n * 2); nt.set(ticks); ticks = nt;
            }
            msgs[n] = m; ticks[n] = tk; ++n;
        }
        const hasNext = this.heapN > 0;
        const nextTick = hasNext ? this.evTick[this.heap[0]] : 0;
        let complete = stop || !hasNext || nextTick > endTick;

        const times = new Float64Array(n);
        let lt = -1, ls = 0;
        for (let i = 0; i < n; ++i) { if (ticks[i] !== lt) { lt = ticks[i]; ls = this.tickToSeconds(lt); } times[i] = ls; }

        const safeExclusive = complete ? endTick + 1 : (hasNext ? nextTick : endTick + 1);
        const sxList = [];
        const hist = this.pendingHistorySysex;
        if (hist && hist.length) for (const s of hist) sxList.push({ data: s.data, time: this.historyTime });
        this.pendingHistorySysex = null;
        while (this.sysexCursor < this.sysex.length && this.sysex[this.sysexCursor].tick < safeExclusive) {
            const s = this.sysex[this.sysexCursor++];
            if (s.data.length) sxList.push({ data: s.data, time: this.tickToSeconds(s.tick) });
        }
        let sxBytes = 0;
        for (const s of sxList) sxBytes += s.data.length;
        const sysexMeta = new Uint32Array(sxList.length * 3), sysexData = new Uint8Array(sxBytes), sysexTimes = new Float64Array(sxList.length);
        let off = 0;
        sxList.forEach((s, i) => {
            sysexMeta[i * 3] = 0; sysexMeta[i * 3 + 1] = off; sysexMeta[i * 3 + 2] = s.data.length;
            sysexData.set(s.data, off); off += s.data.length; sysexTimes[i] = s.time;
        });

        let messages = msgs.subarray(0, n), outTimes = times;
        const pre = this.pendingSelectors;
        if (pre && pre.length) {
            const mm = new Uint32Array(pre.length + n), tt = new Float64Array(pre.length + n);
            mm.set(pre, 0); mm.set(messages, pre.length);
            tt.fill(this.historyTime, 0, pre.length); tt.set(outTimes, pre.length);
            messages = mm; outTimes = tt;
        } else {
            messages = messages.slice();
        }
        this.pendingSelectors = null;

        let safeUntil;
        if (complete) {
            safeUntil = hasNext ? Math.max(targetSeconds, this.tickToSeconds(nextTick)) : 1e9;
        } else {
            safeUntil = Math.min(targetSeconds, this.tickToSeconds(nextTick));
        }
        return { messages, times: outTimes, sysexMeta, sysexData, sysexTimes, safeUntil, complete };
    }
}
// END SmfSynthSource

if (typeof self !== "undefined" && typeof importScripts === "function") {
    let source = null, port = null, generation = 0;
    const MAX_EVENTS = 262144;
    const onPort = (event) => {
        const d = event.data || {};
        if (!source || d.generation !== generation && d.type !== "reset") return;
        if (d.type === "reset") {
            generation = d.generation >>> 0;
            source.resetTo(Math.floor(source.secondsToTick(Math.max(0, Number(d.time) || 0))));
            return;
        }
        if (d.type === "fill") {
            const b = source.fill(Math.max(0, Number(d.target) || 0), MAX_EVENTS,
                Math.max(0, Math.min(127, d.velocityFloor | 0)));
            port.postMessage({ type: "batch", generation, ...b },
                [b.messages.buffer, b.times.buffer, b.sysexMeta.buffer, b.sysexData.buffer, b.sysexTimes.buffer]);
        }
    };
    self.onmessage = (event) => {
        const d = event.data || {};
        if (d.type === "port" && d.port) {
            port = d.port;
            port.onmessage = onPort;
            return;
        }
        if (d.type === "load" && d.file) {
            const t0 = performance.now();
            try {
                const buffer = new FileReaderSync().readAsArrayBuffer(d.file);
                source = new SmfSynthSource(buffer);
                self.postMessage({ type: "ready", ms: performance.now() - t0, maxTick: source.maxTick });
            } catch (error) {
                source = null;
                self.postMessage({ type: "failed", error: String(error && error.message || error) });
            }
        }
    };
}
