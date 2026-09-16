# Handoff — Update 0 Revision 20

## Estado del diagnóstico de rendimiento (leer primero)

`LOAD 475%`, `BLOCK 55.6ms`, `EVT 0.3ms`, `STEALS/s 3156`, `FREE 134`,
24 workers, 8192 voces.

**Descartado con medición, no repetir:**

- *No* es cadencia del pump. Eso se arregló: underruns 4.770 → 82.
- *No* es el pool de voces por sí solo. Con el pool sano igual da 475%.
- *No* es el despacho de eventos. `EVT` es 0,3 ms de 55,6 ms, o sea 0,5%.
  Esta era mi hipótesis y **era falsa**. No la persigan de nuevo.

**La cuenta que queda abierta:** 8.058 voces × 512 frames ÷ 55,3 ms = 74,6 M
voice-samples/s en total. El nativo sostiene ~361 M/s en 16 hilos, o sea 22,6 M
por hilo. Con 24 workers se está obteniendo el equivalente a poco más de 3
hilos nativos. O casi nada se vectoriza, o los workers no se solapan.

**Los dos números que lo resuelven, ya instrumentados en esta revisión:**

- `SIMD %` — porción de voces que toma el batch SIMD. Bajo significa que las
  condiciones de entrada (estéreo, sustain, sin interpolación, frames aptos)
  las están rechazando al camino escalar.
- `BUSY ms` — tiempo ocupado sumado entre workers en el último ciclo. Cerca de
  `workers × BLOCK` significa paralelismo real; cerca de `BLOCK` a secas
  significa que están serializados.

Cómo interpretar la próxima captura:

| SIMD | BUSY | Conclusión |
|------|------|-----------|
| bajo | alto | El DSP corre en escalar. Relajar condiciones de entrada o agrupar voces compatibles antes de renderizar. |
| alto | bajo | Los workers no se solapan. Mirar la barrera y el reparto de la render queue. |
| bajo | bajo | Ambos. Empezar por SIMD. |
| alto | alto | El DSP es genuinamente así de caro en WASM y hay que revisar la meta de 8192 voces. |

## Qué hice en este update

**1. Los stalls invisibles: crecimiento de memoria compartida.**

El dato decisivo fue tuyo: "los stalls no se ven en los stats, se ve como si no
hubiese nada de carga". La telemetría solo corría dentro de `pump()`, así que un
stall no aparecía como carga baja sino como *ausencia de muestra*. Eso descarta
falta de CPU y apunta a que el hilo no se ejecuta.

Causa: el módulo del synth usa pthreads, así que su heap es un
`SharedArrayBuffer` y `memory.grow` tiene que sincronizar a todos los workers.
Con 16–24 workers cada crecimiento los congela a todos a la vez.

Y la asignación que lo provoca escala con el número de workers:
`CH_EVENT_QUEUE_SIZE` son 16384 eventos por *(worker, canal)*, o sea >150 MB de
colas con 24 workers contra ~44 MB con 7. Con `INITIAL_MEMORY` en 256 MB se
cruzaba el límite en el arranque y otra vez durante la reproducción.

Cambios en `CMakeLists.txt` (target `snappysynth_core`):
- `INITIAL_MEMORY` 256 MB → 1 GB, para que la reproducción nunca crezca.
- `MEMORY_GROWTH_GEOMETRIC_STEP=0` + `MEMORY_GROWTH_LINEAR_STEP=256MB`, para que
  un crecimiento sea un salto grande y no muchos chicos.
- `MAXIMUM_MEMORY` 2 GB → 3 GB.
- `DEFAULT_PTHREAD_STACK_SIZE=262144`: los workers de render usan stacks
  superficiales y el default se multiplicaba por todo el pool sin beneficio.

**No verificado en navegador.** Es una hipótesis con evidencia fuerte, no una
medición. La telemetría de abajo es lo que la confirma o la descarta.

**2. Saqué la telemetría de la consola.**

Tenés razón en que los mensajes frenaban el navegador, y era culpa mía. Los
números ahora viajan en el payload de stats, no se imprimen. Campos nuevos en el
estado del bridge, listos para graficar:

- `renderLoadPercent` — carga como % del presupuesto de tiempo real del bloque
- `lastRenderMs` — costo del último bloque
- `ringFillPercent` — ocupación del ring de audio
- `pumpGapMs` — **el importante**: ms desde el pump anterior. Un gap grande con
  carga baja significa hilo detenido, no ocupado. Decae en vez de borrarse, así
  un stall queda visible un momento.
- `blocksPerPump`, `renderBudget`

Queda pendiente engancharlos a los gráficos de la barra lateral (ver abajo).

## Qué planeo hacer en el próximo update

En este orden:

1. **Notas que no suenan con múltiples workers.** Es tu punto #1 y el más grave.
   Sospecha principal, sin confirmar: `g_note_worker_map` enruta por hash de
   tecla y un worker sin voces libres solo roba las propias, así que puede
   descartar una nota teniendo voces libres en otro worker. Plan: robo entre
   workers — tomar de la pila *libre* de otro antes de robar una voz sonando.
   Test de host que verifique que ninguna voz activa se roba mientras exista una
   libre en cualquier worker.
2. **Gráficos de la barra lateral** con los campos ya expuestos: carga,
   latencia, voces, steals/s. El resto como contadores en el menú de SnappySynth.
3. **Prerender de frames visuales** estilo BPFA: el caché de tiles con hilos de
   fondo. El culler ya está hecho y verificado (ver abajo), le falta esa mitad.

## Qué falta a largo plazo

- **Soundfonts GM completos suenan mal / samples incorrectos, y los drums no
  siguen el estándar.** Viene del SnappySynthV2 original. No lo miré todavía.
  Es selección de preset/banco/región, no rendimiento.
- **Carga de MIDI lenta comparada a BPFA.** `StreamingMidiParser` y
  `PreprocessedMidi` de BPFA sin revisar.
- **Bloque de mezcla desacoplado** (BPFA usa 8192 para mezcla y 1024 para
  dispositivo). Requiere sacar el render del hilo de mensajes primero.
- **8192 voces en 8 workers estables.** Hoy se estrangula desde 4096. Depende de
  los puntos 1 y del bloque de mezcla.
- Todo el trabajo de GUI: selector de colores tipo BPFA con popup RGB/hex y
  paleta en cookies, quitar Post-buffer, renombrar el letrero, note speed por
  ticks, unificar NPS, quitar Skipped vel, quitar la barra superior, fondo
  neural, limpieza de referencias a terceros.

## Qué necesita saber GPT para continuar

**Entorno.** Acá no hay Qt6 ni emsdk. `gl_renderer.cpp` y el build de Emscripten
**no se pueden compilar ni una vez**. Todo lo que toca esos caminos va sin
verificar. Lo que sí compila y corre:

    gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 Voice/voice.c
    gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 snappy_wasm_core.c
    node --check web/snappysynth-worker.js
    node tools/pump_slice_sim.mjs
    python3 tools/extract_ssw_schedule_logic.py && cc -O1 -o /tmp/h tools/ssw_schedule_harness.c -lm && /tmp/h
    cc -O1 -o /tmp/s tools/worker_freelist_share_check.c && /tmp/s
    c++ -O2 -std=c++17 -Isrc/renderer -o /tmp/c tools/note_raster_compositor_check.cpp src/renderer/note_raster_compositor.cpp && /tmp/c

`MANIFEST.sha256` cubre 33 archivos y debe regenerarse tras cada cambio.

**Errores que ya cometí. No repetirlos.** Están documentados en
`PORTING_STATUS.md` y `third_party/snappysynthv2/UPSTREAM_NOTES.md`:

- Quitar la frontera de render para bank select / RPN / program change. Parece
  seguro porque la cola se drena en orden, pero `enqueue_event()` enruta notas
  por hash de tecla y eventos de canal por canal, así que con más de un worker
  están en colas distintas y se consumen concurrentemente. Sin la frontera los
  note-on resuelven contra banco/programa equivocado y **la nota se cae sin
  sonar**. Parece bug del stealer y no lo es.
- Fragmentar el render por cada evento de estado. Un bloque de 512 se volvía
  ~100 llamadas a `voice_render_float()`.
- Punto fijo en el mapeo de columnas del culler: redondea distinto que el
  renderizador y descarta notas visibles. Tiene que usar el mismo `floor`/`ceil`
  en double, en el mismo orden.

**Estado del culler visual.** `src/renderer/note_raster_compositor.{hpp,cpp}`
está terminado y verificado: usa el orden de BPFA (gana la que empieza después),
devuelve notas y no geometría para que el shader original quede intacto, y el
test compara celda por celda contra un modelo del propio depth rule — cero
diferencias en 9 casos. **No está conectado a `gl_renderer.cpp`.** 21,8 ms para
un viewport de 1M notas, así que necesita el caché de tiles en hilos de fondo
para pagarse. Al conectarlo: el pase de notas necesita depth test apagado o z
constante, porque el orden lo impone el culler.

**Lo que ya está medido y funcionando.** No tocar sin motivo:
- Cadencia del pump: `UNDERRUNS` bajó de 4.770 a 82 al llenar hacia un objetivo
  de ocupación en tajadas de 12 ms en vez de ráfagas de 16 bloques.
- Gobernador de segmentación en `ssw_render_queued_into()`.
- Lote de freelist proporcional (`update_worker_freelist_refill`).
- Un pedido explícito de workers no se recorta contra `cores` (Brave
  sub-reporta `hardwareConcurrency`).
