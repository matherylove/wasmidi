/**
 * midi_parser_worker.js — WASM-accelerated MIDI parser worker
 *
 * Drop-in replacement for the inline <script id="workerSrc"> worker in MPWGL2.html.
 * Loads midi_parser.wasm, falls back to the pure-JS parser if WASM unavailable.
 *
 * Protocol identical to the original worker:
 *   IN:  ArrayBuffer (raw MIDI file)
 *   OUT: { type:'progress', phase, pct, track?, totalSoFar? }
 *       { type:'chunk', buf, count, totalSoFar, final, fmt, numTracks, ppq,
 *                       tempoMap, secToTempoMap, ccCount,
 *                       ccT, ccType, ccCh, ccD1, ccD2,
 *                       activeChannelMasks }
 *       { type:'error', msg }
 */
'use strict';

/* ─── WASM loader ─── */
let _wasmInstance = null;
let _wasmMemory   = null;
let _wasmReady    = false;

async function loadWasm() {
  try {
    const resp = await fetch(new URL('midi_parser.wasm', self.location.href));
    if (!resp.ok) throw new Error('HTTP ' + resp.status);
    const bytes  = await resp.arrayBuffer();
    const result = await WebAssembly.instantiate(bytes, {
      env: {
        /* minimal env — no imports needed for our pure-C module */
      }
    });
    _wasmInstance = result.instance;
    _wasmMemory   = _wasmInstance.exports.memory;
    _wasmReady    = true;
  } catch (e) {
    /* WASM unavailable — will fall back to pure JS */
    _wasmReady = false;
    console.warn('[midi_parser_worker] WASM load failed, using JS fallback:', e.message);
  }
}

/* Kick off WASM load immediately when worker starts */
const _wasmLoadPromise = loadWasm();

/* ─── WASM parse path ─── */
function parseWithWasm(u8) {
  const exports = _wasmInstance.exports;
  const mem = _wasmMemory;

  /* Alloc input buffer in WASM linear memory */
  const inPtr = exports.malloc(u8.byteLength);
  if (!inPtr) throw new Error('WASM malloc failed');
  new Uint8Array(mem.buffer).set(u8, inPtr);

  self.postMessage({ type: 'progress', phase: 'pass1', pct: 10 });

  const ret = exports.midi_parse(inPtr, u8.byteLength);
  exports.free(inPtr);

  if (ret !== 0) {
    if (ret === -2) throw new Error('SMPTE timing not supported');
    throw new Error('midi_parse returned ' + ret);
  }

  self.postMessage({ type: 'progress', phase: 'pass2', pct: 85 });

  /* Read ResultHeader (44 bytes, all uint32/float) from WASM memory */
  const resPtr = exports.get_result_ptr();
  const view   = new DataView(mem.buffer);
  const noteCount   = view.getUint32(resPtr +  0, true);
  const ccCount     = view.getUint32(resPtr +  4, true);
  const fmt         = view.getUint32(resPtr +  8, true);
  const numTracks   = view.getUint32(resPtr + 12, true);
  const ppq         = view.getUint32(resPtr + 16, true);
  const fileDur     = view.getFloat32(resPtr + 20, true);
  const notesPtr    = view.getUint32(resPtr + 24, true);
  const ccPtr       = view.getUint32(resPtr + 28, true);
  const tmPtr       = view.getUint32(resPtr + 32, true);
  const tmCount     = view.getUint32(resPtr + 36, true);
  const acmPtr      = view.getUint32(resPtr + 40, true);

  /* ── Copy notes buffer ── */
  const notesBuf = mem.buffer.slice(notesPtr, notesPtr + noteCount * 6 * 4);

  /* ── Unpack CC arrays ── */
  /* CCEvent layout: float time (4) + u8 type (1) + u8 ch (1) + u8 d1 (1) + u8 d2 (1) = 8 bytes */
  const CC_STRIDE = 8;
  const ccTArr    = new Float32Array(ccCount);
  const ccTypeArr = new Uint8Array(ccCount);
  const ccChArr   = new Uint8Array(ccCount);
  const ccD1Arr   = new Uint8Array(ccCount);
  const ccD2Arr   = new Uint8Array(ccCount);
  const memU8     = new Uint8Array(mem.buffer);
  const memDv     = new DataView(mem.buffer);
  for (let i = 0; i < ccCount; i++) {
    const base  = ccPtr + i * CC_STRIDE;
    ccTArr[i]    = memDv.getFloat32(base,     true);
    ccTypeArr[i] = memU8[base + 4];
    ccChArr[i]   = memU8[base + 5];
    ccD1Arr[i]   = memU8[base + 6];
    ccD2Arr[i]   = memU8[base + 7];
  }

  /* ── Unpack tempo map ── */
  const tempoMap = [];
  const secToTempoMap = [];
  {
    let sec = 0, prevTick = 0, uspb = 500000;
    for (let i = 0; i < tmCount; i++) {
      const base  = tmPtr + i * 8;
      const tick  = view.getUint32(base,     true);
      const uspb2 = view.getUint32(base + 4, true);
      sec += (tick - prevTick) / ppq * (uspb / 1e6);
      tempoMap.push({ tick, uspb: uspb2 });
      secToTempoMap.push({ sec, bpm: Math.round(60e6 / uspb2) });
      prevTick = tick;
      uspb     = uspb2;
    }
  }

  /* ── Unpack activeChannelMasks ── */
  const activeChannelMasks = [];
  for (let i = 0; i < numTracks; i++)
    activeChannelMasks.push(view.getUint32(acmPtr + i * 4, true));

  self.postMessage({ type: 'progress', phase: 'pass2', pct: 98 });

  /* ── Transfer all buffers ── */
  const ccTBuf    = ccTArr.buffer.slice(0);
  const ccTypeBuf = ccTypeArr.buffer.slice(0);
  const ccChBuf   = ccChArr.buffer.slice(0);
  const ccD1Buf   = ccD1Arr.buffer.slice(0);
  const ccD2Buf   = ccD2Arr.buffer.slice(0);

  self.postMessage({
    type: 'chunk',
    buf:  notesBuf,
    count: noteCount,
    totalSoFar: noteCount,
    final: true,
    fmt, numTracks, ppq,
    tempoMap, secToTempoMap,
    ccCount,
    ccT:    ccTBuf,
    ccType: ccTypeBuf,
    ccCh:   ccChBuf,
    ccD1:   ccD1Buf,
    ccD2:   ccD2Buf,
    activeChannelMasks,
  }, [notesBuf, ccTBuf, ccTypeBuf, ccChBuf, ccD1Buf, ccD2Buf]);
}

/* ─── Pure-JS fallback (identical to original inline worker) ─── */
function parseWithJS(u8) {
  let pos = 0;
  const r32 = () => { const v=(u8[pos]<<24|u8[pos+1]<<16|u8[pos+2]<<8|u8[pos+3])>>>0; pos+=4; return v; };
  const r16 = () => { const v=u8[pos]<<8|u8[pos+1]; pos+=2; return v; };
  function varlen(d,p){let b=d[p++],v=b&0x7F;while(b&0x80){b=d[p++];v=(v<<7)|(b&0x7F);}return[v,p];}

  if (r32() !== 0x4D546864) { self.postMessage({ type:'error', msg:'Not a MIDI file' }); return; }
  r32();
  const fmt=r16(), numTracks=r16(), ppq=r16();
  if (ppq & 0x8000) { self.postMessage({ type:'error', msg:'SMPTE not supported' }); return; }

  const rawTracks = [];
  for (let t = 0; t < numTracks; t++) {
    if (pos + 8 > u8.length) break;
    const hdr = r32();
    if (hdr !== 0x4D54726B) { self.postMessage({ type:'error', msg:`Bad track ${t}` }); return; }
    const len = r32(); rawTracks.push(u8.subarray(pos, pos + len)); pos += len;
  }

  self.postMessage({ type:'progress', phase:'pass1', pct:0 });

  const tempoMap = [{ tick:0, uspb:500000 }];
  for (let ti = 0; ti < rawTracks.length; ti++) {
    const d=rawTracks[ti]; let p=0,tick=0;
    while(p<d.length){
      let[dt,np]=varlen(d,p);p=np;tick+=dt;
      const st=d[p];
      if(st===0xFF){
        p++;const mt=d[p++];let[ml,mp]=varlen(d,p);p=mp;
        if(mt===0x51&&ml>=3)tempoMap.push({tick,uspb:(d[p]<<16)|(d[p+1]<<8)|d[p+2]});
        if(mt===0x2F){p+=ml;break;}p+=ml;
      }else{
        const hi=st>>4;
        if(st===0xF0||st===0xF7){p++;let[ml,mp]=varlen(d,p);p=mp+ml;}
        else if(hi>=0x8&&hi<=0xB||hi===0xE)p+=3;
        else if(hi===0xC||hi===0xD)p+=2;
        else p++;
      }
    }
    self.postMessage({type:'progress',phase:'pass1',pct:Math.round((ti+1)/rawTracks.length*50)});
  }
  tempoMap.sort((a,b)=>a.tick-b.tick);
  const tmN=tempoMap.length;
  const _tk=new Float64Array(tmN),_sc=new Float64Array(tmN),_us=new Float64Array(tmN);
  {let sec=0,prev=0,uspb=500000;
   for(let i=0;i<tmN;i++){const t=tempoMap[i];sec+=(t.tick-prev)/ppq*(uspb/1e6);_tk[i]=t.tick;_sc[i]=sec;_us[i]=t.uspb;prev=t.tick;uspb=t.uspb;}}
  function tickToSec(tick){
    let lo=0,hi=tmN-1;
    while(lo<hi){const m=(lo+hi+1)>>1;if(_tk[m]<=tick)lo=m;else hi=m-1;}
    return _sc[lo]+(tick-_tk[lo])/ppq*(_us[lo]/1e6);
  }
  const secToTempoMap=[];
  {let sec=0,prev=0,uspb=500000;
   for(const t of tempoMap){sec+=(t.tick-prev)/ppq*(uspb/1e6);secToTempoMap.push({sec,bpm:Math.round(60e6/t.uspb)});prev=t.tick;uspb=t.uspb;}}
  self.postMessage({type:'progress',phase:'pass2',pct:50});

  const pending={};
  let noteCap=524288,notePos=0,totalNotes=0;
  let allNotes=new Float32Array(noteCap*6);
  function growNotes(){const n=new Float32Array(noteCap*2*6);n.set(allNotes);allNotes=n;noteCap*=2;}
  const CC_CAP2=262144;
  let ccT=new Float32Array(CC_CAP2),ccType=new Uint8Array(CC_CAP2),ccCh=new Uint8Array(CC_CAP2);
  let ccD1=new Uint8Array(CC_CAP2),ccD2=new Uint8Array(CC_CAP2),ccCount=0;
  function pushCC(t,type,ch,d1,d2){if(ccCount>=CC_CAP2)return;ccT[ccCount]=t;ccType[ccCount]=type;ccCh[ccCount]=ch;ccD1[ccCount]=d1;ccD2[ccCount]=d2;ccCount++;}
  const activeChannelMasks=new Uint32Array(rawTracks.length);
  let _lastProg=0;
  function emitNote(s,e2,note,ch,vel,trackIdx){
    if(e2<=s)e2=s+0.015;
    if(notePos>=noteCap)growNotes();
    const b=notePos*6;
    allNotes[b]=s;allNotes[b+1]=e2;allNotes[b+2]=note;
    allNotes[b+3]=ch;allNotes[b+4]=vel/127;allNotes[b+5]=trackIdx;
    notePos++;totalNotes++;
  }
  for(let ti=0;ti<rawTracks.length;ti++){
    const d=rawTracks[ti];let p=0,tick=0,lastSt=0;
    while(p<d.length){
      let[dt,np]=varlen(d,p);p=np;tick+=dt;
      let st=d[p];
      if(st>=0x80){lastSt=st;p++;}else{st=lastSt;}
      const hi=st>>4,ch=st&0xF;
      if(hi===0x9){
        const note=d[p++],vel=d[p++];
        activeChannelMasks[ti]|=(1<<ch);
        if(vel>0){
          const k=(ti<<11)|(ch<<7)|note;
          if(!pending[k])pending[k]=[];
          pending[k].push({tick,vel});
        }else{
          const k=(ti<<11)|(ch<<7)|note;
          if(pending[k]&&pending[k].length){const on=pending[k].shift();emitNote(tickToSec(on.tick),tickToSec(tick),note,ch,on.vel,ti);}
        }
      }else if(hi===0x8){
        const note=d[p++];p++;
        const k=(ti<<11)|(ch<<7)|note;
        if(pending[k]&&pending[k].length){const on=pending[k].shift();emitNote(tickToSec(on.tick),tickToSec(tick),note,ch,on.vel,ti);}
      }else if(hi===0xB){const d1=d[p++],d2=d[p++];pushCC(tickToSec(tick),0xB,ch,d1,d2);}
      else if(hi===0xC){pushCC(tickToSec(tick),0xC,ch,d[p++],0);}
      else if(hi===0xD){pushCC(tickToSec(tick),0xD,ch,d[p++],0);}
      else if(hi===0xA){p+=2;}
      else if(hi===0xE){const lsb=d[p++],msb=d[p++];pushCC(tickToSec(tick),0xE,ch,lsb,msb);}
      else if(st===0xFF){
        const mt=d[p++];let[ml,mp]=varlen(d,p);p=mp;
        if(mt===0x2F){
          const endSec=tickToSec(tick);
          for(const k in pending){if(pending[k]){while(pending[k].length){
            const on=pending[k].shift();const ki=parseInt(k);
            const n=ki&127,c=(ki>>7)&15,trk=(ki>>11)&255;
            emitNote(tickToSec(on.tick),endSec,n,c,on.vel,trk);
          }}}
          p+=ml;break;
        }p+=ml;
      }else if(st===0xF0||st===0xF7){
        lastSt=0;let[ml,mp]=varlen(d,p);p=mp;p+=ml;
      }
    }
    self.postMessage({type:'progress',phase:'pass2',pct:50+Math.round((ti+1)/rawTracks.length*50),track:ti,totalSoFar:totalNotes});
  }

  /* sort */
  self.postMessage({type:'progress',phase:'pass2',pct:90});
  const N=notePos;
  const idx=new Uint32Array(N);
  for(let i=0;i<N;i++)idx[i]=i;
  let maxSec=0;
  for(let i=0;i<N;i++){const s=allNotes[i*6];if(s>maxSec)maxSec=s;}
  const B=Math.min((maxSec|0)+2,65536);
  const cnt=new Int32Array(B);
  for(let i=0;i<N;i++)cnt[Math.min(allNotes[i*6]|0,B-1)]++;
  const bstart=new Int32Array(B);
  for(let b=1;b<B;b++)bstart[b]=bstart[b-1]+cnt[b-1];
  const pos2=bstart.slice();
  for(let i=0;i<N;i++)idx[pos2[Math.min(allNotes[i*6]|0,B-1)]++]=i;
  for(let b=0;b<B;b++){
    const s=bstart[b],e=s+cnt[b];
    if(e-s>1){
      const sub=new Array(e-s);
      for(let j=0;j<e-s;j++)sub[j]=idx[s+j];
      sub.sort((a,v)=>allNotes[a*6]-allNotes[v*6]);
      for(let j=0;j<e-s;j++)idx[s+j]=sub[j];
    }
  }
  const sorted=new Float32Array(N*6);
  for(let i=0;i<N;i++){const si=idx[i],s6=si*6,d6=i*6;sorted[d6]=allNotes[s6];sorted[d6+1]=allNotes[s6+1];sorted[d6+2]=allNotes[s6+2];sorted[d6+3]=allNotes[s6+3];sorted[d6+4]=allNotes[s6+4];sorted[d6+5]=allNotes[s6+5];}

  self.postMessage({type:'chunk',buf:sorted.buffer,count:N,totalSoFar:N,final:true,
    fmt,numTracks,ppq,tempoMap,secToTempoMap,ccCount,
    ccT:ccT.buffer,ccType:ccType.buffer,ccCh:ccCh.buffer,ccD1:ccD1.buffer,ccD2:ccD2.buffer,
    activeChannelMasks:Array.from(activeChannelMasks)
  },[sorted.buffer,ccT.buffer,ccType.buffer,ccCh.buffer,ccD1.buffer,ccD2.buffer]);
}

/* ─── Message handler ─── */
self.onmessage = async function(e) {
  const u8 = new Uint8Array(e.data);
  /* Wait for WASM load attempt to complete */
  await _wasmLoadPromise;
  try {
    if (_wasmReady) {
      parseWithWasm(u8);
    } else {
      parseWithJS(u8);
    }
  } catch (err) {
    self.postMessage({ type: 'error', msg: err.message });
  }
};
