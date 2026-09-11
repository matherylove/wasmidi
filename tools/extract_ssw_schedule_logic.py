#!/usr/bin/env python3
"""Extract the block-scheduling logic of snappy_wasm_core.c for host testing.

ssw_render_queued_into() decides how many voice_render_float() calls a device
block costs, which is the dominant term in browser render cost. It is pure
control flow over the scheduled-event queue, so it can be exercised on the host
with stubs instead of a full Emscripten build.

    python3 tools/extract_ssw_schedule_logic.py
    cc -O1 -Wall -Wextra -o /tmp/ssw_harness tools/ssw_schedule_harness.c -lm
    /tmp/ssw_harness
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "third_party", "snappysynthv2", "snappy_wasm_core.c")
DST = os.path.join(ROOT, "tools", "ssw_schedule_logic.inc")

BEGIN = "/* Finest scheduling cell, about 1.45 ms at 44.1 kHz. Multiple of 8. */"
END = "int ssw_active_voices(void)"


def main():
    text = open(SRC, encoding="utf-8").read()
    try:
        start = text.index(BEGIN)
        end = text.index(END)
    except ValueError:
        sys.exit("markers not found in %s; update BEGIN/END" % SRC)
    open(DST, "w", encoding="utf-8").write(text[start:end])
    print("wrote %s (%d bytes)" % (DST, end - start))


if __name__ == "__main__":
    main()
