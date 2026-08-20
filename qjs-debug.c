/*
 * qjs-debug.c - standalone QuickJS-NG debug host.
 *
 * Runs a single JS file under the DAP debug server so it can be attached to by
 * VS Code (or any DAP client) over TCP. Usage:
 *
 *     qjs-debug [-p PORT] <script.js>
 *
 * The debug server speaks the Debug Adapter Protocol directly; the port it
 * listens on is what the VS Code launch/attach configuration should point at.
 * Whether execution pauses at the first line is controlled by the DAP
 * `stopOnEntry` launch argument (default: true for "launch").
 */

#include "quickjs.h"
#include "quickjs-libc.h"
#include "quickjs-debugger-server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "qjs-debug: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[r] = '\0';
    *out_len = r;
    return buf;
}

/* Resolve `path` to an absolute path so breakpoint source paths reported by the
 * runtime match what VS Code sends (which are absolute). */
static void abs_path(const char *path, char *out, size_t cap)
{
#ifdef _WIN32
    char tmp[1024];
    if (_fullpath(tmp, path, sizeof(tmp)))
        strncpy(out, tmp, cap - 1);
    else
        strncpy(out, path, cap - 1);
#else
    if (realpath(path, out) == NULL)
        strncpy(out, path, cap - 1);
#endif
    out[cap - 1] = '\0';
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
            /* Remaining args are ignored by this minimal host. */
            break;
        }
    }

    if (!filename) {
        fprintf(stderr, "usage: qjs-debug [-p PORT] <script.js>\n");
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
    abs_path(filename, abspath, sizeof(abspath));

    JSDebugServer *srv = JS_DebugServerInit(rt, ctx, "127.0.0.1", port);
    if (!srv) {
        fprintf(stderr, "qjs-debug: failed to start debug server on port %d\n", port);
        free(code);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Print a readiness line so launchers (e.g. the VS Code extension) can
       detect when the DAP server is accepting connections without probing the
       TCP port, which would otherwise consume the single allowed client. */
    fprintf(stdout, "Debugger listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    int rc = JS_DebugServerRun(srv, code, len, abspath, JS_EVAL_TYPE_GLOBAL);

    JS_DebugServerFree(srv);
    free(code);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc == 0 ? 0 : 1;
}
