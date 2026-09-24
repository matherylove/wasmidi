#!/bin/sh
# Run from tools/feeder_check. See HANDOFF sec. 26.
g++ -O2 -std=c++17 -I../../src -I../../src/midi -o ref ref.cpp ../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp || exit 1
python3 synth_mid.py /tmp/feeder_synth.mid
for w in "0 30" "5.5 20" "12.3 20" "60 5"; do
  set -- $w
  ./ref /tmp/feeder_synth.mid $1 $2 0 /tmp/ref.bin 2>/dev/null
  node jsref.mjs /tmp/feeder_synth.mid $1 $2 0 /tmp/js.bin 2>/dev/null
  cmp -s /tmp/ref.bin /tmp/js.bin && echo "start=$1 IDENTICAL" || echo "start=$1 DIFFER"
  node proto.mjs /tmp/feeder_synth.mid $1 $2 /tmp/proto.bin 2>/dev/null
  printf "start=$1 protocol: "; python3 cmp.py /tmp/ref.bin /tmp/proto.bin
done
