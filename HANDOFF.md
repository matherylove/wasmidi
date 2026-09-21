# WASMIDI — Handoff completo

**Revisión 37.** Escrito para alguien que llega sin ningún contexto. Leé las
secciones 1 a 5 antes de tocar código. La sección 5 es la más importante: dice
dónde está el problema hoy.

---

## 1. Qué es el proyecto

WASMIDI es un reproductor de **Black MIDI** que corre en el navegador,
desplegado en `https://midi.dekx.cc` (GitHub Pages). Black MIDI son archivos con
decenas de millones de notas y cientos de miles a millones de notas por segundo.

Dos mitades:

- **Síntesis de audio.** Un port a WebAssembly de **SnappySynthV2 (SSv2)**, un
  sintetizador de SoundFonts escrito en C para Windows. Vive en
  `third_party/snappysynthv2/`, se compila con Emscripten, y reparte el render
  de voces entre varios pthreads (Web Workers reales).
- **Visualización.** Un piano roll **horizontal** en WebGL2
  (`src/renderer/gl_renderer.cpp`) dentro de una app Qt6 también compilada a
  WASM. El tiempo corre en X y el pitch en Y.

### Las dos restricciones que mandan sobre todo lo demás

1. **No cambiar cómo suena SSv2.** El port tiene que sonar igual que el
   original. Una optimización que altere qué voz suena, cuándo, o cómo, no sirve.
2. **No cambiar cómo se ve el piano roll.**

Meta de rendimiento: **8192 voces estables con 8 workers.**

### Proyectos de referencia

El usuario provee dos árboles de código como referencia, no como dependencias:

- **SSv2 nativo original.** Referencia de sonido y de rendimiento. Tiene su
  propio handoff (`AGENT.MD`) con la lista de optimizaciones del motor.
  Hardware de referencia del autor: Ryzen 3700X (16 hilos).
- **BPFA** (Better Piano From Above). Referencia de arquitectura de rendimiento;
  ya usa SSv2 como motor.

El autor de SSv2 es accesible a través del usuario y ya aportó una corrección
decisiva (ver §6).

---

## 2. Restricciones del entorno (crítico)

**En el entorno donde se edita este código NO hay Qt6 ni emsdk.** Esto define
qué se puede afirmar y qué no:

- `src/mainwindow.cpp`, `src/mainwindow.hpp`, `src/qml/*` y
  `src/renderer/gl_renderer.cpp` **no se pueden compilar ni una vez**.
- El build de Emscripten completo tampoco.
- Nada se puede ejecutar ni perfilar desde acá.

Todo cambio en esos archivos llega al usuario sin verificar. El usuario compila,
prueba en el navegador y manda capturas. **Esas capturas son la única fuente de
verdad sobre el comportamiento real.**

### Lo que sí se puede verificar

```
cd third_party/snappysynthv2
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 Voice/voice.c
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 snappy_wasm_core.c
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 Parser/sfz_parser.c
cd ../..
node --check web/snappysynth-worker.js
node --check web/snappysynth_bridge.js

# Tests de host (extraen el código real de voice.c, no copias)
python3 tools/extract_ssw_schedule_logic.py
cc -O1 -Wall -o /tmp/h tools/ssw_schedule_harness.c -lm && /tmp/h
python3 tools/extract_voice_rebalance_logic.py
cc -O1 -Wall -pthread -o /tmp/b tools/worker_freelist_borrow_check.c && /tmp/b
python3 tools/extract_vor_redundancy_logic.py
cc -O1 -Wall -o /tmp/v tools/vor_redundancy_check.c && /tmp/v
cc -O1 -o /tmp/s tools/worker_freelist_share_check.c && /tmp/s
node tools/pump_slice_sim.mjs

# QML: solo balance de llaves, NO es compilación
python3 -c "import re;s=re.sub(r'//[^\n]*','',open('src/qml/Controls.qml').read());s=re.sub(r'\"(\\\\.|[^\"\\\\])*\"','\"\"',s);print(s.count('{'),s.count('}'))"
```

**`MANIFEST.sha256` cubre 41 archivos. Regenerarlo después de cada cambio** con
`python3 tools/regen_manifest.py`, o el usuario no puede validar el zip.

Entregar siempre el **proyecto completo como zip**, no parches.

---

## 3. Arquitectura

### Cadena de audio

```
midi-parser-worker.js  -> parsea el MIDI, agenda eventos con lookahead
        | scheduleBatch (lote de eventos por mensaje)
snappysynth-worker.js  -> Worker; hospeda el módulo WASM
        |  pump() -> ssw_render_queued_into() -> voice_render_float()
        |                                        \-> N pthreads: worker_thread()
        | ring SharedArrayBuffer
snappysynth-audio-worklet.js -> AudioWorklet, consume en quantums de 128
        |
snappysynth_bridge.js  -> estado compartido, leído por Qt vía EM_JS
```

**Diferencia estructural clave con el nativo:** el nativo recibe eventos uno por
uno en tiempo real (`SendDirectData`) y su hilo de render drena la cola entre
llegadas. **El port agenda por adelantado y despacha el bloque entero de eventos
de golpe antes de cada render.** Muchos problemas se explican por esta diferencia.

### Dentro de `worker_thread` (el hot path)

Cada ciclo de render, cada worker:

1. **Fase previa** (medida por `ALLOC`): drena sus colas de eventos, asigna
   voces para los note-on (con selección de regiones), y roba voces si el pool
   está lleno.
2. **Fase de render** (medida por `BUSY`): consume la cola de render y sintetiza
   las voces.

### Ruteo de eventos (importante)

`enqueue_event()` en `voice.c` enruta así:

```c
if (e.type == EVT_NOTE_ON || e.type == EVT_NOTE_OFF) w = g_note_worker_map[qch][e.key];
else                                                  w = g_channel_worker_map[qch];
```

Notas y eventos de canal van a **colas de workers distintas**. Consecuencias:

- Una nota nunca se interpone entre dos CC en la cola de canal.
- Un program change y un note-on del mismo canal se consumen **concurrentemente**
  en el mismo ciclo. Solo una frontera de render los ordena (ver §7, error 1).

### VOR (Voice Overlap Reduction)

Apila note-ons idénticos repetidos en una sola voz. Es lo que permite que
millones de notas por segundo quepan en 8192 voces. Dos note-on se apilan solo si
comparten `vor_token`, y ese token mezcla la generación **del canal**. **Cada CC
incrementa esa generación** (`vor_break_channel_sequence`), así que un CC entre
dos notas idénticas impide que se apilen. Es comportamiento del original, no del
port.

---

## 4. Telemetría

Los números viajan en el payload de stats, **no por consola** (imprimir desde un
worker a esa frecuencia frena el navegador; lo reportó el usuario).

Ruta para agregar un contador, cinco archivos:
`snappy_wasm_core.c` (export `ssw_*`) → `snappysynth-worker.js` →
`snappysynth_bridge.js` (`state.*`) → `mainwindow.cpp` (`EM_JS`) +
`mainwindow.hpp` (`Q_PROPERTY`) → `Controls.qml`. Agregar también el export en
`CMakeLists.txt` (`EXPORTED_FUNCTIONS`).

### Modo Debug

Botón **Debug** en el panel de SnappySynth. Apagado por defecto. Cuando está
apagado, los contadores caros **ni siquiera se recogen** (una compuerta única
`g_debug_metrics` en `voice.c`). Se puede prender **durante la reproducción** y
**no recarga la soundfont** — va por un camino directo al worker sin pasar por
`applySynthConfig()`. Esto es crítico: antes sí recargaba, y medir destruía el
estado que se quería medir.

Siempre visibles (baratos): `RATE`, `ACTIVE`, `FREE`, `WORKERS`, `VOICES/WKR`,
`REGIONS`, `UNDERRUNS`.

Solo con Debug:

| Contador | Qué mide |
|---|---|
| `LOAD %` | costo del bloque como % de su presupuesto de tiempo real |
| `BLOCK ms` | costo del último bloque |
| `ALLOC ms` | tiempo sumado entre workers en la fase previa (eventos, asignación, robo) |
| `BUSY ms` | tiempo sumado entre workers en el render de voces |
| `WACT` | cuántos workers tomaron al menos un chunk de la cola de render |
| `SIMD %` / `MISS <razón>` | porción de voces en el camino vectorizado y por qué fallan |
| `CC#n x% cnt` | controlador que se lleva más tiempo de despacho |
| `CCOL` | controladores colapsados por adyacencia (ver §8) |
| `PRESMP r/s skip` | regiones resampleadas / salteadas en el último presampleo |
| `STEALS/s`, `DROPPED` | robos por segundo, note-ons descartados |
| `RING %`, `LATE ms` | ocupación del ring de audio, retraso del pump |

**Contadores con defectos conocidos, no confiar en ellos:**
`RECYC` está contaminado por el rebalanceo de voces libres (da más de 100%).

---

## 5. ESTADO ACTUAL: dónde está el problema hoy

Hay **dos problemas distintos**. Se trataron como uno durante mucho tiempo y eso
causó la mayor parte de los errores de la sesión.

### Problema A — Carga inicial: la soundfont NO se precarga

**Síntoma:** tras cargar el SF2 por primera vez, el render va muy lento aunque
haya pocas voces. Empeora mientras suena el MIDI. Mejora al repetir el MIDI. Se
vuelve a romper al cambiar de soundfont. A veces desaparece al recargar la
página y vuelve después.

**Evidencia firme, en tres capturas del usuario con página recargada sin
cookies:**

- Con **30 voces activas** y 2.396 NPS: `LOAD 210%`, `BLOCK 33.5 ms`. Treinta
  voces no deberían costar nada.
- En ese estado **`ALLOC 3 ms`**, o sea que la fase de asignación no es el
  problema. **El tiempo está en el render** (`BUSY` domina).
- **En idle, con el SF2 cargado y `REGIONS 11`: `PRESMP 0/0 skip`.** El
  presampleo no recorre ninguna región — ni las procesa ni las saltea.

**Qué significa:** `sfz_apply_presampling()` corre con `inst->num_regions == 0`,
aunque después `ssw_region_count()` (que lee la misma variable) reporta 11.
Ninguna región recibe `resampled_data`, todas quedan con `is_resampled` falso,
y **cada voz resamplea en tiempo de render** contra el sample original. Eso
explica todo el síntoma.

**Principio del usuario, que manda sobre la solución:** *todo lo relacionado a
la soundfont debe estar precargado y precacheado en RAM antes de que arranque el
MIDI, incluida la soundfont completa.* Hoy la carga dice "SF2 ready" y deja
trabajo pendiente. **La carga no puede declararse terminada hasta que todo esté
materializado:** samples decodificados y resampleados, cachés de región
poblados.

**Siguiente paso concreto:** encontrar por qué `num_regions` es 0 cuando corre
el presampleo. La llamada está al final de `ssw_load_sf2()` en
`snappy_wasm_core.c`, después de `instrument = next`, y
`sf2_load_as_instrument()` falla si no cargó regiones, así que el orden parece
correcto y algo no cuadra. Hay **dos call sites** de `sfz_apply_presampling`
(uno en `ssw_init_ex`, otro en la carga); conviene saber cuál corre último. Una
vez arreglado, la predicción falsable es: `PRESMP` pasa a `11/0` y `SIMD` deja
de ser 0%.

Ya existe `voice_prewarm_note_caches()` (precalienta los cachés note/region al
cargar). No resolvió el problema, pero es parte del principio de "todo
precargado" y conviene dejarlo.

### Problema B — Zona densa con CC: el pool se satura

**Síntoma:** en zonas con muchísimas notas y CC, lag fuerte.

**Evidencia:** `ALLOC` es **20-40× `BUSY`** en cinco capturas distintas. Un
perfilado del navegador lo confirma aparte: `wasm-function[47]` se lleva el 70%
del tiempo, todo *self time*, bajo `invokeEntryPoint` → `handleMessage` de
`snappysynth-core.worker.js`. Eso es el punto de entrada de un **pthread**, o sea
`worker_thread` con todo inlineado por `-O3`. Pool saturado (`FREE` en decenas),
decenas de miles de robos por segundo, cientos de miles de notas descartadas.

**Principio del usuario:** *en un diseño monolítico, bindear un CC no debería
poder lagear el render por sí solo.* Aplicar un CC debería ser escribir un entero
en el estado del canal. Si lagea, hay algo en el camino que no es eso. Candidato:
cada CC rompe el apilado VOR (§3), y el costo aparece después, en las notas que
ya no se apilan y consumen voz propia.

**Plan acordado:** atacar la fase previa **y** la frontera de eventos a la vez,
detrás de un define único, sin tocar nada que decida qué voz suena. Si no
mejora, la fase queda descartada entera. Frentes, por margen real:

1. **Frontera de eventos.** `ssw_queue_events()` hace un `malloc` por lote y un
   `llround` sobre `double` por evento. Aritmética pura, precalculable, resultado
   idéntico.
2. **Drenado de eventos.** Trabajo por evento que sea invariante del lote.
3. **Asignación.** Ampliar cobertura de los cachés note/region existentes.
4. **Robo.** Margen mínimo; casi todo cambia qué voz muere.

---

## 6. Descartado con medición. NO volver a perseguirlo.

- **El DSP como causa de la zona densa.** `BUSY` despreciable frente a `ALLOC`.
- **Los CC como costo directo.** El **autor de SSv2** lo zanjó: *"un CC es un
  evento, una nota son dos; los CC son la mitad de caros que las notas"*. Con
  3,99M de CC contra 44,7M de notas son una fracción mínima. El `CC#7 100%` del
  perfilado por controlador era trivial: CC 7 era el único controlador presente.
- **El handler de CC.** Cada caso es un switch, una escritura y un
  `InterlockedExchange` sobre un flag con consumidor protegido. Ya es óptimo.
- **El reparto de la cola de render.** `WACT 23` de 24.
- **Vectorizar como solución.** `MISS INTERP 100%` es el caso **normal**:
  `no_interp` exige velocidad exactamente 1.0 (solo la nota en el root key, sin
  bend). Casi ninguna nota real califica.
- **El bloque de mezcla desacoplado.** No es costo fijo por llamada.
- **Voces por worker.** El nativo en el 5900X del usuario usaría los mismos 24
  workers.
- **`PK POLY` como medida de demanda de voces.** En Black MIDI la polifonía no
  se traduce en voces ni de cerca: capas de regiones, colas de release, y un
  flujo de notas que llega más rápido de lo que las releases se apagan. **El
  usuario corrigió este error dos veces.**
- **Fuga de voces.** `ACTIVE` sube y baja siguiendo la música.

---

## 7. Errores cometidos. No repetirlos.

1. **Quitar la frontera de render para bank/RPN/program change.** Parece seguro
   porque "la cola se drena en orden", pero notas y eventos de canal están en
   colas distintas (§3). Sin la frontera, los note-on resuelven contra
   banco/programa equivocado y **la nota se cae sin sonar**. Rollback:
   `SSW_FORCE_SELECTOR_BOUNDARIES=1`.
2. **Fragmentar el render por cada evento de estado.** Un bloque se volvía ~100
   llamadas a `voice_render_float()`.
3. **Punto fijo en el mapeo de columnas del culler visual.** Redondea distinto y
   descarta notas visibles. Usar el mismo `floor`/`ceil` en `double`.
4. **Agregar elementos a un `RowLayout` lleno** en QML. No hace wrap y desborda.
   Usar `Flow`.
5. **Ocultar media fila con `visible:`** condicionado a algo que no aplica a
   todos sus elementos. El panel salta y se pierde información.
6. **Comparar capturas que difieren en dos variables a la vez** (dos MIDIs
   distintos; primera carga contra sesión densa). Conclusiones inválidas.
7. **Colapsar CC por supersesión** en vez de por adyacencia. Rompe las rampas de
   automatización; el usuario lo oyó como respuesta de CC peor que el original.
8. **Anclar una edición en un patrón que aparece varias veces en el archivo.**
   Un reset de contador anclado en `if (!inst) return;` aterrizó en otra función
   y el contador midió basura durante varias revisiones. **Verificar dónde
   aterrizó toda inserción antes de entregar.**
9. **Leer mal el árbol del perfilador.** Se interpretó `invokeEntryPoint` como
   una llamada a la API por mensaje; es el arranque de un pthread.

---

## 8. Estado del código: qué está activo

| Mecanismo | Estado | Nota |
|---|---|---|
| Gobernador de segmentación | activo | fronteras en grilla, tope por carga medida |
| Fronteras de selector exactas | activo | correctitud, no optimización |
| Filtro de redundancia VOR | activo | suprime el break solo si el CC repite su valor; no ayudó en el MIDI de prueba porque sus CC cambian de verdad |
| Colapso de CC por adyacencia | **activo** | `SSW_COLLAPSE_CONTROLLER_BURSTS=1`; regla nativa; colapsó 1,78M sin mejorar |
| Rebalanceo de voces libres | activo | hecho por GPT, verificado |
| Prewarm de cachés note/region | activo | no resolvió el problema A |
| `voice_refresh_all_region_caches` | activo | bug real del multiplicador de pitch, pero no era el problema A |
| Cadencia del pump | activo | underruns 4.770 → 82, medido |
| Culler visual | **no conectado** | `src/renderer/note_raster_compositor.*`, verificado celda por celda pero cuesta más de un frame sin un caché de tiles en segundo plano |

Si el colapso de CC por adyacencia altera el sonido, apagarlo con el define.

---

## 9. Método: lo que funcionó y lo que no

Esta sesión acumuló **ocho hipótesis falladas** y **cinco contadores que medían
algo distinto de lo que su nombre prometía**. El patrón fue siempre el mismo:
confirmar una idea con una lectura propia sin validarla contra un caso de
respuesta conocida.

**Todo lo que avanzó vino del usuario:** aislar que sin CC no lagea, que mejora
al repetir, que se rompe al cambiar de soundfont, que el problema desaparece al
recargar, las capturas en idle, y el perfilado del navegador. **Priorizar sus
observaciones sobre la instrumentación nueva.**

Reglas prácticas:

- Antes de sacar conclusiones de un contador nuevo, verificar que da el valor
  esperado en un caso conocido (idle, un MIDI trivial).
- Una sola variable por comparación.
- Leer el código original antes de proponer: el motor es byte por byte idéntico
  en `enqueue_event`, `steal_voice_fast` y `alloc_voice`.
- Tras cualquier inserción automática en un archivo grande, imprimir la región
  y confirmar la función que la contiene.

### La herramienta que más destrabaría

Un **build de diagnóstico con `-O1` o `-g2`** en lugar de `-O3`. Con `-O3` todo
se inlinea en una función y el perfilador del navegador no puede desglosar. Con
símbolos mostraría `collect_note_regions_cached`, `steal_voice_fast`,
`alloc_voice` y el resampleo por separado, y nombraría al culpable directamente.

Cómo perfilar en Brave: F12 → Performance → grabar 5 s → pestaña **Bottom-up** o
**Call tree**, ordenar por Self time, elegir el hilo del worker.

---

## 10. Cómo trabajar con el usuario

- Prueba en **Brave, Windows, Ryzen 9 5900X (24 hilos)**. Brave sub-reporta
  `navigator.hardwareConcurrency` (reporta 7) por defensa anti-fingerprinting;
  por eso un pedido explícito de workers no se recorta contra los cores
  detectados.
- Escribe en español. Manda capturas del panel y de la consola.
- Prefiere arreglos a mediciones, y tiene razón en presionar por eso. Pero
  acepta instrumentar cuando se explica qué decide la medición.
- Fija las reglas de fidelidad y hay que respetarlas: si el original suena así,
  el port también.
- Si no carga el SF2, primero verificar HTTPS, ventana no privada y Shields de
  Brave: los pthreads necesitan `crossOriginIsolated`, que se logra con
  `web/coi-serviceworker.js`.
- Con cada update, actualizar este documento.

---

## 11. Pendientes a largo plazo (sin tocar)

- **GUI:** selector de colores estilo BPFA con popup RGB/hex y paleta en
  cookies; quitar Post-buffer; cambiar "Black MIDI Player" por "WASM-MIDI
  Toolkit / Update 0 Revision 19 (Test Build)"; note speed por ticks; dejar un
  solo gráfico NPS y quitar Skipped vel; quitar la barra superior; fondo
  "neural network"; limpiar referencias a terceros. El usuario pidió dejar esto
  para cuando el rendimiento esté resuelto.
- **Soundfonts GM completos suenan mal** y los drums no siguen el estándar.
  Viene del SSv2 original.
- **Carga de MIDI** lenta comparada con BPFA (`StreamingMidiParser`,
  `PreprocessedMidi` sin revisar).
- **Prerender de frames visuales** con caché de tiles en hilos de fondo, para
  que el culler visual rinda.
- **Precisión de CC** respecto del original: el usuario reportó que el pitch y
  el release a veces no se aplican a tiempo. Dos causas candidatas documentadas
  en el historial: la coalescencia sin timestamp en `enqueue_event` combinada
  con el agendado por lotes, y el gobernador colapsando a un render por bloque
  bajo carga.
