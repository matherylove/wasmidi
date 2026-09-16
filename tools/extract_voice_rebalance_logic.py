#!/usr/bin/env python3
"""Extract the worker freelist rebalance block of voice.c for host testing.

rebalance_worker_freelist() and its helpers are pure data-structure code over
worker_data / the global Treiber pool, so they can be exercised on the host
against stub definitions instead of a full Emscripten build.

    python3 tools/extract_voice_rebalance_logic.py
    cc -O1 -Wall -pthread -o /tmp/b tools/worker_freelist_borrow_check.c && /tmp/b
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "third_party", "snappysynthv2", "Voice", "voice.c")
DST = os.path.join(ROOT, "tools", "voice_rebalance_logic.inc")

BEGIN = "/* BEGIN worker freelist rebalance"
END = "/* END worker freelist rebalance */"


def main():
    text = open(SRC, encoding="utf-8").read()
    try:
        start = text.index(BEGIN)
        end = text.index(END) + len(END)
    except ValueError:
        sys.exit("markers not found in %s; update BEGIN/END" % SRC)
    open(DST, "w", encoding="utf-8", newline="\n").write(text[start:end] + "\n")
    print("wrote %s (%d bytes)" % (DST, end - start))


if __name__ == "__main__":
    main()
