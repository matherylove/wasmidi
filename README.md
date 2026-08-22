# WASMIDI Player

Reproductor MIDI de alta densidad construido con **Qt para WebAssembly** y **WebGL2**.

## 🎹 Caractersticas

- **Parser MIDI nativo en C++** - Soporte completo para Format 0 y 1
- **Renderizado WebGL2** - Piano roll de alto rendimiento
- **Qt/QML** - Interfaz moderna y responsiva
- **Black MIDI ready** - Miles de notas simultá¬°neas
- **Salida MIDI nativa** - Web MIDI API
- **Sintetizador embebido** - SnappySynth (SoundFont 2)

## 📦 Estructura del proyecto

```
wasmidi/
├── src/
│   ├── main.cpp                    # Punto de entrada
│   ├── mainwindow.cpp/hpp          # Lgica principal
│   ├── pianoroll.cpp/hpp           # Renderer WebGL2
│   ├── keyboard.cpp/hpp            # Teclado piano
│   ├── midi/
│   │   ├── midi_parser.cpp/hpp     # Parser MIDI C++
│   │   └── scheduler.cpp/hpp       # Scheduler temporal
│   └── qml/
│       ├── MainWindow.qml          # Ventana principal
│       ├── PianoRoll.qml           # Piano roll
│       ├── Keyboard.qml            # Teclado
│       └── Controls.qml            # Controles UI
├── CMakeLists.txt                  # Configuracin Qt/WASM
├── .github/workflows/
│   └── build-qt-wasm.yml           # CI/CD para Pages
└── old/                            # Archivos JS originales (referencia)
```

## 🚀 GitHub Pages

El sitio se despliega automá¬°ticamente en:
```
https://matherylove.github.io/wasmidi/
```

Cada push a `main` dispara:
1. Build de Qt/WASM en contenedor Docker
2. Compilacin con Emscripten
3. Deploy automá¬°tico a GitHub Pages

## 🛠 Build local

### Requisitos

- **Qt 6.5+** con soporte WASM
- **Emscripten 3.1.74+**
- **CMake 3.14+**
- **Docker** (opcional, para build reproducible)

### Opcin 1: Build con Docker (recomendado)

```bash
# Usar el mismo contenedor que GitHub Actions
docker run --rm -v $(pwd):/work -w /work \
  ghcr.io/nicolasfara/qtwebassemblydockerimage:6.5.0 \
  bash -c "
    mkdir -p build && cd build && \
    emcmake cmake .. -DCMAKE_BUILD_TYPE=Release && \
    emmake make -j$(nproc)
  "
```

### Opcin 2: Build local con Qt WASM

```bash
# Instalar Qt WASM
aqt install linux desktop 6.5.0 wasm_multithread

# Configurar
export QT_ROOT_DIR=/path/to/6.5.0/wasm_multithread
mkdir build && cd build

# Configurar con Emscripten
emcmake cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$QT_ROOT_DIR \
  -DCMAKE_TOOLCHAIN_FILE=$QT_ROOT_DIR/lib/cmake/Qt6/qt.toolchain.cmake

# Compilar
emmake make -j$(nproc)
```

### Output

Despus del build tendrs:
```
build/
├── wasmidi.html      # Punto de entrada
├── wasmidi.js        # Loader WASM
├── wasmidi.wasm      # Binario WebAssembly (~25-30 MB)
└── wasmidi.data      # Datos de Qt
```

### Servir localmente

```bash
# Python
python -m http.server 8000 --directory build

# O con Emscripten
emrun --no_browser --port 8000 build/

# Abrir en navegador
http://localhost:8000/wasmidi.html
```

## ⚙️ GitHub Actions

El workflow `.github/workflows/build-qt-wasm.yml`:

1. **Build job**:
   - Usa contenedor Docker con Qt 6.5 WASM
   - Instala Emscripten 3.1.74
   - Configura CMake con toolchain de Qt
   - Compila con `emmake make`
   - Prepara artefactos para Pages

2. **Deploy job**:
   - Sube artefactos a GitHub Pages
   - Despliega automá¬°ticamente
   - Notifica URL de deploy

### Trigger manual

Puedes disparar el build manualmente desde:
```
https://github.com/matherylove/wasmidi/actions/workflows/build-qt-wasm.yml
```

## 🎯 Uso

### Cargar MIDI

1. Click en "📁 Load MIDI"
2. Selecciona archivo `.mid` o `.midi`
3. El parser cargar todas las notas

### Controles

- **▶ Play** - Iniciar reproduccin
- **⏸ Pause** - Pausar
- **⏹ Stop** - Detener y resetear
- **Slider de tiempo** - Seek en la cancin
- **Note Speed** - Velocidad de scroll (0.1s - 60s)
- **Post-buffer** - Buffer de notas salientes
- **Volume** - Control de volumen
- **Colors** - Modo Per Channel / Per Track

### Teclado

- Muestra las 128 teclas (10 octavas)
- Se ilumina con las notas activas
- Color basado en el canal o track

## 🔧 Configuracin

### Canales de color

Los 16 canales MIDI tienen colores asignados:
- Por defecto: `#818cf8` (í¬°ndigo)
- Personalizable desde la UI
- Modo "Per Track": color por orden de aparicin

### Rendimiento

- **NPS (Notes Per Second)**: Notas que inician por segundo
- **Polyphony**: Notas simultá¬°neas activas
- **FPS**: Frames por segundo del renderer

### Atajos de teclado

- `Espacio`: Play/Pause
- `R`: Stop
- `↑`: Subir volumen
- `↓`: Bajar volumen

## 📊 Estadsticas

Despus de cargar un MIDI:
- **Notes**: Total de notas
- **Tracks**: Número de tracks
- **Dur**: Duracin en segundos
- **BPM**: Tempo base
- **Chs**: Canales activos
- **Fmt**: Formato MIDI (0 o 1)
- **PPQ**: Ticks por beat
- **Tmp Chg**: Cambios de tempo
- **Pk NPS**: Máximo NPS alcanzado
- **Pk Poly**: Máxima polifoná¬°a
- **CC Evts**: Eventos de control
- **Pitch**: Rango de pitch bend

## 🧪 Testing

### Black MIDI

El parser soporta archivos con:
- >100,000 notas
- >16 tracks
- Mltiples cambios de tempo
- Eventos CC densos

### Formatos soportados

- ✅ MIDI Format 0
- ✅ MIDI Format 1
- ❌ MIDI Format 2 (no estandar)
- ❌ SMPTE timing

## 📝 Licencia

Proyecto personal de Dekxtopia. Todos los derechos reservados.

## 🙏 Créditos

- Parser MIDI basado en `midi_parser.c` original
- UI inspirada en `MPWGL2.html`
- Qt para WebAssembly - The Qt Company
- Emscripten - Emscripten contributors

### Large-MIDI memory model (Pass 12.5)

On modern Chromium/Firefox the background parser uses WebAssembly Memory64 and
grows on demand instead of stopping at the old wasm32 2/4 GiB parser ceiling.
WASMIDI does not attempt to guess "free RAM" (the browser does not expose an
exact value); allocation continues until the browser/OS refuses additional
pages, up to the current 16 GiB Memory64 per-memory engine limit. File input and
parser-result transport are streamed/chunked to avoid duplicate file-sized JS
buffers. The Qt UI module is still wasm32 and can use up to its 4 GiB address
space; documents larger than that require the future segmented-residency path.

### Memory64 JavaScript ABI stability (Pass 12.6)

The dedicated parser no longer exposes raw Memory64 pointers or `size_t` values
to JavaScript. Emscripten/wasm64 can represent some raw exported i64 pointer
parameters as `BigInt` while helper-returned addresses may be ordinary Numbers;
mixing those representations caused CI/browser failures such as `Cannot convert
4218976 to a BigInt`.

Pass 12.6 exposes a small Number-only facade (`wmp_alloc_js`, `wmp_free_js`,
`wmp_parse_js`, and Number-returning result/error accessors). C++ converts those
exact integer addresses internally to `uintptr_t`/`size_t`. The current 16 GiB
Memory64 ceiling is far below JavaScript's exact-integer limit (2^53), so no
address precision is lost. The raw MIDI data and parser output remain in
Memory64; this change only stabilizes the JS/WASM call ABI.

## Pass 12.7 — Memory64 progress callback ABI hotfix

The Memory64 parser's public API was already Number-only, but parser progress
callbacks still passed a raw `const char*` through `EM_JS`. Emscripten 3.1.56
represents that wasm64 pointer as JavaScript `BigInt`, while `UTF8ToString`
requires a `Number`. Pass 12.7 legalizes progress-stage pointers to `double` in
C++ before entering JavaScript, so progress reporting uses the same JS-safe ABI
as the parser's exported data/result/error paths. The browser Worker/core URLs
are cache-busted to 12.7, and CI requires at least one decoded string progress
stage during the generated Memory64 parser smoke test.

## Pass 12.8 — paged MIDI source / no raw-file-sized WASM allocation

The background parser no longer copies the selected browser `File` into one
contiguous WebAssembly allocation before parsing. `MidiParser` now has a
`parseReadAt()` source interface and the browser Worker exposes the selected
`File` through synchronous `FileReaderSync` windows. The C++ parser keeps at
most a 4 MiB source window resident while its indexing and decode passes move
through the track ranges.

This specifically removes the failure mode where a ~480 MiB MIDI forced the
Memory64 heap to jump from 64 MiB to ~500 MiB before the first event was parsed.
The source file may now be much larger than the parser's transient input RAM;
RAM growth is driven by the parsed indexes/events/notes that are actually
needed, not by the raw file size itself.

The generated-module CI smoke test includes a virtual ~480 MiB MIDI that is
never materialized as one allocation and fails if any source read exceeds
4 MiB. It also keeps the dense-note crashpoint test and the Memory64 progress
ABI checks from earlier passes.

This is an important step toward system-limited huge-MIDI playback, but the Qt
player is still wasm32 and still installs the final serialized `MidiDocument`
monolithically. Files whose *parsed resident document* exceeds that address
space still require the planned segmented player-residency path; Pass 12.8
removes the raw-input allocation ceiling, not that separate final residency
ceiling.

## Pass 13.0 — SharpMIDI-style huge-MIDI memory model

For very large files, WASMIDI no longer stores one permanent visual note plus a
second permanent playback event representation for every note. The selected
browser `File` stays attached to a persistent Memory64 Worker, which treats it as
the browser equivalent of SharpMIDI-raylib's `MemoryMappedFile`.

The Worker keeps a bounded 32 MiB source-page cache and sparse source checkpoints.
The horizontal renderer requests visible note pages, the keyboard requests its
128-key state, and SnappySynth requests exact bounded event batches. Qt receives
only MIDI metadata. This makes source size and total note count independent from
the Qt wasm32 heap and is the intended path for multi-gigabyte / billion-note
MIDIs, subject to actual browser/OS memory and processing-time limits.
