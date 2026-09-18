/* Host check for the VOR redundancy filter in voice.c.
 *
 *   python3 tools/extract_vor_redundancy_logic.py
 *   cc -O1 -Wall -o /tmp/vor tools/vor_redundancy_check.c && /tmp/vor
 *
 * The filter decides whether a controller event breaks voice stacking. Wrong in
 * one direction, notes stack across a real parameter change and the sound
 * differs from the original. Wrong in the other, stacking collapses on
 * controller-dense material, which is the bug this fixes.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MIDI_CHANNEL_COUNT 16
#define EVT_CONTROL_CHANGE 1
#define EVT_PITCH_BEND     2
#define EVT_NOTE_ON        3

typedef struct { int type; int key; int value; } ch_event;

#include "vor_redundancy_logic.inc"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)){ printf("  FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); ++fails; } } while(0)

static int redundant(int ch, int type, int key, int value) {
    ch_event e; e.type = type; e.key = key; e.value = value;
    return vor_event_is_redundant(ch, &e);
}

int main(void) {
    vor_reset_redundancy_shadow();

    printf("1. el primer valor de un CC nunca es redundante\n");
    CHECK(!redundant(0, EVT_CONTROL_CHANGE, 7, 100), "primer CC7 marcado redundante");

    printf("2. repetir el mismo valor si lo es\n");
    CHECK(redundant(0, EVT_CONTROL_CHANGE, 7, 100), "CC7 repetido no detectado");
    CHECK(redundant(0, EVT_CONTROL_CHANGE, 7, 100), "CC7 repetido no detectado (2)");

    printf("3. un valor nuevo rompe, y se vuelve el nuevo estado\n");
    CHECK(!redundant(0, EVT_CONTROL_CHANGE, 7, 101), "cambio de CC7 no rompe");
    CHECK(redundant(0, EVT_CONTROL_CHANGE, 7, 101), "no se actualizo el estado");

    printf("4. los canales son independientes\n");
    CHECK(!redundant(3, EVT_CONTROL_CHANGE, 7, 101), "canal 3 heredo estado del canal 0");

    printf("5. los controladores son independientes\n");
    CHECK(!redundant(0, EVT_CONTROL_CHANGE, 10, 101), "CC10 heredo estado de CC7");

    printf("6. los CC de secuencia nunca son redundantes\n");
    const int sequence_ccs[] = { 6, 38, 98, 99, 100, 101,
                                 120, 121, 122, 123, 124, 125, 126, 127 };
    for (unsigned i = 0; i < sizeof(sequence_ccs)/sizeof(*sequence_ccs); ++i) {
        const int cc = sequence_ccs[i];
        redundant(1, EVT_CONTROL_CHANGE, cc, 64);
        CHECK(!redundant(1, EVT_CONTROL_CHANGE, cc, 64),
              "CC%d repetido marcado redundante; un repeat de estos es una accion real", cc);
    }

    printf("7. el sustain repetido si es redundante, pero on/off no\n");
    vor_reset_redundancy_shadow();
    CHECK(!redundant(2, EVT_CONTROL_CHANGE, 64, 127), "primer sustain");
    CHECK(redundant(2, EVT_CONTROL_CHANGE, 64, 127), "sustain repetido");
    CHECK(!redundant(2, EVT_CONTROL_CHANGE, 64, 0), "sustain off debe romper");

    printf("8. pitch bend: mismo valor redundante, distinto no\n");
    vor_reset_redundancy_shadow();
    CHECK(!redundant(0, EVT_PITCH_BEND, 0, 8192), "primer bend");
    CHECK(redundant(0, EVT_PITCH_BEND, 0, 8192), "bend repetido no detectado");
    CHECK(!redundant(0, EVT_PITCH_BEND, 0, 8300), "bend distinto no rompe");
    CHECK(!redundant(1, EVT_PITCH_BEND, 0, 8300), "bend: canales no independientes");

    printf("9. otros tipos de evento nunca son redundantes\n");
    CHECK(!redundant(0, EVT_NOTE_ON, 60, 100), "note-on marcado redundante");

    printf("10. el reset limpia el estado\n");
    redundant(0, EVT_CONTROL_CHANGE, 11, 40);
    CHECK(redundant(0, EVT_CONTROL_CHANGE, 11, 40), "estado previo al reset");
    vor_reset_redundancy_shadow();
    CHECK(!redundant(0, EVT_CONTROL_CHANGE, 11, 40), "el reset no limpio el estado");

    printf("11. una rampa densa rompe en cada paso; una meseta solo una vez\n");
    vor_reset_redundancy_shadow();
    int breaks = 0;
    for (int v = 0; v < 128; ++v)
        if (!redundant(0, EVT_CONTROL_CHANGE, 11, v)) ++breaks;
    CHECK(breaks == 128, "una rampa de 128 valores debe romper 128 veces, rompio %d", breaks);
    breaks = 0;
    for (int i = 0; i < 1000; ++i)
        if (!redundant(0, EVT_CONTROL_CHANGE, 11, 127)) ++breaks;
    CHECK(breaks == 0, "1000 repeticiones del mismo valor rompieron %d veces", breaks);

    printf("\n%s (%d)\n", fails ? "FALLOS" : "TODO OK", fails);
    return fails ? 1 : 0;
}
