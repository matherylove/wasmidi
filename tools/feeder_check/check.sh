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

# BPFA store vs current store, synth stream (HANDOFF sec. 43)
g++ -O2 -std=c++17 -I../../src -I../../src/midi -o cmp_old store_compare.cpp ../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp ../../src/midi/bpfa_midi_store.cpp || exit 1
g++ -O2 -std=c++17 -DUSE_BPFA -I../../src -I../../src/midi -o cmp_bpfa store_compare.cpp ../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp ../../src/midi/bpfa_midi_store.cpp || exit 1
for w in "0 30" "1.0 20" "5.5 20" "12.3 20" "25 30" "60 5"; do
  set -- $w
  ./cmp_old /tmp/feeder_synth.mid $1 $2 0 /tmp/a.bin 2>/dev/null
  ./cmp_bpfa /tmp/feeder_synth.mid $1 $2 0 /tmp/b.bin 2>/dev/null
  cmp -s /tmp/a.bin /tmp/b.bin && echo "bpfa store start=$1 IDENTICAL" || echo "bpfa store start=$1 DIFFER"
done
rm -f cmp_old cmp_bpfa

# BPFA store vs current store: renderer sweep and live/keyboard snapshots (HANDOFF sec. 44)
SRCS="../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp ../../src/midi/bpfa_midi_store.cpp"
for tool in render_compare live_compare; do
  g++ -O2 -std=c++17 -I../../src -I../../src/midi -o ${tool}_old $tool.cpp $SRCS || exit 1
  g++ -O2 -std=c++17 -DUSE_BPFA -I../../src -I../../src/midi -o ${tool}_bpfa $tool.cpp $SRCS || exit 1
done
for pt in 0 1; do for w in "0 40" "5.5 20"; do
  set -- $w
  ./render_compare_old /tmp/feeder_synth.mid $1 $2 $pt /tmp/ra.bin 2>/dev/null
  ./render_compare_bpfa /tmp/feeder_synth.mid $1 $2 $pt /tmp/rb.bin 2>/dev/null
  cmp -s /tmp/ra.bin /tmp/rb.bin && echo "render perTrack=$pt start=$1 IDENTICAL" || echo "render perTrack=$pt start=$1 DIFFER"
done; done
./live_compare_old /tmp/feeder_synth.mid /tmp/la.bin 0 5 2.5 30 1 2>/dev/null
./live_compare_bpfa /tmp/feeder_synth.mid /tmp/lb.bin 0 5 2.5 30 1 2>/dev/null
cmp -s /tmp/la.bin /tmp/lb.bin && echo "live snapshots IDENTICAL" || echo "live snapshots DIFFER"
rm -f render_compare_old render_compare_bpfa live_compare_old live_compare_bpfa
