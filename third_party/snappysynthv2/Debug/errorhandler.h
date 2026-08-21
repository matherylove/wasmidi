#ifndef ERRORHANDLER_H
#define ERRORHANDLER_H

#include "errors.h"

#ifdef __cplusplus
extern "C" {
#endif

void ShowError(const char* error);
void ShowErrorWithLocation(const char* error, int line, const char* file);
void ShowErrorWithMessage(const char* error, const char* message);

#ifdef __cplusplus
}
#endif

#endif // ERRORHANDLER_H
