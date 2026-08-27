#ifndef LOGGER_H
#define LOGGER_H

#include "../buildconfig.h"

#ifdef DEBUG
void logger_init(void);
void logger_log(const char* format, ...);
void logger_close(void);
#else
#define logger_init()
#define logger_log(...)
#define logger_close()
#endif

#endif // LOGGER_H
