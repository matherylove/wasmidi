# WASMIDI — Handoff

**Revisión 21.** Escrito para alguien que llega sin contexto previo. Si vas a
continuar este trabajo, leé las secciones 1 a 4 completas antes de tocar código.

## 0. Qué se hizo en la revisión 21 (leer primero)

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

## 1. Qué es el proyecto

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

### Proyectos de referencia

El usuario provee dos árboles de código como referencia. **No son dependencias,
son fuentes de ideas.** Cualquier técnica portable a WASM vale la pena intentarla.

- **SnappySynthV2 original** (nativo, Windows). Referencia de *sonido* y de
  *rendimiento*. Si el port suena distinto al original, el port está mal.
- **BPFA** (Better Piano From Above, fork de PianoFromAbove). Referencia de
  *arquitectura de rendimiento*. Ya usa SnappySynthV2 como motor, así que sus
  decisiones son directamente comparables.

---

## 2. Restricciones del entorno de trabajo (crítico)

**En el entorno donde se edita este código NO hay Qt6 ni emsdk.** Esto define qué
se puede afirmar y qué no:

- `src/renderer/gl_renderer.cpp`, `src/mainwindow.cpp`, `src/mainwindow.hpp` y
  todo `src/qml/` **no se pueden compilar ni una sola vez**.
- El build de Emscripten completo tampoco.
- Todo cambio en esos archivos llega al usuario **sin una sola verificación**.

Esto ya costó recompilaciones perdidas. La regla que salió de ahí: **cambios de
motor se verifican con tests; cambios de GUI se verifican con una captura del
usuario.** Ir en tandas chicas y pedir captura después de cada una.

### Lo que sí se puede verificar

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

## 3. Errores ya cometidos. No repetirlos.

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

## 4. Estado del diagnóstico de rendimiento

Última medición (navegador, rev. 21): `LOAD 494%`, `BLOCK 56.2ms`, `EVT 0.1ms`,
`STEALS/s 2220`, `FREE 40`, `RING 26%`, `LATE 45ms`, `UNDERRUNS 841`,
16 workers, 8192 voces, bloque 512, bufs 48. Confirma que el rebalance de
freelist no era una mejora de throughput: arregla admisión justa, pero el DSP
sigue tardando casi cinco veces el presupuesto real. `SIMD %` y `BUSY ms`
quedaron fuera de la captura por el ancho del panel; ahora están en la primera
fila siempre visible. Son los próximos números obligatorios antes de optimizar.

### Descartado con medición. No volver a perseguirlo.

- **No es cadencia del pump.** Resuelto: underruns 4.770 → 82.
- **No es el pool de voces por sí solo.** Con el pool sano igual da 475%.
- **No es el despacho de eventos.** `EVT` es 0,3 ms de 55,6 ms, o sea 0,5%. Fue
  una hipótesis explícita y **resultó falsa**.

### La cuenta abierta

8.058 voces × 512 frames ÷ 55,3 ms = **74,6 M voice-samples/s** en total. El
nativo sostiene ~361 M/s en 16 hilos, o sea **22,6 M por hilo**. Con 24 workers
se obtiene el equivalente a **poco más de 3 hilos nativos**. O casi nada se
vectoriza, o los workers no se solapan.

### Los dos números que lo resuelven (ya instrumentados)

Visibles en el panel de SnappySynth:

- **`SIMD %`** — porción de voces que toma el batch SIMD. Bajo significa que las
  condiciones de entrada (estéreo, sustain, sin interpolación, frames aptos) las
  rechazan al bucle escalar.
- **`BUSY ms`** — tiempo ocupado sumado entre workers en el último ciclo. Con 24
  workers y `BLOCK 55ms`, paralelismo real daría del orden de 1.300 ms sumados.
  Si da ~55 ms, están serializados.

| SIMD | BUSY | Conclusión |
|------|------|-----------|
| bajo | alto | DSP en escalar. Relajar condiciones de entrada o agrupar voces compatibles antes de renderizar. |
| alto | bajo | Los workers no se solapan. Mirar la barrera y el reparto de la render queue. |
| bajo | bajo | Ambos. Empezar por SIMD. |
| alto | alto | El DSP es genuinamente así de caro en WASM. Revisar la meta de 8192 voces. |

### Nueva captura: corrección de la interpretación anterior

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

## 5. Arquitectura, por si hay que tocarla

### Cadena de audio

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

### Telemetría

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

### Memoria

El módulo del synth usa pthreads, así que su heap es un `SharedArrayBuffer` y
`memory.grow` **sincroniza a todos los workers**: con 16–24 los congela a todos a
la vez. Se ve como un stall sin carga.

Lo que lo dispara escala con el conteo de workers: `CH_EVENT_QUEUE_SIZE` son
16384 eventos por *(worker, canal)*, o sea >150 MB de colas con 24 workers contra
~44 MB con 7. Por eso `INITIAL_MEMORY` está en 1 GB con paso de crecimiento
lineal de 256 MB, y `DEFAULT_PTHREAD_STACK_SIZE` en 256 KB.

### Cross-origin isolation

Los pthreads necesitan `crossOriginIsolated=true`, que en GitHub Pages se logra
con `web/coi-serviceworker.js`. Sin HTTPS no hay Service Worker, sin Service
Worker no hay COI, sin COI no hay `SharedArrayBuffer`, y el SF2 no carga. Si el
usuario reporta "no cargan las soundfonts", verificar HTTPS, ventana no privada y
Shields de Brave **antes** de mirar código.

---

## 6. Trabajo hecho y verificado (no tocar sin motivo)

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

## 7. Pendiente a largo plazo

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

**7.8 — Frames del renderizador se detienen** en pasajes de 3M+ notas por
segundo. El ring visual arranca en `1 << 23` notas y **duplica**: a 12 bytes por
`VisualNote` eso es un VBO de 100 MB y el siguiente escalón 201 MB. Hipótesis sin
confirmar: una de esas duplicaciones falla en la GPU y el ring queda inservible.
Para confirmarlo hay que loguear en `ensureRingCapacity()` y `allocateRing()` el
tamaño pedido, el resultado de `glBufferData` y el error de GL.

---

## 8. Cómo trabajar con el usuario

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
