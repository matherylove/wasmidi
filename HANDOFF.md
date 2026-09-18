# WASMIDI — Handoff

**Revisión 26.** Escrito para alguien que llega sin contexto previo. Si vas a
continuar este trabajo, leé las secciones 1 a 4 completas antes de tocar código.

## 0. Estado en la revisión 26 (leer primero)

### El costo NO está en el DSP. Está en asignar voces.

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

### El mecanismo del acantilado

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

### Lo que falta explicar, y el instrumento de esta revisión

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

## 0. Estado en la revisión 25 (leer primero)

### El reparto de la cola NO es el problema

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

### Dónde están esos 82 ms

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

### Sigue abierto

**El bug de primera carga NO está resuelto.** El arreglo de la rev. 23
(`voice_refresh_all_region_caches`) es correcto en sí mismo pero no era la
causa. Reportado por el usuario tras probar la rev. 24. Sospechoso vigente:
`sfz_apply_presampling`. Verificación pendiente: contador de regiones con
`is_resampled` verdadero tras cada carga; si la primera da 0 y la segunda da
todas, confirmado.

---

## 0. Estado en la revisión 24 (leer primero)

### Corrección de un error de interpretación mío

`MISS INTERP 100%` **no es un bug, es el caso esperado.** `no_interp` exige una
relación de velocidad exactamente 1.0, o sea solo la nota en el root key del
sample sin pitch bend. En música real casi ninguna nota cumple eso, así que el
100% es normal. Lo interpreté como patología durante dos revisiones. El arreglo
de la rev. 23 (`voice_refresh_all_region_caches`) es correcto en sí mismo — el
multiplicador de pitch se aplicaba tarde y eso afectaba el sonido — pero **no
era la causa del bug de primera carga**, que sigue abierto.

### El bug de primera carga, medido

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

### El paralelismo es malo siempre; el bug de primera carga lo camuflaba

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

## 0. Qué se hizo en la revisión 23 (leer primero)

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

## 0. Qué se hizo en la revisión 22 (leer primero)

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
