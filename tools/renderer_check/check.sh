#!/bin/sh
# Compile-checks src/renderer/*.cpp on a host without Qt/GLES (HANDOFF sec. 34).
# GL entry points are stubbed; this proves the C++ compiles, not that it renders.
set -e
cd "$(dirname "$0")/../.."
STUB=/tmp/wasmidi_glstub
mkdir -p $STUB/GLES3
python3 - "$STUB" <<'PY'
import re, sys
stub = sys.argv[1]
src = "".join(open(p).read() for p in ("src/renderer/gl_renderer.cpp", "src/renderer/gl_renderer.hpp",
                                         "src/renderer/note_raster_compositor.cpp"))
funcs = sorted(set(re.findall(r'\b(gl[A-Z][A-Za-z0-9]*)\s*\(', src)))
consts = sorted(set(re.findall(r'\b(GL_[A-Z0-9_]+)\b', src)))
h = ['#pragma once', '#include <cstddef>', '#include <cstdint>',
     'typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum; typedef int GLsizei;',
     'typedef float GLfloat; typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef char GLchar;',
     'typedef std::ptrdiff_t GLsizeiptr; typedef std::ptrdiff_t GLintptr; typedef unsigned char GLubyte; typedef void GLvoid;']
h += ['#define %s 0x%X' % (c, i + 1) for i, c in enumerate(consts)]
h += ['template<typename... A> inline auto %s(A...) { return 0; }' % f for f in funcs]
open(stub + '/GLES3/gl3.h', 'w').write('\n'.join(h) + '\n')
open(stub + '/emscripten.h', 'w').write(
    '#pragma once\n#define EM_JS(ret, name, params, ...) static ret name params { return ret(); }\n'
    '#define EMSCRIPTEN_KEEPALIVE\n#define EM_ASM(...)\n#define EM_ASM_INT(...) 0\n#define EM_ASM_DOUBLE(...) 0.0\n')
PY
for f in src/renderer/gl_renderer.cpp src/renderer/note_raster_compositor.cpp; do
  g++ -std=c++17 -fsyntax-only -Wall -D__EMSCRIPTEN__ -I$STUB -Isrc $f
  echo "compiles: $f"
done
