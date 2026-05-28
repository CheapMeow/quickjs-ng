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

typedef struct JSDebugFrame {
    JSAtom filename;
    JSAtom func_name;
    int line;
    int col;
    uint32_t pc;
} JSDebugFrame;

int JS_GetCurrentLocation(JSContext *ctx, JSDebugLocation *out_loc);

/* Fills out_frames[0..max_frames-1] from innermost to outermost frame.
   Returns the number of frames written, 0 if the stack is empty. */
int JS_GetStackFrames(JSContext *ctx, JSDebugFrame *out_frames, int max_frames);

typedef void (*JSDebugExceptionHandler)(JSContext *ctx, void *opaque);

/* Set a callback invoked when JS_Throw is called (before unwinding).
   The handler runs inside the VM — same constraints as the interrupt handler. */
void JS_SetExceptionHandler(JSRuntime *rt, JSDebugExceptionHandler handler, void *opaque);

#ifdef __cplusplus
}
#endif