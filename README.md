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
