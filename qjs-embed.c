/*
 * qjs-embed.c — minimal QuickJS-NG embedding host that demonstrates the
 * MeowEngine integration pattern.
 *
 * Design (mirrors what MeowEngine will do):
 *   - A dedicated JS thread runs the QuickJS-NG debug server
 *     (JS_DebugServerInit + JS_DebugServerRun). The server speaks DAP over
 *     TCP, so VS Code (or the MeowEngine extension) attaches to it directly.
 *   - The main thread runs an independent "engine tick" loop. The engine and
 *     the JS runtime are decoupled: while JS is paused at a breakpoint (frozen
 *     in its own thread), the engine keeps ticking. This is what lets the
 *     engine pause its own Tick without freezing JS, and vice versa.
 *
 * NOTE: a QuickJS JSRuntime/JSContext is NOT thread-safe. The JS thread owns
 * `rt`/`ctx`; the engine tick loop must never call JS APIs. Cross-thread
 * interaction happens only through the DAP protocol (handled inside the JS
 * thread) or dedicated host APIs added later.
 *
 * Build (Windows, LLVM/clang + the quickjs-ng static libs):
 *   clang qjs-embed.c -I. -o build-ninja/qjs-embed.exe ^
 *     -L build-ninja -lqjs -lqjs-libc ^
 *     -lws2_32 -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 ^
 *     -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 -loldnames
 *
 * Usage:
 *   qjs-embed [-p PORT] <script.js>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-debugger-server.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[r] = 0;
    *len = r;
    return buf;
}

typedef struct {
    JSDebugServer *srv;
    char *code;
    size_t len;
    char *abspath;
    int *done;
} JsThreadArgs;

/* Runs in the dedicated JS thread: blocks on the DAP handshake and then runs
 * the script, pausing on breakpoints / entry until the client continues. */
static int js_thread(void *opaque)
{
    JsThreadArgs *a = (JsThreadArgs *)opaque;
    JS_DebugServerRun(a->srv, a->code, a->len, a->abspath, JS_EVAL_TYPE_GLOBAL);
    *a->done = 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *filename = NULL;
    int port = 9222;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!filename) {
            filename = argv[i];
        } else {
            break; /* remaining args ignored */
        }
    }

    if (!filename) {
        fprintf(stderr, "usage: qjs-embed [-p PORT] <script.js>\n");
        return 1;
    }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_init_handlers(rt);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    js_init_module_bjson(ctx, "bjson");

    size_t len = 0;
    char *code = read_file(filename, &len);
    if (!code) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    char abspath[1024];
#ifdef _WIN32
    strncpy(abspath, filename, sizeof(abspath) - 1);
    abspath[sizeof(abspath) - 1] = '\0';
#else
    if (realpath(filename, abspath) == NULL)
        strncpy(abspath, filename, sizeof(abspath) - 1);
#endif

    JSDebugServer *srv = JS_DebugServerInit(rt, ctx, "127.0.0.1", port);
    if (!srv) {
        fprintf(stderr, "qjs-embed: failed to start debug server on port %d\n", port);
        free(code);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }
    fprintf(stdout, "Debugger listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    int done = 0;
    JsThreadArgs args = { srv, code, len, abspath, &done };
    thrd_t js_t;
    if (thrd_create(&js_t, js_thread, &args) != thrd_success) {
        fprintf(stderr, "qjs-embed: failed to start JS thread\n");
        JS_DebugServerFree(srv);
        free(code);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Engine "tick" loop (decoupled from JS). Real engine work — updating
     * the scene, physics, etc. — would happen here. While JS is paused at a
     * breakpoint, this loop keeps running; the JS thread is frozen. */
    long ticks = 0;
    while (!done) {
        ticks++;
#ifdef _WIN32
        Sleep(16);
#else
        struct timespec ts = { 0, 16 * 1000 * 1000 };
        nanosleep(&ts, NULL);
#endif
    }

    thrd_join(js_t, NULL);

    JS_DebugServerFree(srv);
    free(code);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
