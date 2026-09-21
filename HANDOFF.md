# WASMIDI — Handoff completo

**Revisión 39.** Escrito para alguien que llega sin ningún contexto. Leé las
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
| `PRESMP seen/resmp/skip` | regiones recorridas / realmente resampleadas / no materializadas en el último presampleo |
| `STEALS/s`, `DROPPED` | robos por segundo, note-ons descartados |
| `RING %`, `LATE ms` | ocupación del ring de audio, retraso del pump |

`RECYC` fue corregido en la rev. 38: se recoge **solo con Debug** y cuenta una
voz cuando el barrido post-render realmente retira un `ENV_OFF`. Los movimientos
internos de freelists (refill/rebalance) ya no cuentan como reciclaje.

---

## 5. ESTADO ACTUAL: dónde está el problema hoy

Hay **dos problemas distintos**. Se trataron como uno durante mucho tiempo y eso
causó la mayor parte de los errores de la sesión.

### Problema A — Carga inicial: materialización de la soundfont

**Síntoma que sigue siendo la referencia de navegador:** tras cargar el SF2 por
primera vez, el render puede ir muy lento con pocas voces; repetir el MIDI mejora
y cambiar de soundfont puede reintroducirlo.

**Corrección importante de la rev. 38:** la lectura anterior de `PRESMP 0/0
skip` era inválida. El contador C ya tenía `g_presample_regions_seen`, pero el
panel mostraba únicamente `resampled/skipped`. Una región cuyo sample ya está a
la frecuencia destino se recorre correctamente y puede dejar ambos en cero. Por
tanto, aquella captura **no demostraba `num_regions == 0`**. El panel ahora muestra
`PRESMP <seen> seen <resmp> resmp / <skip> skip`. La condición útil es
`seen == REGIONS` y `skip == 0`; `resmp` puede ser 0 legítimamente.

**Bug real encontrado al revisar el código:** `resample_wav_data()` devuelve el
buffer original tanto cuando no hace falta convertir como cuando falla una
reserva. `sfz_apply_presampling()` marcaba en ambos casos `resampled_rate =
target_sample_rate`. Si la reserva había fallado con sample rates distintos, el
cache quedaba envenenado: las pasadas posteriores creían que el sample ya estaba
preparado y no reintentaban. En rev. 38 ese caso deja el cache sin preparar,
incrementa `skip` y la carga falla en vez de entrar a reproducción a medias.

**Contrato de carga nuevo (rev. 38):**

- La capa recién parseada se presamplea **antes** de incorporarse al instrumento
  vivo.
- Cada región debe tener `cache_entry`, PCM decodificado válido y, cuando el
  sample rate de origen difiere del synth, un `resampled_data` real a la tasa
  destino.
- `ssw_init_ex()` vuelve a validar/materializar la fuente existente cuando cambia
  la configuración del synth.
- Después se ejecutan `voice_refresh_all_region_caches()` y
  `voice_prewarm_note_caches()`.
- Si la condición no se cumple, `ssw_load_sf2()`/`ssw_init_ex()` devuelven fallo y
  el worker **no puede anunciar “SF2 ready”**.

Esto implementa el principio del usuario: todo el trabajo de la soundfont debe
quedar en RAM antes de comenzar el MIDI. **Aún falta validarlo en navegador**; no
se puede declarar resuelto desde este entorno. La prueba decisiva de rev. 38 es:
primera carga con `PRESMP seen == REGIONS`, `skip == 0`, y comparar `LOAD/BLOCK/
BUSY` en la primera reproducción contra la segunda. Si esas invariantes se cumplen
y la primera sigue siendo lenta, el presampleo deja de ser la hipótesis principal.

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

**Rev. 38 — primer pase seguro aplicado, pendiente de A/B en navegador:**

- `ssw_queue_events()` ya no obliga a `malloc/free` por cada lote. Mantiene un
  pool acotado de slabs reutilizables y conserva los más grandes para los lotes
  de 262.144 eventos. Rollback conjunto de esta frontera:
  `-DSSW_REV38_EVENT_HOTPATH=0`.
- La conversión `seconds -> frame` para timestamps no negativos usa el mismo
  redondeo de `llround()` (`floor(x + 0.5)` para este dominio) sin pagar la llamada
  matemática por evento. El mismo define restaura `llround()`.
- La instrumentación de `RECYC`, notas iniciadas y cobertura SIMD dejó de hacer
  atómicos globales por nota/voz cuando Debug está apagado. `worker_thread` toma
  una sola instantánea de la compuerta por ciclo. Esto elimina trabajo añadido
  por el port que no existe en el motor musical y no cambia ninguna decisión de
  voz.
- No se tocó `steal_voice_fast`, `alloc_voice`, selección de regiones, VOR,
  fronteras de selector ni el orden/timestamp de eventos.

Si `ALLOC` sigue dominando después de este pase, continuar dentro de
`worker_thread`: drenado/rebalance/cache locality. No modificar la política del
stealer sin una medición que lo exija.

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
10. **Sobrescribir el handoff sin releerlo.** Pasó dos veces. En la rev. 22 se
    reemplazó la sección de la rev. 21 de GPT en vez de agregar encima, y se
    perdió su trabajo documentado durante varias revisiones. En la rev. 37 se
    borró el documento entero y se reescribió de memoria, perdiendo la política
    de workers, la sección de memoria, los defines de rollback y el análisis de
    varios pendientes. Ambos se restauraron (secciones 12-14). **Regla: este
    documento solo se edita agregando o corrigiendo en el lugar; nunca se
    reemplaza un bloque entero sin haberlo leído y sin conservar su contenido.**

---

## 8. Estado del código: qué está activo

| Mecanismo | Estado | Nota |
|---|---|---|
| Gobernador de segmentación | activo | fronteras en grilla, tope por carga medida |
| Fronteras de selector exactas | activo | correctitud, no optimización |
| Filtro de redundancia VOR | activo | suprime el break solo si el CC repite su valor; no ayudó en el MIDI de prueba porque sus CC cambian de verdad |
| Colapso de CC por adyacencia | **activo** | `SSW_COLLAPSE_CONTROLLER_BURSTS=1`; regla nativa; colapsó 1,78M sin mejorar |
| Rebalanceo de voces libres | activo | hecho por GPT, verificado |
| Pool de slabs de eventos | **activo rev. 38** | evita malloc/free por lote; rollback `SSW_REV38_EVENT_HOTPATH=0` |
| Redondeo rápido de timestamps | **activo rev. 38** | equivalente a `llround` para tiempos admitidos no negativos; mismo rollback |
| Validación de materialización SF | **activo rev. 38** | no declara lista una capa con PCM/cache/resample incompleto |
| Telemetría hot solo con Debug | **corregido rev. 38** | sin atómicos por note-on/voz en reproducción normal |
| Prewarm de cachés note/region | activo | ahora corre después de validar/materializar la carga |
| `voice_refresh_all_region_caches` | activo | bug real del multiplicador de pitch; se conserva |
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

---

## 12. Referencia técnica detallada

La reescritura de la sección 1-11 resumió varias cosas que no deben perderse.
Esta sección las conserva **sin editar**, tal como estaban en la revisión 37
antes de la reescritura. Si algo acá contradice las secciones 1-11, **mandan las
secciones 1-11**, que están actualizadas.

Incluye: política de workers y pool de voces, memoria y `memory.grow`,
cross-origin isolation, trabajo verificado con sus defines de rollback
(`SS_EVENT_SPIN_ITERATIONS`, `WORKER_FREELIST_KEEP_MAX`), el detalle del culler
visual, y los pendientes 7.1 a 7.10 con su análisis completo.

### Política de workers y pool de voces

- `Workers = 0` → todos los hilos que reporte el navegador.
- `Workers = N` → N hilos. **Un pedido explícito no se recorta contra el conteo
  de cores detectado**, porque Brave *farblea* `navigator.hardwareConcurrency`
  por defensa anti-fingerprinting (una máquina de 24 hilos reporta 7).
- El pool de voces se **divide** entre los workers.
- `WORKER_FREELIST_REFILL` es un reparto proporcional, no 512 fijo. Motivo:
  `free_push()` devuelve la voz al stack **local** del worker, nunca al pool
  global, así que con lote fijo los primeros workers se quedaban con todo y el
  resto no podía ni asignar ni robar. Con 1024 voces y 24 workers, 22 quedaban en
  cero, y las notas enrutadas ahí se caían sin sonar.
- **Rebalance por ciclo** (rev. 21): lo anterior arregló el arranque pero no el
  régimen. Ahora cada worker devuelve al pool global, al inicio de cada ciclo,
  las voces libres que sus eventos en cola no van a usar. Ver §0 y
  `PORTING_STATUS.md` → "Per-cycle freelist rebalance".
- `PTHREAD_POOL_SIZE` tiene piso de 34 y **no se deriva de
  `hardwareConcurrency`**, por lo mismo del farbling.
- `_beginthreadex` ahora **comprueba su retorno**. Antes lo ignoraba: si el pool
  se agotaba, `g_threads[w]` quedaba NULL pero `g_worker_count` seguía contándolo,
  y cada render esperaba un done event que nunca llegaba.

#### Memoria

El módulo del synth usa pthreads, así que su heap es un `SharedArrayBuffer` y
`memory.grow` **sincroniza a todos los workers**: con 16–24 los congela a todos a
la vez. Se ve como un stall sin carga.

Lo que lo dispara escala con el conteo de workers: `CH_EVENT_QUEUE_SIZE` son
16384 eventos por *(worker, canal)*, o sea >150 MB de colas con 24 workers contra
~44 MB con 7. Por eso `INITIAL_MEMORY` está en 1 GB con paso de crecimiento
lineal de 256 MB, y `DEFAULT_PTHREAD_STACK_SIZE` en 256 KB.

#### Cross-origin isolation

Los pthreads necesitan `crossOriginIsolated=true`, que en GitHub Pages se logra
con `web/coi-serviceworker.js`. Sin HTTPS no hay Service Worker, sin Service
Worker no hay COI, sin COI no hay `SharedArrayBuffer`, y el SF2 no carga. Si el
usuario reporta "no cargan las soundfonts", verificar HTTPS, ventana no privada y
Shields de Brave **antes** de mirar código.

---

### 12.b Trabajo hecho y verificado (no tocar sin motivo)

**Gobernador de segmentación** (`ssw_render_queued_into` en
`snappy_wasm_core.c`). Las fronteras de render caen solo en una grilla múltiplo
de 8 frames, con tope adaptativo por carga medida. Los eventos selector
(bank/RPN/data entry/program change) toman frontera **exacta** cuando hay una
nota admitida en ese canal desde la última frontera — esto es correctitud, no
optimización (ver 3.1). Test: `tools/ssw_schedule_harness.c`, 14 grupos.

**Cadencia del pump** (`snappysynth-worker.js`). Rellena hacia un objetivo de
ocupación del 75% en tajadas de 12 ms, en vez de ráfagas de 16 bloques. Bajó la
ráfaga sincrónica de 116 ms a 20 ms. Underruns 4.770 → 82 medido en navegador.
Simulación: `tools/pump_slice_sim.mjs`.

**Culler visual** (`src/renderer/note_raster_compositor.{hpp,cpp}`). Port
horizontal del compositor de `NoteMeshCache` de BPFA. Devuelve **notas, no
geometría**, para que el shader original quede intacto — BPFA hornea bordes,
sombreado por velocity y anchos distintos para sostenidos, y eso cambiaría la
estética. Usa el orden de capas de BPFA (gana la que empieza después). El test
compara celda por celda contra un modelo del depth rule: **cero diferencias en 9
casos**. A 1M notas por viewport saca 76% de las instancias en 21,8 ms.

> **NO está conectado a `gl_renderer.cpp`.** 21,8 ms es más de un frame, y a 10k
> notas saca solo 2,6% de instancias por 0,5 ms, o sea que ahí es pérdida. Solo
> rinde con el caché de tiles en hilos de fondo (7.3). Al conectarlo: el pase de
> notas necesita depth test apagado o z constante, porque el orden lo impone el
> culler.

**Spin acotado antes del futex** (`compat/win_compat.h`). 384 iteraciones antes
de parkear. Correctitud verificada con 20.000 ciclos × 6 workers sin wakeups
perdidos. **El beneficio no está medido** — no se puede fuera del navegador.
Rollback: `-DSS_EVENT_SPIN_ITERATIONS=0`.

**Rebalance de freelists por ciclo** (`rebalance_worker_freelist` en
`voice.c`). Ver §0. Test: `tools/worker_freelist_borrow_check.c`, 5 grupos,
incluye el bloque real extraído. Rollback: `-DWORKER_FREELIST_KEEP_MAX=16384`
convierte `keep()` en "quedate con todo" y anula el efecto sin tocar código.

---

### 12.c Pendiente a largo plazo

Ordenado por lo que el usuario priorizó.

**7.1 — Notas que no suenan con múltiples workers.** **Hecho en rev. 21** (ver
§0), pendiente de captura del usuario. La hipótesis era correcta: un worker sin
voces libres solo roba las propias y descartaba notas teniendo voces libres en
otros workers. En vez de robar de la pila de otro (que obliga a hacer la pila
concurrente y toca el camino caliente), cada worker devuelve al pool global lo
que no va a usar, y el pool ya es lock-free. Test de host incluido. Si la
captura muestra `DROPPED > 0`, seguir la guía de §0.

**7.2 — Gráficos en la barra lateral.** El usuario pidió carga, latencia, voces y
steals/s como gráficos junto a los que ya existen (NPS, POLYPHONY), y el resto
como contadores. Hoy están todos como contadores de texto. Los datos ya están
expuestos como `Q_PROPERTY`. **Falta ubicar el componente de gráfico
reutilizable** — no está en `Controls.qml` ni en `StatsPanel.qml`.

**7.3 — Prerender de frames visuales.** La otra mitad de `NoteMeshCache`: 2–32
hilos de fondo, hasta 64 pantallas por adelantado, caché por (firma de settings,
tick de inicio), invalidación por contador de generación en el seek.

**7.4 — Bloque de mezcla desacoplado.** BPFA usa 8192 frames para la mezcla del
synth y 1024 para los buffers del dispositivo, con este comentario en
`SynthAudio.cpp`: *"Larger blocks amortize worker barriers and API transitions
while the separate 1024 frame WinMM buffers retain responsive playback/seek
behavior."* WASMIDI usa un solo `blockFrames` para ambas cosas.
**No es cambiar una constante:** `preferredRenderFrames()` ya tiene una nota de
que se intentó y se revirtió porque un render largo sincrónico dejaba sin atender
la entrega de eventos. Requiere sacar el render del hilo de mensajes primero.

**7.5 — Soundfonts GM completos suenan mal.** Con bancos e instrumentos distintos
se reproducen los samples incorrectos, y los drums no siguen el estándar MIDI.
**Viene del SnappySynthV2 original**, no del port. Sin revisar. Es selección de
preset/banco/región, no rendimiento.

**7.6 — Carga de MIDI lenta y con consumo alto comparada a BPFA.**
`StreamingMidiParser` y `PreprocessedMidi` de BPFA sin revisar.

**7.7 — GUI.** Todo pendiente, y el usuario pidió dejarlo para cuando el
rendimiento esté resuelto:
- Selector de colores como BPFA: eliminar el botón "Per Track", listar los
  colores en Colors, popup al hacer click con RGB y hex en tiempo real, paleta
  guardada en cookies.
- Quitar el botón "Post-buffer".
- Cambiar "Black MIDI Player" por "WASM-MIDI Toolkit" y debajo "Update 0
  Revision 19 (Test Build)".
- Note speed totalmente basado en ticks, no en tiempo. Si empeora el rendimiento,
  avisar.
- "NPS Live" y "NPS" son lo mismo: dejar "NPS". Sacar "Skipped vel".
- Quitar la barra superior (Dekxtopia, Home, MIDI Player WebGL2).
- GUI más responsiva a los colores activos del teclado. Falta el fondo "neural
  network" que antes estaba.
- Limpiar referencias a proyectos de terceros y a funcionamiento interno
  avanzado, para que sea apto para producción.

**7.9 — RESUELTO en la rev. 23.** Ver §0.

**7.10 — Los CC no se aplican como en el SnappySynthV2 original.** Reportado por
el usuario: las canciones se deforman, como si el pitch bend y el release no se
aplicaran correctamente o a tiempo. **Investigado, no arreglado.** Hay dos
causas candidatas y son independientes:

*Causa A: coalescencia de CC contra agendado por lotes.* `enqueue_event()` en
`voice.c` fusiona un CC con el evento inmediatamente anterior de la misma cola
cuando coinciden canal y número de CC:

```c
if (prev->type == EVT_CONTROL_CHANGE && prev->ch == e.ch && prev->key == e.key) {
    prev->value = e.value;
    prev->timestamp_qpc = e.timestamp_qpc;
    return;
}
```

Notar que **no compara timestamps**, a diferencia de la fusión de notas, que
exige `prev->timestamp_qpc == e.timestamp_qpc`. Quedan excluidos los CC 6, 38,
98, 99, 100, 101, 120, 121 y 123, pero **no** los continuos: 1, 7, 10, 11, 64,
71-74, 91, 93.

En el motor nativo esto es inofensivo, porque los eventos llegan en tiempo real
y el hilo de render drena la cola entre llegadas: los valores intermedios de una
rampa sobreviven. En WASMIDI los eventos de un bloque entero se despachan de
golpe **antes** de un único `voice_render_float()`, así que una rampa de CC11 de
0 a 127 a lo largo de 512 frames se colapsa a 127 aplicado al inicio del bloque.
La automatización continua pierde su forma. Esto es estructural del agendado por
lotes, no un bug puntual.

Verificar primero si el pitch bend sufre lo mismo: existe una rama
`prev->type == EVT_PITCH_BEND` en el mismo enqueue y hay que leer sus
condiciones. Si también fusiona sin comparar timestamp, explica directamente la
deformación reportada, porque un bend colapsado salta en vez de deslizarse.

*Causa B: el gobernador de segmentación colapsa a un solo render bajo carga.*
`ssw_render_queued_into()` reduce el presupuesto a 1 cuando la carga medida
pasa el umbral, y con presupuesto 1 **ningún** CC abre frontera: todos se
aplican al inicio del bloque. Con bloque de 512 eso es 11,6 ms de granularidad
justo cuando el material es denso. Además, las fronteras se cuantizan hacia
abajo en la grilla, así que un CC se aplica hasta un cuanto **antes** de su
posición real; el nativo lo aplica *después*, en el chunk siguiente. Temprano y
tarde no suenan igual.

*Cómo separarlas:* forzar `g_render_budget` a su máximo y ver si la deformación
persiste. Si desaparece, es B y se ataca con el presupuesto o con el bloque de
mezcla desacoplado (7.4). Si persiste, es A y hay que hacer que la fusión de CC
respete el timestamp como ya lo hace la de notas — con el costo de que la cola
se llena más, ver `CH_EVENT_QUEUE_SIZE`.

*Advertencia:* tocar esto cambia el sonido por definición, así que es el tipo de
cambio que hay que comparar contra el nativo antes y después, no solo medir.

**7.9 (histórico) — Rendimiento malo hasta cambiar de soundfont una vez.** La primera carga de SF2 parece dejar algo en un estado
peor que el que deja una recarga. Candidatos: layout o conversión de samples,
preprocesado de regiones, el caché de steal score, o el conteo de workers al
momento de la primera carga. Comparar la ruta de `ssw_load_sf2` en primera
carga contra recarga.

**7.8 — Frames del renderizador se detienen** en pasajes de 3M+ notas por
segundo. El ring visual arranca en `1 << 23` notas y **duplica**: a 12 bytes por
`VisualNote` eso es un VBO de 100 MB y el siguiente escalón 201 MB. Hipótesis sin
confirmar: una de esas duplicaciones falla en la GPU y el ring queda inservible.
Para confirmarlo hay que loguear en `ensureRingCapacity()` y `allocateRing()` el
tamaño pedido, el resultado de `glBufferData` y el error de GL.

---

---

## 13. Apéndice A: la revisión 21 de GPT (restaurada)

**Esta sección se había perdido.** En la revisión 22 se reemplazó el bloque de
la revisión 21 en lugar de agregarse encima, y el trabajo documentado de GPT
desapareció del handoff durante varias revisiones sin que nadie lo notara. Se
restaura acá **tal como GPT la escribió**, desde su copia original.

Algunas de sus conclusiones fueron revisadas después (por ejemplo, el rebalance
de freelists sigue activo y verificado; la lectura de `SIMD` que corrigió era
correcta y quedó incorporada). Ante contradicción con las secciones 1-11, mandan
esas.

### Revisión 21 (GPT)

**7.1 resuelto en el motor, con prueba de host. Sin verificar en navegador.**

- `voice.c`: `rebalance_worker_freelist()` corre al inicio de cada ciclo de
  worker. El worker se queda con tantas voces libres como eventos tiene en
  cola (tope 128) y devuelve el resto al pool global en **un solo CAS**
  (encadena el sobrante por `g_next_free` primero). El camino caliente
  (`free_pop`) no cambia. El lote de refill y el pre-refill del ciclo quedan
  acotados por la misma cifra para que los tres no se peleen.
- Invariante que compra, por ciclo: **ninguna voz sonando se roba mientras
  exista una libre en cualquier worker que no la vaya a usar**. Es exactamente
  lo que pedía 7.1.
- Contadores nuevos: `REBAL/s` (voces devueltas al pool por segundo) y
  `DROPPED` (note-ons que se cayeron sin sonar, acumulado). Antes la caída
  era invisible en todo stat. Van por los cinco archivos de la ruta de
  telemetría (§5), en el `Flow`, no en el `RowLayout`.
- Test: `tools/worker_freelist_borrow_check.c` incluye el bloque **real**
  extraído de `voice.c`. Reproduce el bug con rebalance apagado (1792 notas
  caen a robo/drop en 4096/8), luego prueba cero caídas en 1024..16384 voces ×
  2..24 workers, conservación (cada vid en exactamente un lugar), integridad
  de la cadena, y estrés de 8 hilos contra el pool CAS. Determinista, ~90 ms.
- CI: `.github/workflows/build-wasm.yml` ahora corre **todos** los tests de
  host de §2 antes del build de Emscripten. Un cambio de motor que rompa uno
  no llega a deploy.
- Arreglado `tools/ssw_schedule_harness.c`: le faltaban tres globals de
  telemetría de la rev. 20 y no compilaba. Pasa 14/14.

**Qué mirar en la próxima captura:** `DROPPED` debe quedarse en 0 en el
material que antes perdía notas melódicas. Si sube con `REBAL/s > 0` y
`FREE > 0`, el problema es otro (selector, 3.1). Si sube con `REBAL/s = 0`,
el rebalance no está corriendo y hay que mirar `worker_has_pending_events`.

---

---

## 14. Apéndice B: historial de revisiones 22 a 37 (sin editar)

Registro cronológico de cada revisión tal como se escribió en su momento.
**Contiene conclusiones que después se demostraron falsas** — por ejemplo, que
el presampleo estaba descartado (lo estaba por un contador roto, ver error 8 en
la sección 7), o que la rev. 23 resolvía el bug de primera carga (no lo hacía).
Se conserva porque muestra el razonamiento y qué se probó, y porque borrarlo
fue precisamente el error que obligó a restaurar el apéndice A.

**No usar este apéndice como fuente de estado actual. Para eso están las
secciones 1-11.**

### Rev. 37

#### Dónde está el tiempo, confirmado por dos vías independientes

`ALLOC` (tiempo de la fase previa al render, sumado entre workers) es 20-40x
`BUSY` (tiempo del bucle de consumo de la cola de render) en toda zona densa,
en cinco capturas distintas. Un perfilado del navegador lo confirma aparte:
`wasm-function[47]` se lleva el 70,4% del tiempo, todo *self time*, y está bajo
`invokeEntryPoint` -> `handleMessage` de `snappysynth-core.worker.js`, o sea el
punto de entrada de un pthread: es `worker_thread` con todo inlineado por -O3.

**El tiempo está dentro de `worker_thread`, en la fase previa al render:**
drenar colas de eventos, asignar voces, robar.

#### Descartado con medición. No volver a perseguirlo.

- **El DSP.** `BUSY` es despreciable frente a `ALLOC`, y `_emscripten_get_now`
  aparece con 5,5 ms en el perfilado, así que los contadores tampoco distorsionan.
- **Los CC.** El autor del motor y del MIDI de prueba lo zanjó: *"un CC es un
  evento, una nota son dos; los CC son la mitad de caros que las notas"*. Con
  3,99M de CC contra 44,7M de notas son una fracción mínima. El `CC#7 100%` que
  midió el perfilador por controlador era trivial: era el único CC presente.
- **El reparto de la cola de render.** `WACT 23` de 24 workers participando.
- **Vectorizar.** `MISS INTERP 100%` es el caso NORMAL, no una patología:
  `no_interp` exige velocidad exactamente 1.0, o sea solo la nota en el root key
  sin bend. Casi ninguna nota real califica.
- **El bloque de mezcla desacoplado (7.4).** No es costo fijo por llamada.
- **Voces por worker.** El hardware de referencia del repo de SSv2 es un 3700X
  (16 hilos) pero el usuario corre un 5900X (24), así que el nativo en su
  máquina usaría el mismo conteo de workers.

#### Intentos fallidos, con su causa

- **Colapso de ráfagas de CC por supersesión** (rev30). Bajó `ALLOC` de 32x a 8x
  pero la automatización salió escalonada y el usuario reportó la respuesta de
  CC como peor que el original. La fusión nativa exige adyacencia en el slot
  inmediatamente previo de la cola; supersesión no es la misma operación.
- **Colapso por adyacencia** (rev33), que sí es la regla nativa. Colapsó 1,78M
  de eventos y **no mejoró nada**, lo cual es consistente con que los CC no sean
  el costo.
- **Presampleo y `refresh_region_runtime_cache`** (rev23, rev29). Ninguno era la
  causa del bug de primera carga.
- **Prewarm de los cachés note/region** (rev36). No resolvió el bug de primera
  carga.

#### El bug de primera carga: abierto, intermitente

Primera carga de SF2 lenta, mejora al repetir el MIDI, se rompe al cambiar de
soundfont. **Desapareció tras recargar durante las pruebas y volvió después.**
Depende del estado de arranque. Sin causa identificada.

#### Plan acordado con el usuario para la próxima sesión

Atacar los tres componentes de la fase previa al render **más la frontera de
eventos**, todo a la vez, detrás de un define único para revertir de un tiro. Si
no mejora, la fase queda descartada entera.

**Regla que el usuario fijó y que manda sobre todo:** solo optimizaciones que no
puedan alterar qué voz se elige ni cuándo suena. Eso deja FUERA la política del
stealer y la selección de regiones, y deja DENTRO evitar trabajo repetido,
cachear recálculos y sacar cómputo invariante de los bucles.

Los cuatro frentes, en orden de margen real:

1. **Frontera de eventos.** `ssw_queue_events()` hace un `malloc` por lote y,
   por evento, un `llround` sobre `double` más la conversión a frames. Es
   aritmética pura: precalcularla da un resultado idéntico más barato.
2. **Drenado de eventos.** Buscar trabajo por evento que sea invariante del lote.
3. **Asignación.** Los cachés note/region ya existen; ampliar su cobertura no
   cambia qué región se elige, solo cuánto cuesta encontrarla.
4. **Robo.** Margen mínimo: casi todo lo tocable cambia qué voz muere. Limitarse
   a evitar recálculos dentro del bucle de sondeo.

#### Lo que más ayudaría antes de escribir código

Un build de diagnóstico con `-O1` o `-g2` en vez de `-O3`. Con -O3 todo se
inlinea en una función y el perfilador no puede desglosar; con símbolos
mostraría `collect_note_regions_cached`, `steal_voice_fast`, `alloc_voice` por
separado y nombraría al culpable directamente. El reparto relativo sigue siendo
válido aunque el build sea más lento.

#### Método, para quien continúe

Esta sesión acumuló ocho hipótesis falladas y cinco contadores que medían algo
distinto de lo que su nombre prometía (`GAP` invertido, `FREED` mal ubicado,
`RECYC` contaminado por el rebalanceo, `PRESMP` con el reset en otra función,
y una lectura errónea del árbol del perfilador). El patrón siempre fue el mismo:
confirmar una idea con una lectura propia sin validarla antes contra un caso de
respuesta conocida.

Las cosas que **sí** funcionaron vinieron del usuario: aislar que sin CC no
lagea, que mejora al repetir, que se rompe al cambiar de soundfont, y el
perfilado del navegador. Priorizar eso sobre la instrumentación propia.

---

#### Rev. Estado en la revisión 30 (leer primero)

#### Intento fallido: colapso de ráfagas de controlador

Se implementó un pase que, antes de despachar los eventos de un bloque,
descarta un valor de controlador que otro posterior del mismo canal y
controlador supersede. Idea: restaurar el agregado que el motor nativo recibe,
ya que allá los eventos llegan de a uno y la cola los colapsa contra el evento
previo mucho antes de acumular un bloque.

**Resultado numérico:** bueno. `CCOL 1.149.605` de 3.988.966 CC del archivo,
`ALLOC` bajó de ~32x `BUSY` a **8x**, y `UNDERRUNS 0` con NPS 687.872 y
CC/s 74.668.

**Resultado sonoro:** malo. El usuario reportó la respuesta de CC como
claramente menos precisa que la del SSv2 original. **Desactivado por defecto**
(`SSW_COLLAPSE_CONTROLLER_BURSTS` ahora es 0).

**Por qué falló, que es lo importante:** no es la misma operación que hace el
nativo. La fusión nativa exige que el CC anterior esté en el slot
**inmediatamente previo** de la cola, así que solo colapsa ráfagas realmente
consecutivas y **conserva los pasos intermedios de una rampa**. El pase
implementado escanea hacia adelante por toda la tanda y descarta cualquier valor
superseded, deteniéndose solo en una nota del mismo canal, así que una rampa de
expresión repartida a lo largo de un bloque pierde todos los pasos salvo el
último de cada tramo. Es un colapso a nivel **bloque** haciendo de sustituto de
uno a nivel **adyacencia**, y se oye como automatización escalonada.

**Para quien retome esto:** la dirección es correcta (8x contra 32x es una
mejora real y medida), el mecanismo no. Una versión fiel tendría que reproducir
**adyacencia**, no supersesión: colapsar solo cuando dos CC del mismo canal y
controlador son consecutivos en el flujo de despacho sin ningún otro evento de
ese canal en medio. Eso conserva las rampas y sigue matando el spam de valores
repetidos.

#### Lo que sigue abierto

- **Primera carga de SF2 lenta.** Sin causa identificada. `PRESMP 0/0` en ambas
  cargas descartó el presampleo, que era la única hipótesis viva.
- **Stall con CC densos.** El motor es byte por byte idéntico al original, así
  que la causa está en cómo el port entrega los eventos. El intento de esta
  revisión confirma que ahí está el costo, pero no encontró la forma fiel de
  reducirlo.
- `ALLOC` sigue en ~32x `BUSY` con el colapso apagado: el pool saturado y el
  robo fallido siguen dominando el bloque en material con CC.

---

#### Rev. Estado en la revisión 29 (leer primero)

#### Stall con CC densos: el motor NO es la causa

Se comparó el árbol original contra el port línea por línea. **La fusión de CC en
`enqueue_event()` y el break de VOR son byte por byte idénticos.** Mismo código,
mismas exclusiones (CC 6, 38, 98-101, 120, 121, 123), misma rotura de secuencia
por canal para todo evento que no sea nota.

Si el mismo código recibe el mismo flujo produce el mismo resultado, así que el
stall **se origina antes de `enqueue_event`**: el port le entrega al motor un
flujo distinto del que recibe el original. Esa es la parte que el port cambió —
el original recibe eventos uno por uno en tiempo real vía `SendDirectData`,
mientras el port agenda por lotes y despacha el bloque entero antes de renderizar.

Dos diferencias posibles y distinguibles:

- **Volumen:** que lleguen más eventos CC al motor de los que el archivo
  contiene, por duplicación o falta de filtrado en la ruta parser → worker.
- **Orden:** que el intercalado por timestamp rompa la adyacencia que la fusión
  necesita. Objeción a esta idea: si el original reprodujera el mismo archivo
  vería la misma secuencia, salvo que su player filtre antes de entregar.

**El número que decide:** eventos CC por segundo que entran a `enqueue_event`,
contra los que el archivo tiene por segundo. Iguales → el flujo es el mismo y
hay que buscar en otro lado. Órdenes de magnitud más → ahí está el stall, y
corregirlo es fiel por definición porque devuelve el flujo a lo que el original
recibe. **NO tocar el motor.**

El filtro de redundancia de VOR de la rev. 28 no cambió el cuadro
(`ALLOC 1.930 ms` contra `BUSY 66 ms`, 29x), lo cual confirma lo anticipado en
esa revisión: los CC de este archivo cambian de valor de verdad, no son
redundantes.

#### Primera carga de SF2: sigue sin resolverse

`PRESMP 0/0 skip` en **ambas** cargas, con `REGIONS 11`. Ni resampleadas ni
salteadas significa que el bucle de `sfz_apply_presampling()` no recorrió
ninguna región: o `num_regions` era 0 o `inst` era NULL en el momento de
llamar. Como `REGIONS 11` sí aparece en el panel, las regiones existen después.

**Descartado:** el presampleo no es la diferencia entre primera y segunda carga,
porque da 0/0 en las dos. La hipótesis del `cache_entry` sin poblar tampoco se
sostiene: habría dado `skipped > 0`, no 0/0.

Lo que sí está medido y es la pista firme: con las mismas voces activas,
`BUSY` 996 ms en primera carga contra 316 ms en segunda, y `BLOCK` 32,5 ms
contra 3,5 ms. **El trabajo real de render difiere ~3x con el mismo número de
voces**, así que las voces recorren otro camino de datos.

**Advertencia sobre esos números:** `BUSY` y `BLOCK` no cuadran aritméticamente
entre sí (316 ms sumados dentro de un bloque de 3,5 ms serían 90 workers con 24
configurados), así que uno de los dos no está midiendo el ciclo que se cree.
Verificar la sincronización de los snapshots antes de construir nada encima.

**Siguiente paso sugerido:** instrumentar `ssw_load_sf2()` directamente —
`instrument` nulo o no, y `instrument->num_regions` justo antes de llamar a
`sfz_apply_presampling()`, en ambas cargas. Hay dos call sites de esa función
(uno en `ssw_init_ex`, otro en la carga) y conviene saber cuál corre último,
porque el segundo pisa los contadores del primero.

---

#### Rev. Estado en la revisión 28 (leer primero)

#### La causa real, aislada por el usuario

Dos pruebas suyas delimitaron el problema mejor que toda la instrumentación:

- MIDI de millones de notas por segundo, 8192 voces saturadas, cientos de miles
  de robos por segundo: **no lagea**.
- MIDI con cientos de miles de **CC** por segundo: **lagea**.
- En ese MIDI, `ACTIVE` sube y baja siguiendo la música: **no hay fuga de voces**.

Eso descarta, definitivamente: el DSP, el stealer, las barreras, el bloque de
mezcla desacoplado, el pool de voces y la ruta de note-off. Todo lo que se
persiguió antes eran síntomas de un pool legítimamente saturado.

#### El mecanismo

```c
static inline uint16_t vor_current_event_token(int ch, int key) {
    return (uint16_t)(g_vor_key_generation[ch][key] ^
                      (uint16_t)(g_vor_channel_generation[ch] * 257u));
}
static inline void vor_break_channel_sequence(int ch) { ++g_vor_channel_generation[ch]; }
```

VOR apila note-ons idénticos en una sola voz, y es lo que permite que millones
de notas por segundo quepan en 8192 voces. Dos note-on solo se apilan si
comparten `vor_token`, y ese token mezcla la generación **del canal**.

En `enqueue_event()`, **todo** evento que no sea nota llamaba a
`vor_break_channel_sequence()`. Un solo CC entre dos note-on idénticos les da
tokens distintos y **el apilado no ocurre**. Con CC densos, VOR queda desactivado
de hecho: cada nota repetida se lleva su propia voz, el pool satura, el robo se
dispara y `ALLOC` domina el bloque.

#### El arreglo

Romper es correcto cuando el CC cambia algo: dos notas separadas por un cambio
real de volumen o pan no son idénticas y no deben apilarse. Es inútil cuando el
valor es el que el canal ya tenía.

`vor_event_is_redundant()` en `voice.c` suprime el break solo para CC y pitch
bend que repiten su valor actual. **Fiel por construcción:** si el estado del
canal no cambió, las notas realmente son idénticas y se apilan igual que sin el
evento. Los CC de secuencia (6, 38, 98-101, 120-127) nunca son redundantes,
porque repetirlos es una segunda acción real, no un no-op.

    python3 tools/extract_vor_redundancy_logic.py
    cc -O1 -Wall -o /tmp/vor tools/vor_redundancy_check.c && /tmp/vor

11 grupos, incluidos independencia por canal y por controlador, sustain on/off,
pitch bend y limpieza en reset. El test extrae el código real de `voice.c`, no
una copia.

#### Limitación, y cómo se mide

**Esto solo ayuda si los CC son redundantes.** El test 11 lo deja explícito: una
rampa de 128 valores distintos sigue rompiendo 128 veces. Si el archivo del
usuario tiene CC que cambian de verdad a esa densidad, el apilado se rompe igual
y hace falta otra respuesta — probablemente cuantizar el break a la grilla de
render en vez de por evento.

Si tras esta revisión el MIDI con CC sigue lageando, esa es la conclusión y no
hay que volver a mirar voces, stealer ni DSP.

---

#### Rev. Estado en la revisión 27 (leer primero)

#### El hallazgo firme: el costo está en asignar voces, no en el DSP

Zona densa, 8192 voces: `ALLOC 2.243 ms` contra `BUSY 68 ms`. **33× más tiempo
asignando que renderizando.** Con `FREE 95`, `STEALS/s 21.505` y
`DROPPED 282.666`, los workers pasan el bloque entero sondeando en busca de
víctimas de robo que la guarda de prioridad rechaza.

Esto descarta, con medición: vectorizar el DSP, el bloque de mezcla desacoplado
(7.4), y la hipótesis de voces-por-worker.

**El stealer NO se toca.** Se verificó en el árbol original que `alloc_voice()`
también descarta la nota cuando el robo falla, sin fallback. Agregar uno sería
apartarse del original, y el usuario pidió fidelidad.

#### Error corregido en esta revisión: el contador `FREED` mentía

`FREED 0%` fue un falso positivo. Estaba mal de dos formas:

- **Mal ubicado.** El contador vivía en `free_voice()`, que tiene una sola
  llamada en todo el archivo. La ruta **normal** de reciclado es el barrido
  posterior al render en `publish_worker_done()`, que llama a `free_push()`
  directamente. Las voces sí volvían, por un camino no instrumentado.
- **Mal planteado.** Una voz no se libera cuando llega su note-off, sino cuando
  la envolvente de release termina de caer, uno o varios bloques después, y más
  tarde aún con pedal de sustain. Un porcentaje por ciclo entre note-offs y
  liberaciones no significa nada en ningún bloque aislado.

El indicio que lo delataba estaba en la misma captura: `FREE 95` estable. Si de
verdad no se liberara ninguna voz, `FREE` habría caído a 0 en el primer segundo
y se habría quedado ahí.

**Es el segundo contador mal planteado de la sesión**, después de `GAP`
invertido. Lección repetida: una métrica nueva se valida contra un caso donde se
conozca la respuesta esperada, antes de sacar conclusiones de ella.

#### Reemplazo: `RECYC %`, acumulativo

Ahora cuenta en `free_push()`, o sea en **ambas** rutas de reciclado, y compara
acumulados: voces recicladas contra note-ons que consiguieron voz.

| RECYC | Conclusión |
|---|---|
| Justo bajo 100, estable | Las voces vuelven bien. El retraso es simplemente las voces sonando ahora. El pool no alcanza para el material y el nativo con el mismo cap haría lo mismo: no hay fuga. |
| Bajando sostenidamente | Las voces no vuelven. Ahí sí hay fuga, y el candidato es la coalescencia de eventos con agendado por lotes (7.10). |

**Comprobación más barata antes de instrumentar nada más:** `PK POLY` en el
panel de información del archivo dice cuántas voces simultáneas pide el material
de verdad. Si supera 8192, el pool simplemente no alcanza y no hay nada que
arreglar en el motor.

---

#### Rev. Estado en la revisión 26 (leer primero)

#### El costo NO está en el DSP. Está en asignar voces.

Medido en zona densa, 16 workers, 8192 voces:

| | |
|---|---|
| ALLOC | **1.735 ms** |
| BUSY | **56 ms** |
| BLOCK | 106,9 ms |
| WACT | 16 de 16 |
| FREE | 48 |
| STEALS/s | 21.218 |
| DROPPED | 192.736 |

**31× más tiempo asignando que renderizando.** 1.735 ms repartidos en 16 hilos
son ~108 ms cada uno, que es exactamente el `BLOCK`: los workers pasan el bloque
entero escaneando en busca de víctimas de robo.

Esto invalida tres líneas de trabajo que estuvieron sobre la mesa:

- **Vectorizar el DSP no serviría de nada.** El trabajo no está ahí.
- **El bloque de mezcla desacoplado (7.4) tampoco ataca esto**, porque no es
  costo fijo por llamada.
- La hipótesis de voces-por-worker ya se había caído: el hardware de referencia
  del repo de SSv2 es un 3700X (16 hilos), pero el usuario corre un 5900X (24),
  así que el nativo en su máquina usaría el mismo conteo de workers y las mismas
  voces por worker. La fragmentación no explica la diferencia.

#### El mecanismo del acantilado

`steal_voice_fast()` recorre `wd->active[]` con esta guarda:

```c
if (!low_cap_steal && v->env_state != ENV_RELEASE && !v->note_off_received &&
    (v->owner_channel != ch || v->key != new_note_key) &&
    victim_priority >= incoming_priority) { saltar }
```

En material denso casi todas las voces tienen volumen parecido, así que la
condición se cumple para casi todas y la función devuelve −1. Encima
`probe_cap` cae a 128 cuando la presión pasa el percentil 99. Cada note-on
fallido paga hasta 128 sondeos **para no encontrar nada**: con 21.218 robos por
segundo más 192.736 fallos, son millones de sondeos inútiles por segundo.

**El original hace exactamente lo mismo y también descarta la nota** — no tiene
fallback. Se verificó en `alloc_voice()`/`steal_voice_fast()` del árbol
original. Por lo tanto **agregar un fallback sería apartarse del original, no
corregirlo**, y el usuario pidió fidelidad. NO tocar el stealer.

#### Lo que falta explicar, y el instrumento de esta revisión

Si el stealer, el conteo de workers y las voces por worker son idénticos al
nativo, algo alimenta la presión más rápido acá. Es un lazo: menos throughput →
las voces no se liberan a tiempo → la presión queda sobre el percentil 99 →
`probe_cap` en 128 → el robo falla → más descartes → más presión.

Candidato directo, y es un problema introducido por el port: la coalescencia de
eventos con agendado por lotes (ver 7.10). Si los note-offs se fusionan o se
aplican al inicio del bloque en vez de a su offset, **las voces viven de más**.

**Instrumento agregado:** `FREED %`, voces efectivamente liberadas como
porcentaje de note-offs consumidos en el ciclo. Rojo bajo 90%.

| FREED | Conclusión |
|---|---|
| < 90% sostenido | Los note-offs no liberan voces. La fuga está en esa ruta, el stealer es víctima y no causa. Arreglar ahí y el acantilado desaparece solo. |
| ~100% | Las voces sí vuelven y la presión viene del volumen de notas puro. Entonces el techo es throughput general y hay que volver a §4. |

---

#### Rev. Estado en la revisión 25 (leer primero)

#### El reparto de la cola NO es el problema

`WACT 23` de 24 workers en zona densa. Mi sospecha sobre `pop_chunk` queda
descartada: el pool sí participa.

Pero eso hace que el resto de los números digan algo nuevo:

| | Zona densa, post-recarga |
|---|---|
| ACTIVE | 8.090 |
| WACT | 23 de 24 |
| BUSY | 63 ms |
| BLOCK | 85,2 ms |
| STEALS/s | 29.774 |
| DROPPED | 219.220 |
| FREE | 92 |

23 workers participan pero suman apenas 63 ms de trabajo mientras el bloque
tarda 85,2 ms de reloj. Repartidos en 23 hilos eso serían ~2,7 ms de pared.
**Faltan ~82 ms sin explicar.**

#### Dónde están esos 82 ms

`BUSY` arranca **justo antes del bucle de consumo de la cola de render**, o sea
que **no cuenta la fase de eventos**: drenar las colas de canal, asignar voces y
robar. Con el pool agotado (`FREE 92`) y 29.774 robos por segundo, esa fase hace
un trabajo enorme que es invisible para `BUSY` pero está entero dentro de
`BLOCK`.

**Instrumento agregado en esta revisión:** `ALLOC`, tiempo sumado entre workers
en esa fase previa. Se pone rojo cuando supera a `BUSY`. Lectura:

| ALLOC vs BUSY | Conclusión |
|---|---|
| ALLOC >> BUSY | El costo está en asignación de voces y robo, no en el DSP. Atacar el stealer y el agotamiento del pool, no vectorizar. |
| ALLOC ~ 0 | El tiempo está en las barreras y el trabajo fijo por llamada. Ahí sí entra el bloque de mezcla desacoplado (7.4). |

Si `ALLOC` resulta dominante, notar que **vectorizar el DSP no serviría de
nada**: el trabajo no está ahí. Sería la tercera pista que se descarta midiendo.

#### Sigue abierto

**El bug de primera carga NO está resuelto.** El arreglo de la rev. 23
(`voice_refresh_all_region_caches`) es correcto en sí mismo pero no era la
causa. Reportado por el usuario tras probar la rev. 24. Sospechoso vigente:
`sfz_apply_presampling`. Verificación pendiente: contador de regiones con
`is_resampled` verdadero tras cada carga; si la primera da 0 y la segunda da
todas, confirmado.

---

#### Rev. Estado en la revisión 24 (leer primero)

#### Corrección de un error de interpretación mío

`MISS INTERP 100%` **no es un bug, es el caso esperado.** `no_interp` exige una
relación de velocidad exactamente 1.0, o sea solo la nota en el root key del
sample sin pitch bend. En música real casi ninguna nota cumple eso, así que el
100% es normal. Lo interpreté como patología durante dos revisiones. El arreglo
de la rev. 23 (`voice_refresh_all_region_caches`) es correcto en sí mismo — el
multiplicador de pitch se aplicaba tarde y eso afectaba el sonido — pero **no
era la causa del bug de primera carga**, que sigue abierto.

#### El bug de primera carga, medido

Misma soundfont, mismo material, prácticamente las mismas voces activas:

| | 1ª carga | 2ª carga |
|---|---|---|
| ACTIVE | 5.209 | 5.022 |
| BUSY | **609 ms** | **5 ms** |
| BLOCK | 31 ms | 3,5 ms |
| LOAD | 269% | 31% |

Con el mismo conteo de voces, el trabajo real de los workers difiere ~100×.
Eso no puede ser planificación ni paralelismo: cada voz recorre un camino de
datos distinto. Sospechoso principal:
`sfz_apply_presampling(instrument, g_cfg.sample_rate)`. Si en la primera carga
no se aplica o se aplica con la tasa equivocada, `is_resampled` queda falso y
cada voz resamplea en tiempo de ejecución sobre el sample original; al cargar
otra soundfont la región ya quedó presampleada. **Verificación pendiente:** un
contador de cuántas regiones tienen `is_resampled` verdadero tras cada carga.
Si la primera da 0 y la segunda da todas, confirmado.

#### El paralelismo es malo siempre; el bug de primera carga lo camuflaba

`BUSY` no estaba roto: lee 0 cuando el bloque es barato. Ahora que funciona,
paralelismo efectivo = BUSY / BLOCK.

**Cuidado con qué se compara contra qué.** Entre la captura de primera carga y
la de sesión densa cambian DOS variables a la vez (primera-vs-segunda carga, y
liviano-vs-denso), así que esa comparación no sirve para aislar el paralelismo.
La comparación válida es entre las dos capturas que son ambas post-recarga:

| Escenario | ACTIVE | BUSY | BLOCK | Workers efectivos (de 24) |
|---|---|---|---|---|
| post-recarga, liviano | 5.022 | 5 ms | 3,5 ms | **1,4** |
| post-recarga, denso | 8.182 | 65 ms | 22,8 ms | **2,9** |
| primera carga, liviano | 5.209 | 609 ms | 31 ms | ~20 (ver abajo) |

O sea: **después de recargar, el paralelismo es malo en los dos casos.** El ~20
de la primera carga NO es buen paralelismo, es un artefacto del bug de primera
carga: al encarecer cada voz ~100×, los chunks duran lo suficiente como para que
todos los workers alcancen a tomar uno. Cuando el trabajo por voz vuelve a su
costo real, el reparto no llega a involucrar al pool.

Son dos problemas separados, y el segundo estaba camuflado por el primero:

1. **Primera carga:** ~100× de costo por voz. Sospechoso `sfz_apply_presampling`.
2. **Siempre:** 24 workers rinden como ~3. Este es el techo real de throughput y
   el que bloquea la meta de 8192 voces con 8 workers.

En la sesión densa además `FREE 0`, `STEALS/s 87.793` y `DROPPED 309.544`: el
pool está agotado encima de todo lo anterior.

**Instrumento agregado en esta revisión:** `WACT`, cuántos workers distintos
consumieron al menos un chunk de la cola en el último ciclo. Se pone rojo bajo
la mitad del conteo configurado. Junto a `BUSY` separa las dos lecturas:

| WACT | Lectura |
|---|---|
| ~24 con BUSY bajo | Todos participan pero hay poco trabajo; el costo está en las barreras y el trabajo fijo por llamada. |
| ~3 con BUSY alto | El reparto de la cola no involucra al pool. Mirar `pop_chunk` en `worker_thread`: se dimensiona como `max(base, render_size / g_worker_count)`, así que con chunks grandes los primeros workers en despertar se llevan todo antes de que los demás lleguen. |

---

#### Rev. Qué se hizo en la revisión 23 (leer primero)

**7.9 resuelto: el bug de la primera carga de soundfont.** Era la causa del
`SIMD 0%`, del `MISS INTERP 100%`, y de la aparente variación de rendimiento
entre soundfonts. Los tres eran el mismo bug.

`refresh_region_runtime_cache()` pliega `sample_rate / g_audio.sample_rate`
dentro de `cached_pitch_base_multiplier`, y tenía **una sola llamada en todo
voice.c**, dentro de `voice_init_with_count()`. En la primera soundfont las
regiones se crean *después* de esa llamada, así que nunca reciben la
corrección. Consecuencia: todas las voces reproducen a una relación distinta
de 1.0, `no_interp` es falso para todas, y **el 100% cae al bucle escalar**.
Cargar una segunda soundfont volvía a entrar a init con regiones presentes y lo
arreglaba en silencio: de ahí "el primer SF2 siempre carga mal, luego de cargar
otro mejora", y el 400% de carga en sesiones sin trabajo real.

Arreglo: `voice_refresh_all_region_caches()` en `voice.c`, llamada desde
`ssw_load_sf2()` justo después de `sfz_apply_presampling()`. El multiplicador de
pitch también fija la velocidad de reproducción, así que esto es tanto una
corrección de **sonido** como de rendimiento — antes de esto la primera
soundfont sonaba con el pitch mal.

**Qué mirar en la próxima captura:** con la primera soundfont recién cargada,
`MISS INTERP` debería dejar de ser 100% y `SIMD %` subir. Si eso pasa, la
decisión sobre el resampler vectorizado queda en suspenso hasta volver a medir,
porque la medición que la motivaba estaba contaminada por este bug.

**Sigue abierto y sin explicar:** `BUSY 0ms` con `BLOCK 101.5ms`. El reloj no es
el problema (`QueryPerformanceCounter` da nanosegundos con frecuencia 1e9, y la
aritmética no trunca ni desborda). O el contador tiene un camino no visto, o los
workers realmente no consumen la cola. Quedó a medias un contador de
participación de workers que lo separa: cuenta cuántos workers distintos
tomaron al menos un chunk. Si reporta 24 con BUSY en 0, el timer está mal; si
reporta 1 o 2, el pool no reparte la cola y ese es el bug real.

---

#### Rev. Qué se hizo en la revisión 22 (leer primero)

**La pregunta abierta de §4 quedó respondida: `SIMD 0%`.** Casi ninguna voz
entra al camino vectorizado, y por eso el DSP corre escalar. Medido en
navegador junto con `BUSY 63ms` contra `BLOCK 87.9ms` con 24 workers: si el
render de voces sumara 63 ms entre 24 hilos mientras el bloque tarda 87,9, el
tiempo no está ahí, está en el trabajo fijo y las barreras. Pero la causa raíz
de que el bloque sea tan caro es que nada se vectoriza.

Las guardas del camino SIMD por voz exigen, **todas a la vez**:

```c
vor_fast_ok && f_start == 0 && pending_note_off_samples < 0 &&
channels == 1 && s_ch == 1 && no_interp && env_state == ENV_SUSTAIN &&
!filter_enabled && !loop_active
```

Tres son muy restrictivas en material real, y explican las tres observaciones
del usuario:

- `no_interp` exige velocidad exactamente 1.0, o sea solo la nota en el root
  key sin pitch bend.
- `!loop_active`: la mayoría de los samples sostenidos de un SF2 tienen loop.
  Esto es el **"depende de la soundfont y de los samples, no de los efectos"**.
- `!filter_enabled`: cualquier CC que active el filtro echa a todas las voces
  del camino de golpe. Esto es el **"algunos CC provocan lag"**.

**Instrumentación agregada:** contador `MISS <razón> <pct>` que atribuye cada
voz que falla a la primera guarda que la bloquea, en el orden en que se
evalúan, así las cuentas particionan en vez de solaparse. Razones: `INTERP`,
`LOOP`, `FILTER`, `OTHER`. Va por los cinco archivos de la ruta de §5 y en el
`Flow`.

**Por qué importa la razón y no solo el número:** cada bloqueo necesita un
kernel distinto. Interpolación pide un resampler vectorizado; el loop pide que
el wrap entre en el bucle vectorial; el filtro pide un biquad vectorizado. La
próxima captura dice cuál escribir primero en vez de adivinar.

**Pendiente nuevo, reportado por el usuario y sin investigar:** el rendimiento
es malo al arrancar **hasta cambiar de soundfont al menos una vez**. Eso
sugiere que la ruta de primera carga deja algo en un estado distinto del que
deja una recarga (layout de samples, preprocesado de regiones, o algún caché).
Es un bug concreto y aparte del tema SIMD. Ver 7.9.

**Revisión de la revisión 21, hecha y aprobada.** Verifiqué manifiesto 41/41,
compilación de C, `node --check`, llaves de QML, y corrí los seis tests de
host: todos pasan. Confirmé además que `tools/voice_rebalance_logic.inc` se
regenera idéntico desde `voice.c`, o sea que el test usa el código real y no
una copia que se pueda desincronizar. El rebalance corre en el tope del ciclo
del worker, donde la pila es owner-only, así que `free_pop()` ahí es seguro.

Riesgo anotado sobre esa revisión: con 0 eventos pendientes `keep = 0` y el
worker devuelve **todas** sus voces libres de a una. `REBAL/s` es el número que
lo delata; si sale alto y `BLOCK` no mejora, conviene acotar el `give` por
ciclo. En la captura actual `REBAL/s` es 0 porque el pool está agotado
(`FREE 79`), así que el dato todavía no se pudo leer.

---

---

## 15. Apéndice C: secciones base de la revisión 37 anterior (sin editar)

Las secciones 1-11 de este documento reescriben con otras palabras las
secciones base que había antes. Este apéndice conserva las originales tal cual,
para garantizar que ningún dato se perdió en la reformulación. **Ante
contradicción, mandan las secciones 1-11.**

### Anterior §1. Qué es el proyecto

WASMIDI es un reproductor de Black MIDI que corre en el navegador, desplegado en
`https://midi.dekx.cc` (GitHub Pages). Dos mitades:

- **Síntesis de audio.** Un port a WebAssembly de **SnappySynthV2**, un motor de
  voces escrito en C para Windows. El original vive en
  `third_party/snappysynthv2/` y se compila con Emscripten. Usa pthreads reales
  (Web Workers) para repartir el render de voces.
- **Visualización.** Un piano roll **horizontal** en OpenGL ES 3 / WebGL2
  (`src/renderer/gl_renderer.cpp`), dentro de una app Qt6 compilada también a
  WASM. El tiempo corre en el eje X y el pitch en el eje Y.

El objetivo declarado: **igualar el rendimiento del SnappySynthV2 nativo lo más
posible, sin alterar su sonido**, y **sin alterar la estética del renderizador
visual**. Esas dos restricciones mandan sobre cualquier optimización. Una mejora
que cambie el sonido o el dibujo no sirve.

Meta concreta pendiente: **8192 voces estables con 8 workers** (1024 por worker).
Hoy se estrangula desde ~4096.

#### Proyectos de referencia

El usuario provee dos árboles de código como referencia. **No son dependencias,
son fuentes de ideas.** Cualquier técnica portable a WASM vale la pena intentarla.

- **SnappySynthV2 original** (nativo, Windows). Referencia de *sonido* y de
  *rendimiento*. Si el port suena distinto al original, el port está mal.
- **BPFA** (Better Piano From Above, fork de PianoFromAbove). Referencia de
  *arquitectura de rendimiento*. Ya usa SnappySynthV2 como motor, así que sus
  decisiones son directamente comparables.

---

### Anterior §2. Restricciones del entorno de trabajo (crítico)

**En el entorno donde se edita este código NO hay Qt6 ni emsdk.** Esto define qué
se puede afirmar y qué no:

- `src/renderer/gl_renderer.cpp`, `src/mainwindow.cpp`, `src/mainwindow.hpp` y
  todo `src/qml/` **no se pueden compilar ni una sola vez**.
- El build de Emscripten completo tampoco.
- Todo cambio en esos archivos llega al usuario **sin una sola verificación**.

Esto ya costó recompilaciones perdidas. La regla que salió de ahí: **cambios de
motor se verifican con tests; cambios de GUI se verifican con una captura del
usuario.** Ir en tandas chicas y pedir captura después de cada una.

#### Lo que sí se puede verificar

```
# C del motor (compila standalone fuera de Emscripten)
cd third_party/snappysynthv2
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 Voice/voice.c
gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 snappy_wasm_core.c

# JavaScript
node --check web/snappysynth-worker.js
node --check web/snappysynth_bridge.js

# Tests de host, sin toolchain
python3 tools/extract_ssw_schedule_logic.py
cc -O1 -Wall -o /tmp/h tools/ssw_schedule_harness.c -lm && /tmp/h
cc -O1 -o /tmp/s tools/worker_freelist_share_check.c && /tmp/s
python3 tools/extract_voice_rebalance_logic.py
cc -O1 -Wall -pthread -o /tmp/b tools/worker_freelist_borrow_check.c && /tmp/b
node tools/pump_slice_sim.mjs
c++ -O2 -std=c++17 -Isrc/renderer -o /tmp/c \
    tools/note_raster_compositor_check.cpp \
    src/renderer/note_raster_compositor.cpp && /tmp/c

# QML: solo balance de llaves. No es compilación.
```

En Windows con MinGW, `snappy_wasm_core.c` necesita
`-include ../../tools/host_posix_env_shim.h` (declara `setenv`/`unsetenv`, que
esa libc no tiene). Emscripten y glibc no lo necesitan. Solo declaraciones.

**Todos estos checks corren en GitHub Actions** (paso "Host tests") antes del
build de Emscripten. Nada se compila localmente: el flujo es editar, subir,
y leer el log del workflow.

`MANIFEST.sha256` cubre 41 archivos y **hay que regenerarlo después de cada
cambio** con `python3 tools/regen_manifest.py` (la lista de archivos vive ahí;
agregar los nuevos). CI lo verifica con `--check` y con `sha256sum -c`, así que
un manifiesto viejo rompe el build en vez de pasar desapercibido.

---

### Anterior §3. Errores ya cometidos. No repetirlos.

Cada uno costó una recompilación. También están en `PORTING_STATUS.md` y
`third_party/snappysynthv2/UPSTREAM_NOTES.md`.

**3.1 — Quitar la frontera de render para bank select / RPN / program change.**
Parece seguro: "la cola de eventos se drena en orden de envío". Es falso entre
clases de evento. `enqueue_event()` enruta notas con `g_note_worker_map[ch][key]`
(hash por tecla) y eventos de canal con `g_channel_worker_map[ch]`. Con más de un
worker están en **colas distintas** consumidas concurrentemente en un mismo ciclo
de render. Sin la frontera, los note-on resuelven contra banco/programa
equivocado, el selector de regiones no encuentra nada, y **la nota se cae sin
sonar y sin error**. Se ve como robo agresivo de voces en material no denso y no
lo es. Rollback: `SSW_FORCE_SELECTOR_BOUNDARIES=1`.

**3.2 — Fragmentar el render por cada evento de estado.** Un bloque de 512 frames
se convertía en ~100 llamadas a `voice_render_float()`. El costo de esa llamada
está dominado por trabajo fijo: dos barreras de futex, rebuild de la render
queue, setup por voz de *todas* las voces activas, y pérdida de los caminos SIMD.

**3.3 — Punto fijo en el mapeo de columnas del culler visual.** Redondea distinto
que el renderizador y descarta notas que sí ocupaban un píxel. Debe usar el mismo
`floor`/`ceil` en `double`, en el mismo orden de operaciones. Comentado en el
código fuente.

**3.4 — La métrica `GAP` invertida.** Medía el tiempo entre invocaciones de
`pump()`. Con poca carga el ring se llena, el pump duerme, y el gap era enorme
*porque todo estaba bien*. Se encendía en rojo justo cuando no pasaba nada.
Corregido a `LATE` = `gap − cobertura del ring`, con piso en cero. **Lección:
validar toda métrica nueva contra un caso donde se conozca la respuesta.**

**3.5 — Agregar elementos a un `RowLayout` lleno.** No hace wrap: al desbordar
empujó fuera de pantalla los controles de SnappySynth. La telemetría vive ahora
en un `Flow`, que reflota.

**3.6 — Comparar dos MIDIs distintos como si fueran el mismo workload** y sacar
conclusiones sobre el conteo de workers. Inválido. Siempre fijar el archivo.

---

### Anterior §4. Estado del diagnóstico de rendimiento

Última medición (navegador, rev. 21): `LOAD 494%`, `BLOCK 56.2ms`, `EVT 0.1ms`,
`STEALS/s 2220`, `FREE 40`, `RING 26%`, `LATE 45ms`, `UNDERRUNS 841`,
16 workers, 8192 voces, bloque 512, bufs 48. Confirma que el rebalance de
freelist no era una mejora de throughput: arregla admisión justa, pero el DSP
sigue tardando casi cinco veces el presupuesto real. `SIMD %` y `BUSY ms`
quedaron fuera de la captura por el ancho del panel; ahora están en la primera
fila siempre visible. Son los próximos números obligatorios antes de optimizar.

#### Descartado con medición. No volver a perseguirlo.

- **No es cadencia del pump.** Resuelto: underruns 4.770 → 82.
- **No es el pool de voces por sí solo.** Con el pool sano igual da 475%.
- **No es el despacho de eventos.** `EVT` es 0,3 ms de 55,6 ms, o sea 0,5%. Fue
  una hipótesis explícita y **resultó falsa**.

#### La cuenta abierta

8.058 voces × 512 frames ÷ 55,3 ms = **74,6 M voice-samples/s** en total. El
nativo sostiene ~361 M/s en 16 hilos, o sea **22,6 M por hilo**. Con 24 workers
se obtiene el equivalente a **poco más de 3 hilos nativos**. O casi nada se
vectoriza, o los workers no se solapan.

#### Los dos números que lo resuelven (ya instrumentados)

Visibles en el panel de SnappySynth:

- **`SIMD %`** — porción de voces que toma el camino vectorizado. **Medido: 0%.**
  Ver §0 para las guardas exactas y qué las bloquea.
- **`MISS <razón> <pct>`** — cuál guarda rechaza a la mayoría. Es el número que
  decide qué kernel escribir.
- **`BUSY ms`** — tiempo ocupado sumado entre workers en el último ciclo. Con 24
  workers y `BLOCK 55ms`, paralelismo real daría del orden de 1.300 ms sumados.
  Si da ~55 ms, están serializados.

| SIMD | BUSY | Conclusión |
|------|------|-----------|
| bajo | alto | DSP en escalar. Relajar condiciones de entrada o agrupar voces compatibles antes de renderizar. |
| alto | bajo | Los workers no se solapan. Mirar la barrera y el reparto de la render queue. |
| bajo | bajo | Ambos. Empezar por SIMD. |
| alto | alto | El DSP es genuinamente así de caro en WASM. Revisar la meta de 8192 voces. |

#### Nueva captura: corrección de la interpretación anterior

Captura recibida: `LOAD 509%`, `BLOCK 58ms`, `EVT 0.1ms`, `SIMD 0%`,
`BUSY 873ms`, 16 workers, 8192 voces activas, 512 frames, 44100 Hz,
`STEALS/s 3120`, `DROPPED 0`, `RING 2%`, `LATE 48ms`, `UNDERRUNS 802`.

**La tabla anterior no basta para elegir una optimización.** La inspección de
`Voice/voice.c` confirma que `g_path_fast_voices` sólo cuenta admisiones al batch
estéreo sustain; `g_path_scalar_voices` se incrementa antes de los otros caminos
SIMD por voz (incluidos mono → estéreo). Por eso 0% NO prueba DSP completamente
escalar. El panel ahora dice `BATCH` para describir el contador real, conservando
los nombres internos/API por compatibilidad.

El batch WASM exige sustain, muestra estéreo, pitch casi unitario, sin loops,
sin filtros ni transiciones pendientes. No se deben quitar esas condiciones sin
implementar sus semánticas. No sabemos cuáles dominan en este SoundFont/captura.

`873 / 58 ≈ 15` es consistente con workers solapados, no con serialización total.
Son tiempos de pared: no prueban utilización CPU efectiva ni scaling ideal.
Siguiente paso: perfilar los caminos fuera del batch (envolvente, interpolación,
loops y filtros), o instrumentar sus causas de rechazo y cobertura SIMD real.
No hay todavía una mejora de throughput validada en navegador.

---

### Anterior §5. Arquitectura, por si hay que tocarla

#### Cadena de audio

```
midi-parser-worker.js  (parsea el MIDI, agenda eventos con lookahead)
        | scheduleBatch
snappysynth-worker.js  (Worker; hospeda el modulo WASM del synth)
        |  pump() -> ssw_render_queued_into() -> voice_render_float()
        |                                        \-> N worker pthreads
        | ring SharedArrayBuffer
snappysynth-audio-worklet.js  (AudioWorklet, consume en quantums de 128)
        |
snappysynth_bridge.js  (estado compartido, leido por Qt via EM_JS)
```

**Detalle que importa:** `pump()` corre en **el mismo hilo** que recibe
`scheduleBatch`. Un render largo y sincrónico deja sin atender la entrega de
eventos. BPFA no tiene este problema porque renderiza en un hilo dedicado con un
ring en medio. Esto bloquea portar el bloque de mezcla grande de BPFA (ver 7.4).

#### Telemetría

Los números viajan en el payload de stats, **no por consola** — imprimir desde un
worker a esa frecuencia le cuesta tiempo real al hilo principal, y fue un
problema reportado por el usuario.

Ruta: `snappy_wasm_core.c` (exports `ssw_*`) → `snappysynth-worker.js` →
`snappysynth_bridge.js` (`state.*`) → `mainwindow.cpp` (`EM_JS`) →
`mainwindow.hpp` (`Q_PROPERTY`) → `Controls.qml`.

Para agregar un contador hay que tocar los cinco. El patrón a copiar es
`synthSteals`.

Campos actuales: `LOAD %`, `BLOCK ms`, `EVT ms`, `SIMD %`, `BUSY ms`, `RING %`,
`LATE ms`, `REBAL/s`, `DROPPED`, más `WORKERS`, `VOICES/WKR`, `STEALS/s`,
`UNDERRUNS`.

### Anterior §8. Cómo trabajar con el usuario

- Prueba en **Brave, en Windows, en un Ryzen 9 5900X (24 hilos)**. Brave
  sub-reporta `hardwareConcurrency` como 7.
- Manda capturas del panel y de la consola. Esa es la única fuente de verdad
  sobre el navegador.
- El flujo que funciona: **instrumentar antes que arreglar.** Cuando se adivinó,
  se falló (3.1, 3.4, 3.6, y la hipótesis del despacho de eventos). Cuando se
  midió, se resolvió en una iteración.
- Los archivos se editan en el árbol del usuario y se suben a GitHub tal cual;
  el build corre en Actions. Hacer los cambios completos directamente en los
  archivos, no entregar diffs ni parches para aplicar a mano.
- Con cada update, actualizar este documento: qué se hizo, qué sigue, qué falta a
  largo plazo, y qué necesita saber quien continúe.

---

## 15. Revisión 38 — materialización de SF y costo agregado por el port

Esta revisión es el primer pase de arreglo sobre el estado descrito en §5. No se
reclama mejora de navegador hasta recibir una A/B del usuario; Qt6 y emsdk no
están disponibles en el entorno de edición.

### 15.1 Soundfont: no entrar a realtime con trabajo pendiente

Se corrigió la interpretación de `PRESMP`: `resampled == 0` no implica que no se
hayan recorrido regiones. La UI ahora expone `seen`. Además se corrigió el caso
en que un fallo de reserva durante `resample_wav_data()` quedaba cacheado como si
hubiera producido datos a la tasa destino.

`ssw_load_sf2()` presamplea y valida la capa entrante antes de mezclarla;
`ssw_init_ex()` valida también el instrumento existente al reinicializar el audio.
Una región sólo pasa como materializada si tiene PCM/cache válidos y, cuando la
tasa difiere, un buffer resampleado real. Fallar esa comprobación devuelve error
al worker en lugar de publicar `SF2 ready`.

### 15.2 Frontera de eventos

`ssw_queue_events()` reutiliza slabs en un pool acotado en lugar de reservar y
liberar un bloque por mensaje. Para timestamps no negativos, la conversión a frame
usa `x + 0.5` + conversión entera, la misma regla de `llround()` en ese dominio.
Ambas optimizaciones se revierten juntas con:

```text
-DSSW_REV38_EVENT_HOTPATH=0
```

No cambian el orden de eventos, la frontera de selector ni el timestamp resultante.

### 15.3 Telemetría que estaba dentro del hot path

La instrumentación había reintroducido trabajo global por voz incluso con Debug
apagado. `g_notes_started_total` se incrementaba por cada voz admitida y el
reciclaje se contaba en `free_push()`, donde además se mezclaban refill/rebalance
con liberaciones reales. Los contadores de cobertura SIMD tenían el mismo patrón.

Ahora `worker_thread` toma `g_debug_metrics` una sola vez por ciclo. Los atómicos
de diagnóstico sólo se ejecutan cuando Debug está activo, y `RECYC` cuenta la
retirada real de `ENV_OFF` en el barrido post-render. Al prender Debug se reinicia
el baseline de lifecycle para que el porcentaje siga siendo interpretable.

### 15.4 Archivos tocados

- `third_party/snappysynthv2/snappy_wasm_core.c`
- `third_party/snappysynthv2/Parser/sfz_parser.c`
- `third_party/snappysynthv2/Voice/voice.c`
- `web/snappysynth-worker.js`
- `src/mainwindow.cpp`
- `src/mainwindow.hpp`
- `src/qml/Controls.qml`
- `tools/ssw_schedule_harness.c`
- `HANDOFF.md`
- `MANIFEST.sha256` (regenerar al cerrar la revisión)

### 15.5 Validación local de rev. 38

Disponible y ejecutada: sintaxis C de `voice.c`, `snappy_wasm_core.c` y
`sfz_parser.c`; sintaxis Node de worker/bridge; harness de scheduler; harness de
rebalance; harness de redundancia VOR; freelist share; simulación del pump.

No disponible aquí: build Qt6, build Emscripten completo, ejecución en Brave ni
prueba audible. La prueba del usuario debe comprobar primero `PRESMP seen ==
REGIONS`, `skip == 0` y después comparar primera vs segunda reproducción del mismo
MIDI/SF con Debug apagado. Para el caso CC, comparar `ALLOC`, `BLOCK`, underruns y
respuesta audible con exactamente el mismo MIDI/configuración.



---

## 16. Revisión 38.1 — diagnóstico reproducible del manifest en CI

GitHub Actions reportó `MANIFEST.sha256 is stale` después de que todos los tests
de host pasaran. El ZIP de rev. 38, ejecutando localmente el bloque exacto
`Host tests (engine logic, no Emscripten)` del workflow, sí pasa
`regen_manifest.py --check` y `sha256sum -c`. Eso demuestra que el fallo aparece
cuando el árbol que llega al commit no coincide byte por byte con el drop
empaquetado (por ejemplo, un archivo subido selectivamente o una versión previa
del workflow).

`tools/regen_manifest.py --check` ahora imprime cada archivo cuyo SHA difiere,
mostrando hash registrado y hash actual, además de entradas faltantes, extra o
malformadas. No se cambió código de runtime, audio, MIDI, renderer, VOR ni
stealing en esta revisión. `MANIFEST.sha256` se regeneró después de este cambio.


---

## 17. Revisión 39 — hot path del stealer bajo saturación CC

La prueba real de rev. 38 confirmó dos cosas distintas. Primero, la preparación de
la SF2 ya recorre las 11 regiones (`PRESMP seen 11`) sin skips en idle. Segundo,
el caso CC sigue siendo el cuello principal: con 24 workers y 8192 voces la
captura mostró `ACTIVE 8112`, `FREE 80`, `ALLOC 1974 ms`, `BUSY 59 ms`,
`STEALS/s 18285`, `DROPPED 310290`, `BLOCK 89.3 ms`, `LOAD 751%`, `RING 6%` y
`LATE 77 ms`. Es decir, el tiempo sigue concentrado en la fase de admisión/robo,
no en el DSP.

### 17.1 Workers=0 en navegador

Se encontró un bug independiente: en algunas configuraciones Chromium/Brave el
`GetSystemInfo()` emulado dentro del módulo devolvía un procesador, por lo que
`Workers=0` terminaba creando un único worker (`WORKERS 1`, `VOICES/WKR 8192`).
El worker JS ahora resuelve Auto en el límite del navegador usando
`navigator.hardwareConcurrency` y entrega ese valor explícitamente a
`ssw_init_ex()`. Un valor manual continúa pasando sin recortarse, porque Brave
puede farblear el conteo reportado.

### 17.2 Adyacencia de CC en O(n) por lote

La reproducción browser-side del colapso nativo de CC por adyacencia buscaba el
siguiente evento de canal haciendo un scan hacia delante por cada controlador.
Con corridas largas de notas entre CC eso revisita repetidamente los mismos
eventos. Ahora, al admitir el slab, un pase inverso construye el índice del
siguiente evento no-nota de cada canal. La consulta durante render queda O(1).

La relación es idéntica a la anterior: ignora note-on/off (van a otra cola), no
cruza límites de slab, no cruza el final del bloque y sólo absorbe cuando el
siguiente evento de ese canal es el mismo CC. Se verificó además por comparación
aleatoria contra el algoritmo anterior; no aparecieron diferencias de decisión.

### 17.3 Espejo compacto de campos estáticos del stealer

`steal_voice_fast()` es byte por byte equivalente al SSv2 nativo en rev. 38, pero
cada intento fallido puede sondear 128--512 voces. En Shared-WASM, leer para cada
probe `key`, `voice_age` y el volumen de prioridad desde un `voice` grande obliga
a tocar líneas de memoria dispersas aunque muchas candidatas se descarten antes
de necesitar el resto del estado.

Rev. 39 añade un arreglo compacto por voz con exactamente esos campos:
`priority_volume`, `voice_age`, `key` y la clase de prioridad derivada. Se llena al
admitir una voz y se actualiza en todas las rutas VOR que modifican
`original_volume`. El loop de probes usa ese espejo para los guards tempranos y
sigue leyendo del `voice` para estado dinámico, loudness, envelope y la decisión
final.

No cambia el orden del scan, `probe_cap`, cursor, scores, protección de bass,
prioridades, guards, víctima elegida ni qué note-on se descarta. Si el arreglo no
se puede reservar, el accessor cae automáticamente a los campos originales. Se
puede compilar sin el camino nuevo con `-DSSW_STEAL_STATIC_CACHE=0`.

### 17.4 Validación disponible

Pasan sintaxis C de `voice.c`, `snappy_wasm_core.c` y `sfz_parser.c`; sintaxis
Node de worker/bridge; scheduler 14/14; freelist rebalance; VOR redundancy;
freelist share y simulación del pump. Falta, como siempre en este entorno, el build
Qt/Emscripten completo y la prueba real en Brave.

La próxima comparación debe usar 24 workers explícitos primero para aislar el
cambio del stealer. En el mismo punto CC de la captura anterior comparar
`ALLOC`, `BLOCK`, `STEALS/s`, `DROPPED`, `RING`, `LATE` y respuesta audible. La
expectativa de esta revisión no es cambiar `STEALS/s`/`DROPPED` por política, sino
bajar el costo temporal de llegar a exactamente las mismas decisiones.
