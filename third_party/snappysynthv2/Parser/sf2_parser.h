#ifndef SF2_PARSER_H
#define SF2_PARSER_H

#include "sfz_parser.h"

typedef struct {
    char name[256];
    char engine[256];
    char product[256];
    char comments[256];
    int preset_count;
    int instrument_count;
    int sample_count;
} sf2_metadata;

int sf2_read_metadata(const char* path, sf2_metadata* out_meta);
sfz_instrument* sf2_load_as_instrument(const char* path);

#endif
