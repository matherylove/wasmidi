/**
 * SnappySynthIntegration.js
 *
 * Fixes aplicados (sync con SnappySynthDriver.js):
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
 *   #OPT6    — numVoices default 128
 *   #OPT7    — tc2s floor 0.001s
 *   #OPT9    — Robo proactivo de voces cuando RT > 85%
 *   #FIX-V   — _applySettings recorta voices[] cuando numVoices baja
 *   #FIX-S   — _steal() respeta límite numVoices en iteración
 *   #FIX-TS  — midiTimestampToAudioTime recalcula origin dinámicamente (sin drift)
 *   #FIX-READY — process() guard: this.regions (ready nunca se seteaba)
 *   #FIX-RT2 — rendering time via currentTime (AudioWorkletGlobalScope)
 *   #FIX-PANEL — ssPanel oculto por defecto; visible solo cuando Embedded Synth activo
 *   #FIX-LAYERS — numLayers = N AudioWorkletNodes separados, cada uno con su pool
 *                 de voces y su subconjunto de tracks MIDI; merge via GainNode
 *   #FIX-VOICES — slider polyphony sube a 1024 (por layer)
 *   #FIX-REG  — _workletReadyP cachea addModule() por AudioContext
 *   #FIX-SYNC — sendDword() hace broadcast a TODOS los layers
 *   #FIX-EXT  — Driver cargado como archivo externo (SnappySynthDriver.js)
 *   #SYSEX5  — ssBridge.sendSysEx(uint8array): expone SysEx al host
 *   #SYSEX6  — _fakeOutput.send() detecta 0xF0 y rutea a sendSysEx
 *   #BATCH1  — sendDwordBatch(events, trackIndex): N eventos en un solo postMessage
 */

// ── UI ─────────────────────────────────────────────────────────────────────────
(function () {
  const ssPanel = document.getElementById('ssPanel');
  if (!ssPanel) return;

  ssPanel.style.display = 'none';

  const style = document.createElement('style');
  style.textContent = `
    .ss-panel{font-family:monospace;font-size:.75rem;color:#e2e8f0;display:flex;flex-direction:column;gap:.35rem;}
    .ss-row{display:flex;align-items:center;gap:.5rem;flex-wrap:wrap;}
    .ss-label{color:#94a3b8;min-width:5rem;}
    .ss-val{color:#f8fafc;font-weight:600;}
    .ss-status{font-size:.62rem;font-weight:600;padding:.15rem .45rem;border-radius:5px;border:1px solid transparent;}
    .ss-status.idle   {background:rgba(167,139,250,.07);border-color:rgba(167,139,250,.18);color:rgba(196,181,253,.4);}
    .ss-status.loading{background:rgba(251,191,36,.07); border-color:rgba(251,191,36,.25); color:#fbbf24;}
    .ss-status.ok     {background:rgba(52,211,153,.07); border-color:rgba(52,211,153,.25); color:#34d399;}
    .ss-status.err    {background:rgba(251,113,133,.07);border-color:rgba(251,113,133,.22);color:#fb7185;}
    .ss-drop{border:1.5px dashed rgba(167,139,250,.3);border-radius:7px;padding:.45rem .65rem;text-align:center;font-size:.65rem;color:rgba(196,181,253,.4);cursor:pointer;transition:border-color 150ms,color 150ms;user-select:none;}
    .ss-drop:hover,.ss-drop.drag-over{border-color:rgba(167,139,250,.7);color:#c4b5fd;}
    .ss-url-row{display:flex;gap:.35rem;align-items:center;}
    .ss-url-input{flex:1;font-size:.63rem;padding:.2rem .4rem;border-radius:5px;border:1px solid rgba(167,139,250,.2);background:rgba(10,8,30,.7);color:#c4b5fd;min-width:0;}
    .ss-url-input::placeholder{color:rgba(196,181,253,.25);}
    .ss-btn{font-size:.62rem;font-weight:600;padding:.2rem .5rem;border-radius:5px;border:1px solid rgba(167,139,250,.22);background:rgba(99,63,210,.12);color:#a78bfa;cursor:pointer;white-space:nowrap;transition:background 120ms;}
    .ss-btn:hover{background:rgba(99,63,210,.28);}
    .ss-btn:disabled{opacity:.35;cursor:not-allowed;}
    .ss-divider{border:none;border-top:1px solid rgba(167,139,250,.1);margin:.1rem 0;}
    .ss-cfg-row{display:flex;align-items:center;gap:.4rem;flex-wrap:wrap;}
    .ss-cfg-lbl{font-size:.6rem;color:rgba(196,181,253,.38);min-width:4.5rem;}
    .ss-cfg-val{font-size:.62rem;color:#a78bfa;min-width:2.5rem;font-variant-numeric:tabular-nums;}
    .ss-slider{-webkit-appearance:none;appearance:none;height:3px;border-radius:999px;background:rgba(167,139,250,.2);outline:none;cursor:pointer;flex:1;min-width:50px;}
    .ss-slider::-webkit-slider-thumb{-webkit-appearance:none;width:11px;height:11px;border-radius:50%;background:#a78bfa;}
    .ss-tab-disabled{opacity:.35;pointer-events:none;}
  `;
  document.head.appendChild(style);

  ssPanel.className = 'midi-panel ss-panel';
  ssPanel.innerHTML = `
    <div class="ss-row">
      <span class="ss-label">SF2</span>
      <span class="ss-status idle" id="ssSFStatus">No SF2</span>
      <span class="ss-val" id="ssSFRegions" style="font-size:.65rem;color:#64748b;"></span>
      <button class="ss-btn" id="ssPanicBtn" title="All Notes Off">⏹ Panic</button>
    </div>

    <div class="ss-row">
      <span class="ss-label">Voices</span>
      <span class="ss-val" id="ssVoiceCount">0</span>
      <span class="ss-label" style="margin-left:.5rem;">RT</span>
      <span class="ss-val" id="ssRenderTime">0%</span>
      <span class="ss-label" style="margin-left:.5rem;">Workers</span>
      <span class="ss-val" id="ssWorkerCount">0</span>
    </div>

    <hr class="ss-divider">

    <div class="ss-drop" id="ssSFDrop">
      🎹 Drop an SF2 file here, or <u>click to browse</u>
      <input type="file" id="ssSFInput" accept=".sf2" style="display:none">
    </div>

    <div class="ss-url-row">
      <input class="ss-url-input" id="ssSFUrl" type="text" placeholder="https://…/soundfont.sf2">
      <button class="ss-btn" id="ssSFUrlBtn">Load URL</button>
    </div>

    <hr class="ss-divider">

    <div class="ss-cfg-row">
      <span class="ss-cfg-lbl">Master Vol</span>
      <input type="range" class="ss-slider" id="ssMasterVol" min="0" max="100" value="50">
      <span class="ss-cfg-val" id="ssMasterVolVal">50%</span>
    </div>

    <div class="ss-cfg-row">
      <span class="ss-cfg-lbl">Voices/Layer</span>
      <input type="range" class="ss-slider" id="ssNumVoices" min="8" max="1024" step="8" value="256">
      <span class="ss-cfg-val" id="ssNumVoicesVal">256</span>
    </div>

    <div class="ss-cfg-row">
      <span class="ss-cfg-lbl">Workers</span>
      <input type="range" class="ss-slider" id="ssNumLayers" min="1" max="8" step="1" value="2">
      <span class="ss-cfg-val" id="ssNumLayersVal">2</span>
    </div>

    <div class="ss-cfg-row">
      <span class="ss-cfg-lbl">Limiter</span>
      <button class="ss-btn" id="ssLimiterToggle" data-on="true">ON</button>
      <span class="ss-cfg-lbl" style="margin-left:.3rem;">Threshold</span>
      <input type="range" class="ss-slider" id="ssLimiterThresh" min="50" max="100" value="95">
      <span class="ss-cfg-val" id="ssLimiterThreshVal">0.95</span>
    </div>
  `;

  function _sfStatus(msg, cls) {
    const el = document.getElementById('ssSFStatus');
    if (!el) return;
    el.textContent = msg;
    el.className   = 'ss-status ' + (cls || 'idle');
  }

  let ctx = null;
  let _initP = null;
  let _sfArrayBuffer = null;
  let _sfName = null;

  const _cfg = {
    masterVol:        0.5,
    numVoices:        256,
    numLayers:        2,
    limiterEnabled:   true,
    limiterThreshold: 0.95,
  };

  let _layers = [];
  let _mergeGain = null;

  let _workletReadyP = null;

  async function _ensureWorkletLoaded() {
    if (_workletReadyP) return _workletReadyP;
    _workletReadyP = ctx.audioWorklet.addModule('SnappySynthDriver.js');
    return _workletReadyP;
  }

  async function _buildLayers(numLayers) {
    for (const l of _layers) {
      try { l.node.port.postMessage({ type: 'reset' }); } catch (_) {}
      try { l.node.disconnect(); } catch (_) {}
    }
    if (_mergeGain) { try { _mergeGain.disconnect(); } catch (_) {} }
    _layers = [];

    _mergeGain = ctx.createGain();
    _mergeGain.gain.value = 1 / Math.max(1, numLayers);
    _mergeGain.connect(ctx.destination);

    await _ensureWorkletLoaded();

    for (let i = 0; i < numLayers; i++) {
      const node = new AudioWorkletNode(ctx, 'snappy-synth', {
        numberOfInputs:    0,
        numberOfOutputs:   1,
        outputChannelCount:[2],
      });
      node.connect(_mergeGain);

      const layerIndex = i;
      node.port.onmessage = ({ data: d }) => {
        if (d.type === 'sf_loading') {
          if (layerIndex === 0) _sfStatus('Parsing\u2026', 'loading');
        } else if (d.type === 'sf_loaded') {
          if (layerIndex === 0) {
            _sfStatus('Loaded \u2714', 'ok');
            const r = document.getElementById('ssSFRegions');
            if (r) r.textContent = d.regionCount + ' regions';
          }
        } else if (d.type === 'sf_error') {
          _sfStatus('Error: ' + d.message, 'err');
        } else if (d.type === 'stats' && layerIndex === 0) {
          const vc = document.getElementById('ssVoiceCount');
          if (vc) vc.textContent = d.activeVoices;
          const rt = document.getElementById('ssRenderTime');
          if (rt && d.renderingTime !== undefined)
            rt.textContent = (d.renderingTime * 100).toFixed(1) + '%';
        }
      };

      node.port.postMessage({ type: 'init', settings: {
        masterVol:        _cfg.masterVol,
        numVoices:        _cfg.numVoices,
        numLayers:        1,
        limiterEnabled:   _cfg.limiterEnabled,
        limiterThreshold: _cfg.limiterThreshold,
      }});

      _layers.push({ node, index: i });
    }

    const wc = document.getElementById('ssWorkerCount');
    if (wc) wc.textContent = numLayers;

    if (_sfArrayBuffer) {
      for (const l of _layers) {
        const copy = _sfArrayBuffer.slice(0);
        l.node.port.postMessage({ type: 'load_sf_buffer', buffer: copy, name: _sfName }, [copy]);
      }
    }
  }

  async function init() {
    if (_initP) return _initP;
    _initP = _doInit();
    return _initP;
  }

  async function _doInit() {
    try {
      if (!window.AudioWorklet) throw new Error('AudioWorklet not supported');
      ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'playback' });
      if (ctx.state === 'suspended') await ctx.resume();
      _workletReadyP = null;
      await _buildLayers(_cfg.numLayers);
    } catch (err) {
      console.error('[SnappySynth] init error:', err);
      _sfStatus('Init failed: ' + err.message, 'err');
      _initP = null;
      throw err;
    }
  }

  function midiTimestampToAudioTime(ts) {
    if (!ctx || !ts) return ctx ? ctx.currentTime : 0;
    const origin = performance.now() - ctx.currentTime * 1000;
    return (ts - origin) / 1000;
  }

  // #FIX-SYNC: broadcast MIDI a TODOS los layers
  function sendDword(dword, time, trackIndex) {
    if (_layers.length === 0) return false;
    const t = time ?? (ctx ? ctx.currentTime : 0);
    if (trackIndex !== undefined && trackIndex >= 0) {
      _layers[trackIndex % _layers.length].node.port.postMessage({ type: 'midi', dword, time: t });
    } else {
      for (const l of _layers) l.node.port.postMessage({ type: 'midi', dword, time: t });
    }
    return true;
  }

  // #BATCH1 — Envía N eventos MIDI en un solo postMessage por layer.
  // events: Array<{ dword: number, time?: number }>
  // trackIndex: si está definido, reparte por layer (round-robin); si no, broadcast.
  function sendDwordBatch(events, trackIndex) {
    if (_layers.length === 0 || !events || events.length === 0) return false;
    const now = ctx ? ctx.currentTime : 0;
    // Normalizar tiempos ausentes
    const normalized = events.map(e => ({ dword: e.dword, time: e.time ?? now }));
    if (trackIndex !== undefined && trackIndex >= 0) {
      _layers[trackIndex % _layers.length].node.port.postMessage(
        { type: 'midi_batch', events: normalized }
      );
    } else {
      for (const l of _layers)
        l.node.port.postMessage({ type: 'midi_batch', events: normalized });
    }
    return true;
  }

  // #SYSEX5 — Envía un mensaje SysEx a todos los layers.
  // data: Uint8Array con o sin delimitadores F0/F7.
  function sendSysEx(data) {
    if (_layers.length === 0 || !data) return false;
    for (const l of _layers) {
      // Transferir como ArrayBuffer (copia) para evitar errores de compartición
      const copy = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
      l.node.port.postMessage({ type: 'sysex', data: copy }, [copy]);
    }
    return true;
  }

  function getCurrentTime() { return ctx ? ctx.currentTime : 0; }
  function isRunning()      { return !!(ctx && ctx.state === 'running' && _layers.length > 0); }

  async function resumeCtx() {
    if (!ctx) return false;
    if (ctx.state !== 'running') await ctx.resume();
    return ctx.state === 'running';
  }

  function loadSFBuffer(arrayBuffer, name) {
    if (_layers.length === 0) return;
    _sfArrayBuffer = arrayBuffer.slice(0);
    _sfName = name;
    for (const l of _layers) {
      const copy = _sfArrayBuffer.slice(0);
      l.node.port.postMessage({ type: 'load_sf_buffer', buffer: copy, name }, [copy]);
    }
  }

  function loadSFUrl(url) {
    if (_layers.length === 0) return;
    for (const l of _layers) {
      l.node.port.postMessage({ type: 'reload_sf', url });
    }
  }

  function panic() {
    for (const l of _layers) l.node.port.postMessage({ type: 'reset' });
  }

  function updateSetting(key, value) {
    _cfg[key] = value;
    if (key === 'numLayers') {
      if (ctx) _buildLayers(value);
      return;
    }
    for (const l of _layers)
      l.node.port.postMessage({ type: 'settings', settings: { [key]: value } });
  }

  const ssBridge = {
    init, sendDword, sendDwordBatch, sendSysEx,
    getCurrentTime, midiTimestampToAudioTime,
    isRunning, resumeCtx, loadSFBuffer, loadSFUrl, panic, updateSetting,
    get ready() { return !!(ctx && _layers.length > 0); },
  };

  // #SYSEX6 — _fakeOutput.send() detecta SysEx (status 0xF0) y rutea a sendSysEx
  const _fakeOutput = {
    name: 'SnappySynth (Embedded)',
    send(data, timestamp) {
      if (!data || data.length === 0) return;
      // SysEx: primer byte es 0xF0
      if ((data[0] & 0xFF) === 0xF0) {
        ssBridge.sendSysEx(data instanceof Uint8Array ? data : new Uint8Array(data));
        return;
      }
      const dword = (data[0] & 0xFF)
                  | (((data[1] || 0) & 0xFF) << 8)
                  | (((data[2] || 0) & 0xFF) << 16);
      const time = ssBridge.midiTimestampToAudioTime(timestamp);
      ssBridge.sendDword(dword, time);
    },
  };

  let _realMidiOutput = null;

  async function _ssActivateTab() {
    ['tabWmidi','tabMidiIn','tabNone'].forEach(id => {
      const el = document.getElementById(id);
      if (el) { el.classList.remove('active'); el.classList.add('ss-tab-disabled'); }
    });
    if (!ssBridge.ready) {
      try { await ssBridge.init(); } catch (e) { console.error(e); return; }
    } else {
      await ssBridge.resumeCtx();
    }
    _realMidiOutput    = window.midiOutput;
    window.midiOutput  = _fakeOutput;
    window.midiEnabled = true;
    ssPanel.style.display = '';
  }

  async function _ssDeactivateTab() {
    window.midiOutput  = _realMidiOutput;
    window.midiEnabled = !!_realMidiOutput;
    ssPanel.style.display = 'none';
  }

  const tabSS = document.getElementById('tabSS');
  if (tabSS) {
    ['tabWmidi','tabMidiIn','tabNone'].forEach(id => {
      const el = document.getElementById(id);
      if (el) el.addEventListener('click', () => {
        tabSS.classList.remove('active');
        ssPanel.classList.remove('on');
        _ssDeactivateTab();
      });
    });
    tabSS.addEventListener('click', async () => {
      ['panelWmidi','panelMidiIn','panelNone'].forEach(id => {
        const p = document.getElementById(id);
        if (p) p.classList.remove('on');
      });
      ['tabWmidi','tabMidiIn','tabNone'].forEach(id => {
        const el = document.getElementById(id);
        if (el) { el.classList.remove('active'); el.classList.add('ss-tab-disabled'); }
      });
      tabSS.classList.add('active');
      ssPanel.classList.add('on');
      if (typeof window.midiEnabled !== 'undefined') window.midiEnabled = true;
      await _ssActivateTab();
    });
  }

  const sfDrop  = document.getElementById('ssSFDrop');
  const sfInput = document.getElementById('ssSFInput');
  if (sfDrop) {
    sfDrop.addEventListener('click',    () => sfInput && sfInput.click());
    sfDrop.addEventListener('dragover', e => { e.preventDefault(); sfDrop.classList.add('drag-over'); });
    sfDrop.addEventListener('dragleave',  () => sfDrop.classList.remove('drag-over'));
    sfDrop.addEventListener('drop', e => {
      e.preventDefault(); sfDrop.classList.remove('drag-over');
      const file = e.dataTransfer.files[0];
      if (file) _loadSFFile(file);
    });
  }
  if (sfInput) {
    sfInput.addEventListener('change', () => {
      const file = sfInput.files[0];
      if (file) _loadSFFile(file);
    });
  }

  async function _loadSFFile(file) {
    if (!ssBridge.ready) {
      try { await ssBridge.init(); } catch (e) { _sfStatus('Init failed', 'err'); return; }
    }
    _sfStatus('Reading\u2026', 'loading');
    const buf = await file.arrayBuffer();
    ssBridge.loadSFBuffer(buf, file.name);
  }

  const sfUrlBtn   = document.getElementById('ssSFUrlBtn');
  const sfUrlInput = document.getElementById('ssSFUrl');
  if (sfUrlBtn && sfUrlInput) {
    sfUrlBtn.addEventListener('click', async () => {
      const url = sfUrlInput.value.trim();
      if (!url) return;
      if (!ssBridge.ready) {
        try { await ssBridge.init(); } catch (e) { _sfStatus('Init failed', 'err'); return; }
      }
      ssBridge.loadSFUrl(url);
    });
  }

  const panicBtn = document.getElementById('ssPanicBtn');
  if (panicBtn) panicBtn.addEventListener('click', () => ssBridge.panic());

  const masterVolSlider = document.getElementById('ssMasterVol');
  const masterVolVal    = document.getElementById('ssMasterVolVal');
  if (masterVolSlider) {
    masterVolSlider.addEventListener('input', () => {
      const v = parseInt(masterVolSlider.value) / 100;
      masterVolVal.textContent = masterVolSlider.value + '%';
      ssBridge.updateSetting('masterVol', v);
    });
  }

  const numVoicesSlider = document.getElementById('ssNumVoices');
  const numVoicesVal    = document.getElementById('ssNumVoicesVal');
  if (numVoicesSlider) {
    numVoicesSlider.addEventListener('input', () => {
      const v = parseInt(numVoicesSlider.value);
      numVoicesVal.textContent = v;
      ssBridge.updateSetting('numVoices', v);
    });
  }

  const numLayersSlider = document.getElementById('ssNumLayers');
  const numLayersVal    = document.getElementById('ssNumLayersVal');
  if (numLayersSlider) {
    numLayersSlider.addEventListener('input', () => {
      const v = parseInt(numLayersSlider.value);
      numLayersVal.textContent = v;
      ssBridge.updateSetting('numLayers', v);
    });
  }

  const limiterToggle = document.getElementById('ssLimiterToggle');
  if (limiterToggle) {
    limiterToggle.addEventListener('click', () => {
      const on = limiterToggle.dataset.on !== 'true';
      limiterToggle.dataset.on   = String(on);
      limiterToggle.textContent  = on ? 'ON' : 'OFF';
      limiterToggle.style.color  = on ? '#34d399' : 'rgba(196,181,253,.4)';
      ssBridge.updateSetting('limiterEnabled', on);
    });
  }

  const limiterThreshSlider = document.getElementById('ssLimiterThresh');
  const limiterThreshVal    = document.getElementById('ssLimiterThreshVal');
  if (limiterThreshSlider) {
    limiterThreshSlider.addEventListener('input', () => {
      const v = parseInt(limiterThreshSlider.value) / 100;
      limiterThreshVal.textContent = v.toFixed(2);
      ssBridge.updateSetting('limiterThreshold', v);
    });
  }

  window.ssBridge = ssBridge;
})();
