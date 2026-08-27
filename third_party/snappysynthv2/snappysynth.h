#ifndef SNAPPYSYNTH_H
#define SNAPPYSYNTH_H

#include "buildconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// voice.c uses this only as a fatal-init hook. The browser adapter provides
// a no-op implementation because there is no process/DLL audio backend.
void TerminateSynth(void);

#ifdef __cplusplus
}
#endif

#endif
