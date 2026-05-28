#pragma once

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JSDebugLocation {
    JSAtom filename;
    int line;
    int col;
} JSDebugLocation;

int JS_GetCurrentLocation(JSContext *ctx, JSDebugLocation *out_loc);

#ifdef __cplusplus
}
#endif