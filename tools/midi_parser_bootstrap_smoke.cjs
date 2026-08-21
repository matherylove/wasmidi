"use strict";

// Executes the *generated* modularized parser exactly enough to catch startup
// aborts before GitHub Pages deployment. The target includes ENVIRONMENT=node
// solely for this CI test; production still imports the same file in a Worker.
const fs = require("fs");
const path = require("path");

// Emscripten is intentionally built for both `worker` and `node`. The parser's
// EM_JS progress callbacks are optional in Node, but providing the tiny Worker
// surface here makes the smoke test exercise the same callback path that Pages
// uses instead of failing merely because Node has no `self`/`postMessage`.
const parserProgressMessages = [];
if (typeof globalThis.self === "undefined")
    globalThis.self = globalThis;
if (typeof globalThis.postMessage !== "function") {
    globalThis.postMessage = message => {
        parserProgressMessages.push(message);
    };
}

async function main() {
    const jsPath = path.resolve(
        process.argv[2] ||
        path.join("build", "wasmidi-midi-parser.js"));

    if (!fs.existsSync(jsPath))
        throw new Error("Missing generated parser module: " + jsPath);

    delete require.cache[require.resolve(jsPath)];
    const createParser = require(jsPath);

    if (typeof createParser !== "function")
        throw new Error("Generated parser did not export a module factory.");

    let abortReason = "";
    const Module = await createParser({
        noInitialRun: true,
        noExitRuntime: true,
        onAbort(reason) {
            abortReason = String(reason || "unknown abort");
        },
        print() {},
        printErr(text) {
            if (text)
                process.stderr.write(String(text) + "\n");
        }
    });

    if (abortReason)
        throw new Error("Parser aborted during bootstrap: " + abortReason);

    for (const name of [
        "_wmp_parse",
        "_wmp_result_ptr",
        "_wmp_result_size",
        "_wmp_error_ptr",
        "_malloc",
        "_free"
    ]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }

    // Valid format-0 MIDI: header + one track containing only EndOfTrack.
    const midi = Uint8Array.from([
        0x4d, 0x54, 0x68, 0x64,
        0x00, 0x00, 0x00, 0x06,
        0x00, 0x00,
        0x00, 0x01,
        0x01, 0xe0,
        0x4d, 0x54, 0x72, 0x6b,
        0x00, 0x00, 0x00, 0x04,
        0x00, 0xff, 0x2f, 0x00
    ]);

    const ptr = Module._malloc(midi.length);
    if (!ptr)
        throw new Error("Parser smoke test malloc failed.");

    try {
        Module.HEAPU8.set(midi, ptr);
        if (!Module._wmp_parse(ptr, midi.length)) {
            const ep = Module._wmp_error_ptr();
            const detail = ep
                ? Module.UTF8ToString(ep)
                : "unknown parse failure";
            throw new Error("Parser rejected smoke-test MIDI: " + detail);
        }

        const resultPtr = Module._wmp_result_ptr();
        const resultSize = Number(Module._wmp_result_size()) >>> 0;
        if (!resultPtr || resultSize === 0)
            throw new Error("Parser produced an empty serialized document.");

        if (!parserProgressMessages.some(message =>
                message && message.type === "progress")) {
            throw new Error(
                "Parser smoke test completed without exercising progress callbacks.");
        }
    } finally {
        Module._free(ptr);
    }

    console.log("MIDI parser generated-module bootstrap/parse smoke test OK");
}

main().catch(error => {
    console.error(error && error.stack ? error.stack : error);
    process.exitCode = 1;
});
