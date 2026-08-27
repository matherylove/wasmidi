/*
 * WASMIDI Memory64 heap-growth compatibility shim for Emscripten 3.1.56.
 *
 * In MEMORY64=1 builds, that release can generate growMemory() code which
 * converts a fractional page count such as 10.999984741210938 directly with
 * BigInt(...).  WebAssembly.Memory.grow for an i64 memory needs a BigInt page
 * delta, but BigInt rejects non-integral Numbers.  The requested byte size is
 * already page-aligned by emscripten_resize_heap; nevertheless, explicitly
 * ceil the byte delta to an integer number of 64 KiB pages before converting.
 *
 * This file is linked with --post-js into the parser module only.  Qt and
 * SnappySynth keep their normal Emscripten growth helpers.
 */
(() => {
    const WASM_PAGE_BYTES = 64 * 1024;

    if (typeof growMemory !== "function" ||
        typeof wasmMemory === "undefined") {
        throw new Error("WASMIDI Memory64 growth shim could not find Emscripten runtime symbols.");
    }

    growMemory = function wasmidiGrowMemory64(size) {
        const requestedSize = Number(size);
        const oldHeapSize = Number(wasmMemory.buffer.byteLength);
        const deltaBytes = requestedSize - oldHeapSize;

        if (!Number.isFinite(requestedSize) || !Number.isSafeInteger(requestedSize))
            throw new RangeError("Invalid Memory64 heap size requested: " + String(size));
        if (deltaBytes <= 0)
            return 1;

        const pageCount = Math.ceil(deltaBytes / WASM_PAGE_BYTES);
        if (!Number.isSafeInteger(pageCount) || pageCount <= 0)
            throw new RangeError("Invalid Memory64 page growth: " + String(pageCount));

        try {
            wasmMemory.grow(BigInt(pageCount));
            if (typeof updateMemoryViews === "function")
                updateMemoryViews();
            if (typeof emscriptenMemoryProfiler !== "undefined" &&
                emscriptenMemoryProfiler &&
                typeof emscriptenMemoryProfiler.onMemoryResize === "function") {
                emscriptenMemoryProfiler.onMemoryResize(
                    oldHeapSize, Number(wasmMemory.buffer.byteLength));
            }
            return 1;
        } catch (error) {
            const text =
                "growMemory: Attempted to grow heap from " + oldHeapSize +
                " bytes to " + requestedSize + " bytes (" + pageCount +
                " whole pages), but got error: " + error;
            if (typeof err === "function")
                err(text);
            else if (typeof console !== "undefined" && console.error)
                console.error(text);
        }
        return 0;
    };
})();
