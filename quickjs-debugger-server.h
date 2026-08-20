#pragma once

#include "quickjs.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque debug server handle. */
typedef struct JSDebugServer JSDebugServer;

/* Initialize the debug server.
 *
 * - Installs a per-op interrupt handler on `rt` (inert until the script runs).
 * - Installs an exception handler used for "stop on exception".
 * - Listens on (bind_host, port) for a DAP client (VS Code / the MeowEngine
 *   extension). `bind_host` is typically "127.0.0.1".
 *
 * Returns NULL on failure. */
JSDebugServer *JS_DebugServerInit(JSRuntime *rt, JSContext *ctx,
                                  const char *bind_host, int port);

/* Block until a DAP client connects and the launch/attach + configurationDone
 * handshake completes. Breakpoints and exception filters are exchanged here.
 * Returns 0 on success, <0 on error / disconnect. */
int JS_DebugServerAttach(JSDebugServer *srv);

/* One-shot run used by the standalone qjs-debug host:
 *   1. handshake (attach),
 *   2. evaluate `code` as a script/module,
 *   3. pause at entry if requested,
 *   4. run until the script finishes or the client disconnects.
 * `eval_flags` is passed straight to JS_Eval (e.g. JS_EVAL_TYPE_GLOBAL or
 * JS_EVAL_TYPE_MODULE). Returns 0 on success. */
int JS_DebugServerRun(JSDebugServer *srv,
                      const char *code, size_t code_len,
                      const char *filename, int eval_flags);

/* Release all resources associated with the server. */
void JS_DebugServerFree(JSDebugServer *srv);

#ifdef __cplusplus
}
#endif
