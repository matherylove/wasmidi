#!/usr/bin/env python3
"""Patch Emscripten 3.1.56's Memory64 growMemory implementation.

Emscripten 3.1.56 has two problems in the JavaScript helper used by
ALLOW_MEMORY_GROWTH when MEMORY64=1:

1. The page delta is computed with floating-point division and is not forced
   back to an integer.
2. The JavaScript WebAssembly.Memory.grow() call receives that delta as a
   Number. For an i64-addressed (Memory64) memory the JS API requires a BigInt.

The first issue can produce fractional values such as 10.999984741210938. Even
when the first issue is fixed with ``| 0``, passing the resulting Number (for
example ``10``) to a Memory64 ``Memory.grow()`` still fails with errors such as
``TypeError: Cannot convert 10 to a BigInt``.

Modern Emscripten handles both requirements: it rounds the page count to an
integer and converts the delta to the memory's index type before calling
Memory.grow().  Qt 6.8 binary packages are tied to Emscripten 3.1.56, so WASMIDI
backports the equivalent behavior into the installed runtime template before
linking.

The patch is intentionally conditional at Emscripten preprocess time: wasm32
builds still call ``Memory.grow(pages)`` with a Number, while MEMORY64 builds
call ``Memory.grow(BigInt(pages))``.  This keeps the global Emscripten runtime
safe for the Qt and SnappySynth wasm32 targets as well as the parser's wasm64
target.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys

OLD_PAGES = (
    "var pages = (size - b.byteLength + {{{ WASM_PAGE_SIZE - 1 }}}) / "
    "{{{ WASM_PAGE_SIZE }}};"
)
FIXED_PAGES = (
    "var pages = ((size - b.byteLength + {{{ WASM_PAGE_SIZE - 1 }}}) / "
    "{{{ WASM_PAGE_SIZE }}}) | 0;"
)

# Emscripten 3.1.56 calls Memory.grow() with a Number even for an i64-addressed
# memory. Make the emitted argument depend on MEMORY64 at JS-library preprocess
# time. We deliberately do not use newer Emscripten's toIndexType() helper,
# because that helper does not exist in the pinned 3.1.56 toolchain.
OLD_GROW = "wasmMemory.grow(pages);"
FIXED_GROW = (
    "wasmMemory.grow({{{ MEMORY64 ? 'BigInt(pages)' : 'pages' }}});"
)

# Later Emscripten versions renamed b -> oldHeapSize and already force the
# page delta to an integer. Recognize those as fixed rather than rewriting them.
NEWER_FIXED_PAGE_MARKERS = (
    "var pages = ((size - oldHeapSize + {{{ WASM_PAGE_SIZE - 1 }}}) / "
    "{{{ WASM_PAGE_SIZE }}}) | 0;",
    "Math.ceil((size - b.byteLength) / {{{ WASM_PAGE_SIZE }}})",
    "Math.ceil((size - oldHeapSize) / {{{ WASM_PAGE_SIZE }}})",
)

# Current Emscripten uses toIndexType('pages'), which expands to BigInt(pages)
# for full wasm64/Memory64 and leaves wasm32 page deltas as Numbers.
NEWER_FIXED_GROW_MARKERS = (
    "wasmMemory.grow({{{ toIndexType('pages') }}});",
    'wasmMemory.grow({{{ toIndexType("pages") }}});',
)


def compiler_path(value: str) -> Path:
    candidate = Path(value)
    if candidate.exists():
        return candidate.resolve()
    found = shutil.which(value)
    if found:
        return Path(found).resolve()
    raise FileNotFoundError(f"cannot resolve Emscripten compiler: {value}")


def locate_library_js(compiler: str) -> Path:
    cc = compiler_path(compiler)
    candidates = [cc.parent / "src" / "library.js"]

    emsdk = os.environ.get("EMSDK")
    if emsdk:
        candidates.append(Path(emsdk) / "upstream" / "emscripten" / "src" / "library.js")

    # Handle wrappers/symlinks one directory above the actual emscripten root.
    candidates.extend([
        cc.parent.parent / "emscripten" / "src" / "library.js",
        cc.parent.parent / "upstream" / "emscripten" / "src" / "library.js",
    ])

    seen: set[Path] = set()
    for path in candidates:
        path = path.resolve()
        if path in seen:
            continue
        seen.add(path)
        if path.is_file():
            return path

    rendered = "\n  ".join(str(p) for p in seen)
    raise FileNotFoundError(
        "could not locate Emscripten src/library.js; checked:\n  " + rendered
    )


def pages_are_fixed(text: str) -> bool:
    return FIXED_PAGES in text or any(
        marker in text for marker in NEWER_FIXED_PAGE_MARKERS
    )


def grow_type_is_fixed(text: str) -> bool:
    return FIXED_GROW in text or any(
        marker in text for marker in NEWER_FIXED_GROW_MARKERS
    )


def patch(path: Path, check_only: bool) -> str:
    text = path.read_text(encoding="utf-8")

    needs_pages = not pages_are_fixed(text)
    needs_grow_type = not grow_type_is_fixed(text)

    if not needs_pages and not needs_grow_type:
        return f"Memory64 growMemory page count/index type already fixed: {path}"

    problems: list[str] = []

    if needs_pages:
        count = text.count(OLD_PAGES)
        if count != 1:
            problems.append(
                "expected exactly one Emscripten 3.1.56 growMemory page-count "
                f"pattern, found {count}"
            )

    if needs_grow_type:
        count = text.count(OLD_GROW)
        if count != 1:
            problems.append(
                "expected exactly one Emscripten 3.1.56 Memory.grow(pages) "
                f"pattern, found {count}"
            )

    if problems:
        raise RuntimeError(
            f"unrecognized Emscripten growMemory implementation in {path}: "
            + "; ".join(problems)
            + "; refusing an unsafe toolchain patch"
        )

    if check_only:
        missing: list[str] = []
        if needs_pages:
            missing.append("integral page rounding")
        if needs_grow_type:
            missing.append("Memory64 BigInt page delta")
        raise RuntimeError(
            f"Emscripten Memory64 growMemory fix is incomplete in {path}: "
            + ", ".join(missing)
        )

    if needs_pages:
        text = text.replace(OLD_PAGES, FIXED_PAGES, 1)
    if needs_grow_type:
        text = text.replace(OLD_GROW, FIXED_GROW, 1)

    path.write_text(text, encoding="utf-8")

    verify = path.read_text(encoding="utf-8")
    if not pages_are_fixed(verify) or not grow_type_is_fixed(verify):
        raise RuntimeError(f"failed to verify patched Emscripten runtime: {path}")

    fixes: list[str] = []
    if needs_pages:
        fixes.append("integral page rounding")
    if needs_grow_type:
        fixes.append("Memory64 BigInt grow delta")
    return f"Patched Emscripten Memory64 growMemory ({', '.join(fixes)}): {path}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", help="CMAKE_CXX_COMPILER / em++ path")
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the installed runtime is already fixed without modifying it",
    )
    args = parser.parse_args()

    try:
        library_js = locate_library_js(args.compiler)
        print(patch(library_js, args.check))
    except Exception as exc:  # noqa: BLE001 - command-line diagnostic
        print(f"WASMIDI Memory64 toolchain patch failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
