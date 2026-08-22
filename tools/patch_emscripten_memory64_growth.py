#!/usr/bin/env python3
"""Patch the Emscripten 3.1.56 Memory64 growMemory page-count bug.

Emscripten 3.1.56 emits a floating-point page count:
    (size - b.byteLength + 65535) / 65536
For MEMORY64 the JS legalization path converts that value to BigInt. Exact
page-aligned growth therefore becomes e.g. 10.999984741210938 and BigInt()
throws. Newer Emscripten truncates the rounded-up value to an integer with
`| 0` before Memory.grow.

This script patches the installed Emscripten JS runtime template *before link*,
so the fix lives inside MODULARIZE's factory scope rather than in --post-js.
It is idempotent and also accepts newer toolchains that are already fixed.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys

OLD = (
    "var pages = (size - b.byteLength + {{{ WASM_PAGE_SIZE - 1 }}}) / "
    "{{{ WASM_PAGE_SIZE }}};"
)
FIXED = (
    "var pages = ((size - b.byteLength + {{{ WASM_PAGE_SIZE - 1 }}}) / "
    "{{{ WASM_PAGE_SIZE }}}) | 0;"
)

# Later Emscripten versions renamed b -> oldHeapSize and already force the
# result to an integer. Recognize those as fixed rather than rewriting them.
NEWER_FIXED_MARKERS = (
    "var pages = ((size - oldHeapSize + {{{ WASM_PAGE_SIZE - 1 }}}) / "
    "{{{ WASM_PAGE_SIZE }}}) | 0;",
    "Math.ceil((size - b.byteLength) / {{{ WASM_PAGE_SIZE }}})",
    "Math.ceil((size - oldHeapSize) / {{{ WASM_PAGE_SIZE }}})",
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


def is_fixed(text: str) -> bool:
    return FIXED in text or any(marker in text for marker in NEWER_FIXED_MARKERS)


def patch(path: Path, check_only: bool) -> str:
    text = path.read_text(encoding="utf-8")

    if is_fixed(text):
        return f"Memory64 growMemory page count already integral: {path}"

    count = text.count(OLD)
    if count != 1:
        raise RuntimeError(
            f"expected exactly one Emscripten 3.1.56 growMemory pattern in {path}, "
            f"found {count}; refusing an unsafe toolchain patch"
        )

    if check_only:
        raise RuntimeError(
            f"Emscripten Memory64 growMemory bug is still present in {path}"
        )

    patched = text.replace(OLD, FIXED, 1)
    path.write_text(patched, encoding="utf-8")

    verify = path.read_text(encoding="utf-8")
    if OLD in verify or FIXED not in verify:
        raise RuntimeError(f"failed to verify patched Emscripten runtime: {path}")

    return f"Patched Emscripten Memory64 growMemory page rounding: {path}"


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
