#!/usr/bin/env python3
"""Extract the VOR redundancy filter from voice.c for host testing.

vor_event_is_redundant() decides whether a controller event breaks the voice
stacking sequence. Getting it wrong is audible in both directions: too eager and
notes stack across a real parameter change, too shy and stacking collapses on
controller-dense material. It is pure state logic, so it can be exercised on the
host with no engine around it.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "third_party", "snappysynthv2", "Voice", "voice.c")
DST = os.path.join(ROOT, "tools", "vor_redundancy_logic.inc")
BEGIN = "static unsigned char g_last_cc_value[MIDI_CHANNEL_COUNT][128];"
END = "static inline void vor_break_channel_sequence(int ch) {"


def main():
    text = open(SRC, encoding="utf-8", errors="surrogateescape").read()
    try:
        start = text.index(BEGIN)
        end = text.index(END, start)
    except ValueError:
        sys.exit("markers not found in %s" % SRC)
    open(DST, "w", encoding="utf-8").write(text[start:end])
    print("wrote %s (%d bytes)" % (DST, end - start))


if __name__ == "__main__":
    main()
