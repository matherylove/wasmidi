/* Host-only shim for the `gcc -fsyntax-only` check of snappy_wasm_core.c on
 * toolchains whose libc lacks POSIX setenv()/unsetenv() (MinGW-w64 on
 * Windows). Emscripten and glibc declare both in <stdlib.h>, so this header
 * is never part of a real build; it exists so the syntax check in HANDOFF.md
 * §2 can run on Windows:
 *
 *   gcc -fsyntax-only -I. -DSNAPPYSYNTH_WASM=1 \
 *       -include ../../tools/host_posix_env_shim.h snappy_wasm_core.c
 *
 * Declarations only. Nothing here is linked or executed. */
#ifndef WASMIDI_HOST_POSIX_ENV_SHIM_H
#define WASMIDI_HOST_POSIX_ENV_SHIM_H

#if defined(__MINGW32__) || defined(__MINGW64__) || defined(_MSC_VER)
#ifdef __cplusplus
extern "C" {
#endif
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
#ifdef __cplusplus
}
#endif
#endif

#endif /* WASMIDI_HOST_POSIX_ENV_SHIM_H */
