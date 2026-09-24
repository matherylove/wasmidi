#!/bin/sh
# Run from tools/host_bench. Extra defines go in $CFLAGS (e.g. CFLAGS=-DSSW_NOTEOFF_FIFO=1).
E=../../third_party/snappysynthv2
SRC="$E/snappy_wasm_core.c $E/Voice/voice.c $E/Parser/sf2_parser.c $E/Parser/sfz_parser.c $E/Parser/wav_loader.c $E/wasm_stubs.c"
gcc -O3 $CFLAGS -I$E -DSNAPPYSYNTH_WASM=1 -pthread -o bench bench.c $SRC -lm
gcc -O3 $CFLAGS -I$E -DSNAPPYSYNTH_WASM=1 -pthread -o midibench midibench.c $SRC -lm
