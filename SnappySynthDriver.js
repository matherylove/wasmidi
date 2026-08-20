/**
 * SnappySynthDriver.js — SnappySynth V2 JS Port
 *
 * Direct port of the C++ SnappySynth V2 voice engine to a single
 * AudioWorkletProcessor. Acts as a MIDI Out device: receives MIDI DWORDs
 * from the main thread, synthesizes audio from an SF2 soundfont, and
 * writes directly to outputs[0] on every process() callback.
 *
 * Architecture: ONE node, no ring buffer, no relay, no pre-render queue.
 *   main thread → port.postMessage({ type:'midi', dword, time }) → process() → outputs[0] → DAC
 *
 * Messages IN (main → worklet):
 *   { type:'init',            settings }                  — apply initial settings
 *   { type:'settings',        settings }                  — update settings live
 *   { type:'midi',            dword, time }               — single MIDI DWORD + AudioContext timestamp
 *   { type:'midi_batch',      events }                    — array of { dword, time } (#BATCH1)
 *   { type:'sysex',           data }                      — SysEx ArrayBuffer (#SYSEX3)
 *   { type:'load_sf_buffer',  buffer, name }              — load SF2 from ArrayBuffer
 *   { type:'reload_sf',       url }                       — load SF2 from URL
 *   { type:'reset' }                                      — all-notes-off + channel reset
 *
 * Messages OUT (worklet → main):
 *   { type:'sf_loading' }
 *   { type:'sf_loaded',  regionCount, name }
 *   { type:'sf_error',   message }
 *   { type:'stats',      activeVoices, renderingTime }
 *
 * Fixes / Optimizaciones:
 *   Bug #1   — MIDI timing intra-block
 *   Bug #2   — _steal() priority: IDLE → RELEASE → SUSTAIN → DECAY → oldest active
 *   Bug #3   — V_DECAY loop infinito con sustainLvl >= 1
 *   #A       — exClass: quickRelease 8ms sin contaminar relCoeff
 *   #B       — re-trigger quickRelease solo si voz no está ya en V_RELEASE
 *   #V1      — Velocity curve SF2-spec: 960cB range → pow(10,-cB/200)
 *   #V2      — volSustain en centibels correcto
 *   #V3      — CC10 pan divisor 64
 *   #P1      — Pitch bend cacheado, Math.pow solo cuando cambia
 *   #P3      — MIDI timestamp → AudioContext time
 *   #P4      — CC0/CC32 bank MSB+LSB combinados
 *   #REL     — Release mínimo 60ms
 *   #S2      — Limiter floor 0.001 (nunca silencio total)
 *   #S3      — _steal() nunca devuelve null
 *   #OPT1    — activeVoices como Set
 *   #OPT2    — Pitch bend cacheado por canal
 *   #OPT4    — Limiter release 80ms, recuperación correcta
 *   #OPT5    — rendering time via currentTime (AudioWorkletGlobalScope, segundos)
 *   #OPT6    — numVoices default 128
 *   #OPT7    — tc2s floor 0.001s
 *   #OPT9    — Robo proactivo de voces cuando RT > 85%
 *   #FIX-V   — _applySettings recorta voices[] cuando numVoices baja
 *   #FIX-S   — _steal() respeta límite numVoices en iteración
 *   #FIX-PERF — performance.now() → currentTime (no existe en AudioWorkletGlobalScope)
 *   #SYSEX1  — _dispatchSysEx: GM/GS/XG reset, Universal Master Volume 14-bit, GS drum part
 *   #SYSEX2  — _applyGMReset: reinicio completo a defaults GM en 16 canales
 *   #SYSEX3  — nuevo msg type 'sysex' { data: ArrayBuffer }
 *   #SYSEX4  — nuevo msg type 'midi_batch' [{ dword, time }] (ráfagas sin postMessage por evento)
 */
'use strict';

const clamp = (v, lo, hi) => v < lo ? lo : v > hi ? hi : v;
const tc2s  = tc => tc <= -32768 ? 0.001 : Math.max(0.001, Math.pow(2, tc / 1200));

class SF2Parser {
  constructor(buf) {
    this.dv  = new DataView(buf);
    this.buf = buf;
    this.pos = 0;
  }
  u8()   { return this.dv.getUint8(this.pos++); }
  i8()   { return this.dv.getInt8(this.pos++); }
  u16()  { const v = this.dv.getUint16(this.pos, true); this.pos += 2; return v; }
  i16()  { const v = this.dv.getInt16(this.pos,  true); this.pos += 2; return v; }
  u32()  { const v = this.dv.getUint32(this.pos, true); this.pos += 4; return v; }
  cc()   { let s = ''; for (let i = 0; i < 4; i++) s += String.fromCharCode(this.dv.getUint8(this.pos++)); return s; }
  str(n) { let s = ''; for (let i = 0; i < n; i++) { const c = this.dv.getUint8(this.pos++); if (c) s += String.fromCharCode(c); } return s; }
  seek(p){ this.pos = p; }

  parse() {
    if (this.cc() !== 'RIFF') throw new Error('Not a valid SF2 (no RIFF)');
    const riffSize = this.u32();
    if (this.cc() !== 'sfbk') throw new Error('Not a valid SF2 (no sfbk)');
    const chunks = {};
    while (this.pos < 8 + riffSize) {
      const id = this.cc(), sz = this.u32(), start = this.pos;
      if (id === 'LIST') { const t = this.cc(); chunks[t] = { start: this.pos, size: sz - 4 }; }
      this.pos = start + sz;
    }
    let smpl = null;
    if (chunks['sdta']) {
      let p = chunks['sdta'].start, e = p + chunks['sdta'].size;
      while (p < e) {
        this.seek(p); const sid = this.cc(), ssz = this.u32();
        if (sid === 'smpl') smpl = new Int16Array(this.buf, this.pos, ssz >> 1);
        p += 8 + ssz;
      }
    }
    if (!smpl) throw new Error('SF2 has no smpl chunk');
    const sc = {};
    if (chunks['pdta']) {
      let p = chunks['pdta'].start, end = p + chunks['pdta'].size;
      while (p < end) {
        this.seek(p); const sid = this.cc(), ssz = this.u32();
        sc[sid] = { offset: this.pos, size: ssz }; p += 8 + ssz;
      }
    }
    const phdr = this._phdr(sc['phdr']), pbag = this._bag(sc['pbag']),
          pgen = this._gen(sc['pgen']),  inst = this._inst(sc['inst']),
          ibag = this._bag(sc['ibag']),  igen = this._gen(sc['igen']),
          shdr = this._shdr(sc['shdr']);
    return this._build(phdr, pbag, pgen, inst, ibag, igen, shdr, smpl);
  }

  _phdr(c) {
    if (!c) return []; this.seek(c.offset);
    const n = (c.size / 38) | 0, list = [];
    for (let i = 0; i < n; i++) {
      const name = this.str(20), preset = this.u16(), bank = this.u16(), bagIdx = this.u16();
      this.pos += 12; list.push({ name, preset, bank, bagIdx });
    }
    return list;
  }
  _bag(c) {
    if (!c) return []; this.seek(c.offset);
    const n = (c.size / 4) | 0, list = [];
    for (let i = 0; i < n; i++) { const genIdx = this.u16(), modIdx = this.u16(); list.push({ genIdx, modIdx }); }
    return list;
  }
  _gen(c) {
    if (!c) return []; this.seek(c.offset);
    const n = (c.size / 4) | 0, list = [];
    for (let i = 0; i < n; i++) { const oper = this.u16(), amount = this.i16(); list.push({ oper, amount }); }
    return list;
  }
  _inst(c) {
    if (!c) return []; this.seek(c.offset);
    const n = (c.size / 22) | 0, list = [];
    for (let i = 0; i < n; i++) { const name = this.str(20), bagIdx = this.u16(); list.push({ name, bagIdx }); }
    return list;
  }
  _shdr(c) {
    if (!c) return []; this.seek(c.offset);
    const n = (c.size / 46) | 0, list = [];
    for (let i = 0; i < n; i++) {
      const name = this.str(20), start = this.u32(), end = this.u32(),
            loopStart = this.u32(), loopEnd = this.u32(),
            sampleRate = this.u32(), originalKey = this.u8(), correction = this.i8(),
            sampleLink = this.u16(), sampleType = this.u16();
      list.push({ name, start, end, loopStart, loopEnd, sampleRate, originalKey, correction, sampleLink, sampleType });
    }
    return list;
  }

  _build(phdr, pbag, pgen, inst, ibag, igen, shdr, smpl) {
    const G = {
      START_OFF:0,END_OFF:1,LSTART_OFF:2,LEND_OFF:3,START_COARSE:4,END_COARSE:12,
      PAN:17,VENV_DELAY:33,VENV_ATTACK:34,VENV_HOLD:35,VENV_DECAY:36,VENV_SUSTAIN:37,
      VENV_RELEASE:38,INSTRUMENT:41,KEY_RANGE:43,VEL_RANGE:44,LSTART_COARSE:45,
      ATTENUATION:48,LEND_COARSE:50,COARSE_TUNE:51,FINE_TUNE:52,SAMPLE_ID:53,
      SAMPLE_MODES:54,SCALE_TUNING:56,EXCLUSIVE_CLASS:57,ROOT_KEY:58,
    };
    const regions = [];
    for (let pi = 0; pi < phdr.length - 1; pi++) {
      const p = phdr[pi], pBagEnd = phdr[pi + 1].bagIdx;
      for (let bi = p.bagIdx; bi < pBagEnd; bi++) {
        const pgEnd = bi + 1 < pbag.length ? pbag[bi + 1].genIdx : pgen.length;
        const pG = {};
        for (let gi = pbag[bi].genIdx; gi < pgEnd; gi++) pG[pgen[gi].oper] = pgen[gi].amount;
        const instIdx = pG[G.INSTRUMENT];
        if (instIdx === undefined || instIdx >= inst.length - 1) continue;
        const iBagEnd = inst[instIdx + 1].bagIdx;
        for (let ibi = inst[instIdx].bagIdx; ibi < iBagEnd; ibi++) {
          const igEnd = ibi + 1 < ibag.length ? ibag[ibi + 1].genIdx : igen.length;
          const iG = {};
          for (let gi = ibag[ibi].genIdx; gi < igEnd; gi++) iG[igen[gi].oper] = igen[gi].amount;
          const sIdx = iG[G.SAMPLE_ID]; if (sIdx === undefined) continue;
          const smp = shdr[sIdx]; if (!smp || smp.sampleType === 0) continue;
          const krR = iG[G.KEY_RANGE], vrR = iG[G.VEL_RANGE];
          const keyLo = krR !== undefined ? (krR & 0xFF) : 0;
          const keyHi = krR !== undefined ? ((krR >> 8) & 0xFF) : 127;
          const velLo = vrR !== undefined ? (vrR & 0xFF) : 0;
          const velHi = vrR !== undefined ? ((vrR >> 8) & 0xFF) : 127;
          const rootKey    = iG[G.ROOT_KEY] !== undefined ? iG[G.ROOT_KEY] : smp.originalKey;
          const coarseTune = (iG[G.COARSE_TUNE] || 0) + (pG[G.COARSE_TUNE] || 0);
          const fineTune   = (iG[G.FINE_TUNE]   || 0) + (pG[G.FINE_TUNE]   || 0) + smp.correction;
          const scaleTune  = iG[G.SCALE_TUNING] !== undefined ? iG[G.SCALE_TUNING] : 100;
          const atten      = (iG[G.ATTENUATION] || 0) + (pG[G.ATTENUATION] || 0);
          const gain       = Math.pow(10, -atten / 200);
          const pan        = clamp(((iG[G.PAN] || 0) + (pG[G.PAN] || 0)) / 500, -1, 1);
          const loopMode   = (iG[G.SAMPLE_MODES] || 0) & 0x03;
          const exClass    = iG[G.EXCLUSIVE_CLASS] || 0;
          const startOff   = (iG[G.START_OFF]    || 0) + (iG[G.START_COARSE]  || 0) * 32768;
          const endOff     = (iG[G.END_OFF]      || 0) + (iG[G.END_COARSE]    || 0) * 32768;
          const lsOff      = (iG[G.LSTART_OFF]   || 0) + (iG[G.LSTART_COARSE] || 0) * 32768;
          const leOff      = (iG[G.LEND_OFF]     || 0) + (iG[G.LEND_COARSE]   || 0) * 32768;
          regions.push({
            bank: p.bank, preset: p.preset, keyLo, keyHi, velLo, velHi,
            rootKey, coarseTune, fineTune, scaleTune, gain, pan, loopMode, exClass,
            volDelay:   tc2s(iG[G.VENV_DELAY]   !== undefined ? iG[G.VENV_DELAY]   : -12000),
            volAttack:  tc2s(iG[G.VENV_ATTACK]  !== undefined ? iG[G.VENV_ATTACK]  : -12000),
            volHold:    tc2s(iG[G.VENV_HOLD]    !== undefined ? iG[G.VENV_HOLD]    : -12000),
            volDecay:   tc2s(iG[G.VENV_DECAY]   !== undefined ? iG[G.VENV_DECAY]   : -12000),
            volSustain: clamp(Math.pow(10, -(iG[G.VENV_SUSTAIN] || 0) / 200), 0, 1),
            volRelease: tc2s(iG[G.VENV_RELEASE] !== undefined ? iG[G.VENV_RELEASE] : -12000),
            sample: {
              data: smpl,
              start:      smp.start     + startOff,
              end:        smp.end       + endOff,
              loopStart:  smp.loopStart + lsOff,
              loopEnd:    smp.loopEnd   + leOff,
              sampleRate: smp.sampleRate,
            },
          });
        }
      }
    }
    return regions;
  }
}

const V_IDLE=0, V_DELAY=1, V_ATTACK=2, V_HOLD=3, V_DECAY=4, V_SUSTAIN=5, V_RELEASE=6;

class Voice {
  constructor() {
    this.state          = V_IDLE;
    this.region         = null;
    this._sustainHeld   = false;
    this._quickRel      = false;
    this._quickRelCoeff = 0;
    this.envLevel       = 0;
    this._voiceAge      = 0;
  }

  noteOn(region, ch, note, vel, sr, gainVol, panL, panR, age) {
    this.region   = region;
    this.channel  = ch;
    this.note     = note;
    this.gainVol  = gainVol;
    this.panL     = panL;
    this.panR     = panR;
    this.exClass  = region.exClass;
    this._sustainHeld = false;
    this._quickRel    = false;
    this._voiceAge    = age;

    const semis = (note - region.rootKey) * (region.scaleTune / 100) + region.coarseTune;
    this.basePhaseInc = (region.sample.sampleRate / sr)
                      * Math.pow(2, semis / 12)
                      * Math.pow(2, region.fineTune / 1200);
    this.phaseInc = this.basePhaseInc;
    this.phase    = region.sample.start;
    this.envLevel = 0;

    this.delayLeft  = Math.round(region.volDelay  * sr);
    this.attackLeft = Math.max(1, Math.round(region.volAttack * sr));
    this.holdLeft   = Math.round(region.volHold   * sr);
    this.decayCoeff = region.volDecay > 0.001
      ? Math.exp(-Math.log(1000) / (region.volDecay * sr)) : 0;
    this.sustainLvl = region.volSustain;
    const effRel = Math.max(region.volRelease, 0.060);
    this.relCoeff = Math.exp(-Math.log(1000) / (effRel * sr));
    this._quickRelCoeff = Math.exp(-Math.log(1000) / (0.008 * sr));
    this.state = this.delayLeft > 0 ? V_DELAY : V_ATTACK;
  }

  noteOff() {
    if (this.state !== V_IDLE && this.state !== V_RELEASE) {
      this._quickRel = false;
      this.state     = V_RELEASE;
    }
  }

  quickRelease() {
    if (this.state === V_IDLE) return;
    this._quickRel = true;
    this.state     = V_RELEASE;
  }

  updatePitchBend(pb) {
    this.phaseInc = pb !== 0
      ? this.basePhaseInc * Math.pow(2, pb * 2 / 12)
      : this.basePhaseInc;
  }

  render(outL, outR, offset, count) {
    if (this.state === V_IDLE) return false;
    const smp  = this.region.sample;
    const data = smp.data;
    let env    = this.envLevel;
    let phase  = this.phase;
    const pi_  = this.phaseInc;

    for (let i = offset, end = offset + count; i < end; i++) {
      if (this.state === V_DELAY) {
        if (--this.delayLeft <= 0) this.state = V_ATTACK;
        env = 0;
      } else if (this.state === V_ATTACK) {
        env += 1 / this.attackLeft;
        if (--this.attackLeft <= 0 || env >= 1) {
          env = 1; this.state = this.holdLeft > 0 ? V_HOLD : V_DECAY;
        }
      } else if (this.state === V_HOLD) {
        env = 1;
        if (--this.holdLeft <= 0) this.state = V_DECAY;
      } else if (this.state === V_DECAY) {
        if (this.sustainLvl >= 1.0) { env = 1.0; this.state = V_SUSTAIN; }
        else {
          env = this.decayCoeff > 0 ? env * this.decayCoeff : this.sustainLvl;
          if (env <= this.sustainLvl + 0.001) { env = this.sustainLvl; this.state = V_SUSTAIN; }
        }
      } else if (this.state === V_SUSTAIN) {
        env = this.sustainLvl;
      } else if (this.state === V_RELEASE) {
        const rc = this._quickRel ? this._quickRelCoeff : this.relCoeff;
        env *= rc;
        if (env < 0.0001) {
          this._quickRel = false; this.state = V_IDLE; this.envLevel = 0; return false;
        }
      }

      const pi = phase | 0;
      if (pi >= smp.end - 1) {
        if (this.region.loopMode >= 1) phase = smp.loopStart + (phase - smp.loopEnd);
        else { this.state = V_IDLE; this.envLevel = 0; return false; }
      }
      const pf = phase - (phase | 0);
      const s  = (data[phase | 0] + (data[(phase | 0) + 1] - data[phase | 0]) * pf) / 32768.0;
      phase += pi_;
      if (this.region.loopMode >= 1 && phase >= smp.loopEnd)
        phase = smp.loopStart + (phase - smp.loopEnd);

      const out = s * env * this.gainVol;
      outL[i] += out * this.panL;
      outR[i] += out * this.panR;
    }
    this.envLevel = env;
    this.phase    = phase;
    return this.state !== V_IDLE;
  }
}

class ChannelState {
  constructor(ch) {
    this._ch = ch;
    this.isDrum = (ch === 9);
    this.reset();
  }
  reset() {
    this.program    = 0;
    this.bank       = 0;
    this.bankLSB    = 0;
    this.volume     = 100 / 127;
    this.expression = 1;
    this.pan        = 0;
    this.sustain    = false;
    this.pitchBend  = 0;
    // isDrum no se resetea aquí — se preserva entre GM resets si fue asignado vía SysEx GS
  }
}

class Limiter {
  constructor(sr) {
    this.enabled   = true;
    this.threshold = 0.95;
    this.gain      = 1.0;
    this.atkCoeff  = Math.exp(-1 / (sr * 0.003));
    // Release 80ms — recuperación más rápida que 250ms original (#OPT4)
    this.relCoeff  = Math.exp(-1 / (sr * 0.080));
  }
  process(L, R, i) {
    if (!this.enabled) return;
    const peak = Math.max(Math.abs(L[i]), Math.abs(R[i]));
    if (peak * this.gain > this.threshold) {
      this.gain = this.gain * this.atkCoeff + (this.threshold / peak) * (1 - this.atkCoeff);
    } else {
      this.gain += (1 - this.gain) * (1 - this.relCoeff);
    }
    if (this.gain < 0.001) this.gain = 0.001; // nunca silencio total (#S2)
    L[i] *= this.gain;
    R[i] *= this.gain;
  }
  updateAttack(sr, sec)  { this.atkCoeff = Math.exp(-1 / (sr * sec)); }
  updateRelease(sr, sec) { this.relCoeff = Math.exp(-1 / (sr * sec)); }
}

// ─── SysEx helpers (#SYSEX1) ────────────────────────────────────────────────
// Normaliza el payload quitando delimitadores 0xF0 / 0xF7 si están presentes.
function sysexNormalize(data) {
  let s = 0, e = data.length;
  if (e > 0 && data[s] === 0xF0) s++;
  if (e > s && data[e - 1] === 0xF7) e--;
  return data.subarray ? data.subarray(s, e) : data.slice(s, e);
}

// GM System On (F0 7E xx 09 01 F7) o GM2 (09 03)
function sysexIsGMReset(d) {
  d = sysexNormalize(d);
  return d.length >= 4 && d[0] === 0x7E && d[2] === 0x09 && (d[3] === 0x01 || d[3] === 0x03);
}

// GS Reset: F0 41 xx 42 12 40 00 7F 00 [checksum] F7
function sysexIsGSReset(d) {
  d = sysexNormalize(d);
  return d.length >= 9 && d[0] === 0x41 && d[2] === 0x42 && d[3] === 0x12
      && d[4] === 0x40 && d[5] === 0x00 && d[6] === 0x7F && d[7] === 0x00;
}

// XG System On: F0 43 xx 4C 00 00 7E 00 F7
function sysexIsXGReset(d) {
  d = sysexNormalize(d);
  return d.length >= 7 && d[0] === 0x43 && d[2] === 0x4C
      && d[3] === 0x00 && d[4] === 0x00 && d[5] === 0x7E && d[6] === 0x00;
}

// Universal Master Volume: F0 7F xx 04 01 lsb msb F7  → 14-bit value
// Retorna el valor 0–16383 o -1 si no coincide.
function sysexGetMasterVolume14(d) {
  d = sysexNormalize(d);
  if (d.length < 6 || d[0] !== 0x7F || d[2] !== 0x04 || d[3] !== 0x01) return -1;
  return ((d[5] & 0x7F) << 7) | (d[4] & 0x7F);
}

// GS Drum Part: F0 41 xx 42 12 40 [part] 15 [mode] [checksum] F7
// Retorna { channel (0-based), mode } o null si no coincide.
// Port de gs_part_number_to_midi_channel_zero_based() del C++.
function sysexGetGSDrumPart(d) {
  d = sysexNormalize(d);
  if (d.length < 9) return null;
  if (d[0] !== 0x41 || d[2] !== 0x42 || d[3] !== 0x12 || d[4] !== 0x40) return null;
  if (d[6] !== 0x15) return null;
  const partAddr = d[5] & 0x7F;
  if ((partAddr & 0x70) !== 0x10) return null;
  const part = partAddr & 0x0F;
  // GS part → MIDI channel (0-based): part 0 = ch 9, parts 1-9 = ch 0-8, parts 10-15 = ch 10-15
  const ch = part === 0 ? 9 : (part <= 9 ? part - 1 : part);
  if (ch < 0 || ch >= 16) return null;
  return { channel: ch, mode: d[7] & 0x7F };
}

// #FIX-CTX: AudioWorkletProcessor only exists inside AudioWorkletGlobalScope.
const _AWBase = (typeof AudioWorkletProcessor !== 'undefined')
  ? AudioWorkletProcessor
  : class { constructor(){} get port(){ return { onmessage:null, postMessage(){} }; } };

class SnappySynthProcessor extends _AWBase {
  constructor(options) {
    super(options);
    this.sr             = sampleRate;
    this.ready          = false;
    this.regions        = null;
    this.numVoices      = 128;
    this.numLayers      = 4;
    this.masterVol      = 1.0;
    this.voices         = Array.from({ length: 128 }, () => new Voice());
    this.channels       = Array.from({ length: 16  }, (_, i) => new ChannelState(i));
    this.limiter        = new Limiter((typeof sampleRate !== 'undefined') ? sampleRate : 44100);
    this.activeVoices   = new Set();
    this.midiQueue      = [];
    this._statCount     = 0;
    this._renderingTime = 0;
    this._blockDuration = 128 / sampleRate;
    this._voiceAge      = 0;
    this.port.onmessage = e => this._onMsg(e.data);
  }

  _onMsg(d) {
    switch (d.type) {
      case 'init':           if (d.settings) this._applySettings(d.settings); break;
      case 'settings':       this._applySettings(d.settings); break;
      case 'midi':           this.midiQueue.push({ dword: d.dword, time: d.time || 0 }); break;
      // #SYSEX4 — batch MIDI: array de { dword, time } en un solo mensaje IPC
      case 'midi_batch': {
        const evs = d.events;
        if (Array.isArray(evs)) {
          for (let i = 0; i < evs.length; i++)
            this.midiQueue.push({ dword: evs[i].dword, time: evs[i].time || 0 });
        }
        break;
      }
      // #SYSEX3 — SysEx desde ArrayBuffer
      case 'sysex': {
        const buf = d.data;
        if (buf) {
          const arr = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
          this._dispatchSysEx(arr);
        }
        break;
      }
      case 'load_sf_buffer': this._loadBuffer(d.buffer, d.name); break;
      case 'reload_sf':      this._loadUrl(d.url); break;
      case 'reset':          this._panic(); break;
    }
  }

  _applySettings(s) {
    if (s.numVoices !== undefined) {
      this.numVoices = s.numVoices;
      while (this.voices.length < this.numVoices) this.voices.push(new Voice());
      if (this.voices.length > this.numVoices) {
        const removed = this.voices.splice(this.numVoices);
        for (const v of removed) { v.state = V_IDLE; this.activeVoices.delete(v); }
      }
    }
    if (s.numLayers  !== undefined) this.numLayers  = s.numLayers;
    if (s.masterVol  !== undefined) this.masterVol  = s.masterVol;
    if (s.limiterEnabled   !== undefined) this.limiter.enabled   = s.limiterEnabled;
    if (s.limiterThreshold !== undefined) this.limiter.threshold = s.limiterThreshold;
    if (s.limiterAttack    !== undefined) this.limiter.updateAttack(this.sr,  s.limiterAttack);
    if (s.limiterRelease   !== undefined) this.limiter.updateRelease(this.sr, s.limiterRelease);
  }

  _loadBuffer(buffer, name) {
    this.ready = false;
    this.port.postMessage({ type: 'sf_loading' });
    try {
      this.regions = new SF2Parser(buffer).parse();
      this.ready   = true;
      this.port.postMessage({ type: 'sf_loaded', regionCount: this.regions.length, name });
    } catch (err) {
      this.port.postMessage({ type: 'sf_error', message: err.message });
    }
  }

  async _loadUrl(url) {
    if (!url) return;
    this.ready = false;
    this.port.postMessage({ type: 'sf_loading' });
    try {
      const resp = await fetch(url);
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
      this._loadBuffer(await resp.arrayBuffer(), url.split('/').pop());
    } catch (err) {
      this.port.postMessage({ type: 'sf_error', message: err.message });
    }
  }

  _dispatch(dword) {
    const status = dword & 0xFF, cmd = status & 0xF0, ch = status & 0x0F;
    const b1 = (dword >> 8) & 0xFF, b2 = (dword >> 16) & 0xFF;
    switch (cmd) {
      case 0x90: b2 > 0 ? this._noteOn(ch, b1, b2) : this._noteOff(ch, b1); break;
      case 0x80: this._noteOff(ch, b1); break;
      case 0xB0: this._cc(ch, b1, b2); break;
      case 0xC0: this.channels[ch].program = b1; break;
      case 0xE0: {
        const pb = ((b2 << 7 | b1) - 8192) / 8192;
        this.channels[ch].pitchBend = pb;
        for (const v of this.activeVoices)
          if (v.channel === ch) v.updatePitchBend(pb);
        break;
      }
    }
  }

  // #SYSEX1 — Despachador SysEx: port de dispatch_sysex_data_at_qpc() del C++
  _dispatchSysEx(data) {
    // GM / GM2 / GS / XG reset → _applyGMReset
    if (sysexIsGMReset(data) || sysexIsGSReset(data) || sysexIsXGReset(data)) {
      this._applyGMReset();
      return;
    }
    // Universal Master Volume 14-bit
    const vol14 = sysexGetMasterVolume14(data);
    if (vol14 >= 0) {
      // 0x3FFF (16383) = volumen máximo → normalizar a 0.0–1.0
      this.masterVol = vol14 / 16383;
      return;
    }
    // GS Drum Part remap
    const drumInfo = sysexGetGSDrumPart(data);
    if (drumInfo) {
      this.channels[drumInfo.channel].isDrum = (drumInfo.mode !== 0);
      return;
    }
    // SysEx no reconocida: ignorar silenciosamente
  }

  // #SYSEX2 — Port de apply_gm_reset_at_qpc() del C++
  // Reinicia todos los canales a defaults GM: CC120/121/123, program 0,
  // pitch bend centrado, drum solo en ch 9.
  _applyGMReset() {
    // All-notes-off + release activas
    for (const v of this.activeVoices) v.noteOff();
    for (const c of this.channels) {
      c.reset();
      c.isDrum = (c._ch === 9);
    }
    this.masterVol = 1.0;
    // Limpiar pitch bend en voces activas
    for (const v of this.activeVoices) v.updatePitchBend(0);
  }

  _noteOn(ch, note, vel) {
    if (!this.regions) return;
    const chan = this.channels[ch];
    const bank = chan.isDrum ? 128 : chan.bank;
    const prog = chan.program;

    for (const v of this.activeVoices)
      if (v.state !== V_RELEASE && v.channel === ch && v.note === note)
        v.quickRelease();

    const velCB   = (1 - vel / 127) * 960;
    const velGain = Math.pow(10, -velCB / 200);

    let layers = 0;
    for (const r of this.regions) {
      if (r.bank !== bank || r.preset !== prog) continue;
      if (note < r.keyLo || note > r.keyHi)     continue;
      if (vel  < r.velLo || vel  > r.velHi)     continue;
      if (layers >= this.numLayers) break;

      if (r.exClass > 0)
        for (const v of this.activeVoices)
          if (v.channel === ch && v.exClass === r.exClass) v.quickRelease();

      const voice = this._steal();
      if (!voice) continue;

      const vol   = chan.volume * chan.expression * velGain * r.gain * this.masterVol;
      const pan   = clamp(chan.pan + r.pan, -1, 1);
      const angle = (pan + 1) * Math.PI / 4;
      voice.noteOn(r, ch, note, vel, this.sr, vol, Math.cos(angle), Math.sin(angle), ++this._voiceAge);
      voice.updatePitchBend(chan.pitchBend);
      this.activeVoices.add(voice);
      layers++;
    }
  }

  _noteOff(ch, note) {
    for (const v of this.activeVoices) {
      if (v.channel === ch && v.note === note && v.state !== V_IDLE) {
        if (this.channels[ch].sustain) v._sustainHeld = true;
        else v.noteOff();
      }
    }
  }

  _cc(ch, cc, val) {
    const c = this.channels[ch];
    if      (cc === 7)  c.volume     = val / 127;
    else if (cc === 11) c.expression = val / 127;
    else if (cc === 10) c.pan        = (val - 64) / 64;
    else if (cc === 0)  c.bank       = (val << 7) | (c.bankLSB || 0);
    else if (cc === 32) { c.bankLSB  = val; c.bank = ((c.bank >> 7) << 7) | val; }
    else if (cc === 64) {
      c.sustain = val >= 64;
      if (!c.sustain)
        for (const v of this.activeVoices)
          if (v.channel === ch && v._sustainHeld) { v._sustainHeld = false; v.noteOff(); }
    }
    else if (cc === 120 || cc === 123)
      for (const v of this.activeVoices)
        if (v.channel === ch && v.state !== V_IDLE) v.noteOff();
    else if (cc === 121) c.reset();
  }

  _panic() {
    for (const v of this.voices) v.state = V_IDLE;
    this.activeVoices.clear();
    for (const c of this.channels) { c.reset(); c.isDrum = (c._ch === 9); }
    this.midiQueue.length = 0;
  }

  _steal() {
    for (let i = 0; i < this.numVoices; i++) {
      const v = this.voices[i];
      if (v && v.state === V_IDLE && !this.activeVoices.has(v)) return v;
    }
    let best = null, bestE = Infinity;
    for (const v of this.activeVoices)
      if (v.state === V_RELEASE && v.envLevel < bestE) { best = v; bestE = v.envLevel; }
    if (best) return best;
    bestE = Infinity;
    for (const v of this.activeVoices)
      if (v.state === V_SUSTAIN && v.envLevel < bestE) { best = v; bestE = v.envLevel; }
    if (best) return best;
    bestE = Infinity;
    for (const v of this.activeVoices)
      if (v.state === V_DECAY && v.envLevel < bestE) { best = v; bestE = v.envLevel; }
    if (best) return best;
    let oldest = null, oldestAge = Infinity;
    for (const v of this.activeVoices)
      if (v._voiceAge < oldestAge) { oldest = v; oldestAge = v._voiceAge; }
    return oldest || this.voices[0];
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    if (!out || !out[0]) return true;
    const outL = out[0], outR = out.length > 1 ? out[1] : out[0];
    const len  = outL.length;
    this._blockDuration = len / this.sr;
    outL.fill(0); outR.fill(0);

    const t0 = (typeof currentTime !== 'undefined') ? currentTime : 0;

    if (this.ready) {
      if (this._renderingTime > 0.85 && this.activeVoices.size > 0) {
        const target = Math.max(1, Math.floor(this.numVoices * (1 - (this._renderingTime - 0.85))));
        while (this.activeVoices.size > target) {
          let worst = null, worstE = Infinity;
          for (const v of this.activeVoices)
            if ((v.state === V_SUSTAIN || v.state === V_RELEASE || v.state === V_DECAY)
                && v.envLevel < worstE) { worst = v; worstE = v.envLevel; }
          if (!worst) break;
          worst.quickRelease();
          if (this._renderingTime < 0.95) break;
        }
      }

      const blockStart = (typeof currentTime !== 'undefined') ? currentTime : 0;
      const invSr      = 1 / this.sr;

      if (this.midiQueue.length > 0) {
        const pending = this.midiQueue.splice(0);
        let qi = 0;
        for (let i = 0; i < len; i++) {
          const sampleTime = blockStart + i * invSr;
          while (qi < pending.length && pending[qi].time <= sampleTime) {
            this._dispatch(pending[qi].dword);
            qi++;
          }
          for (const v of this.activeVoices) {
            const alive = v.render(outL, outR, i, 1);
            if (!alive) this.activeVoices.delete(v);
          }
          this.limiter.process(outL, outR, i);
        }
        for (let k = qi; k < pending.length; k++) this.midiQueue.unshift(pending[k]);
      } else {
        for (const v of this.activeVoices) {
          const alive = v.render(outL, outR, 0, len);
          if (!alive) this.activeVoices.delete(v);
        }
        for (let i = 0; i < len; i++) this.limiter.process(outL, outR, i);
      }
    } else {
      if (this.midiQueue.length > 0) this.midiQueue.length = 0;
    }

    const elapsed = ((typeof currentTime !== 'undefined') ? currentTime : 0) - t0;
    this._renderingTime = this._renderingTime * 0.85 + (elapsed / this._blockDuration) * 0.15;

    if ((++this._statCount & 127) === 0) {
      this.port.postMessage({
        type: 'stats',
        activeVoices: this.activeVoices.size,
        renderingTime: this._renderingTime,
      });
    }
    return true;
  }
}

if (typeof registerProcessor !== 'undefined') {
  try {
    registerProcessor('snappy-synth', SnappySynthProcessor);
  } catch (e) {
    if (!String(e).includes('already registered')) throw e;
  }
}
