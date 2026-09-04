/*
 * QuickJS-NG debug server.
 *
 * Speaks the Debug Adapter Protocol (DAP) over a length-prefixed TCP stream
 * (standard `Content-Length: N\r\n\r\n` framing) and drives pausing through the
 * native debug API (JS_GetCurrentLocation / JS_GetStackFrames /
 * JS_GetFrameVariables) combined with the per-op interrupt handler.
 *
 * The server is intentionally "thin": it does not interpret bytecode itself.
 * All stack / variable inspection is delegated to the native API, and all
 * JSON (de)serialization is delegated to QuickJS itself (JS_ParseJSON /
 * JS_JSONStringify). This keeps the wire logic small and robust.
 */

#include "quickjs.h"
#include "quickjs-debugger.h"
#include "quickjs-debugger-server.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Socket layer (portable between Win32 and POSIX).                    */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define CLOSE_SOCKET closesocket
static int g_wsa_inited = 0;
static void dbg_socket_init(void)
{
    if (g_wsa_inited)
        return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_wsa_inited = 1;
}
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <netdb.h>
#  define CLOSE_SOCKET close
static void dbg_socket_init(void) {}
#endif

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

#define DBG_MAX_FRAMES 64
#define DBG_MAX_VARS   256

/* Scope reference encoding used in `scopes` / `variables` requests. */
#define SCOPE_LOCALS(ref)   ((int)(10000 + (ref)))
#define SCOPE_GLOBALS(ref)  ((int)(20000 + (ref)))
#define IS_LOCALS(ref)      ((ref) >= 10000 && (ref) < 20000)
#define IS_GLOBALS(ref)     ((ref) >= 20000)
#define FRAME_OF(ref)       ((int)(ref) - ((ref) < 20000 ? 10000 : 20000))

typedef enum {
    STEP_NONE = 0,
    STEP_OVER,
    STEP_IN,
    STEP_OUT
} StepMode;

typedef struct Breakpoint {
    char  *filename;
    JSAtom filename_atom;   /* atom of `filename`; enables O(1) compare */
    int    line;
} Breakpoint;

typedef struct Debugger {
    JSRuntime *rt;
    JSContext *ctx;

    int listen_sock;
    int client_sock;

    char  *rbuf;
    size_t rbuf_len;
    size_t rbuf_cap;

    bool running;            /* script is executing (interrupt handler active) */
    bool in_loop;            /* processing DAP messages (guards re-entrant pauses) */
    bool launch_stop_on_entry; /* launch requested stopOnEntry */
    bool has_breakpoints;
    bool stop_on_exception;
    bool pause_requested;
    bool stop_at_entry;      /* transient: pause once at the first line */
    bool exception_pending;
    bool abort;

    StepMode step_mode;
    int step_base_depth;
    char *last_file;
    int  last_line;

    /* Cache for breakpoint check to avoid repeated JS_AtomToCString
       when many consecutive opcodes are on the same source line. */
    JSAtom  bp_cache_filename_atom;
    int     bp_cache_line;
    bool    bp_cache_result;   /* true if the cached location is a hit */

    /* Line we just resumed from after a breakpoint pause. We must not
       re-pause on the same line immediately (the resume opcode is still on
       that line), so breakpoint checks are skipped until the pc leaves it. */
    int     resume_line;

    int seq;

    /* Raw JSON text of the most recently received DAP request frame.
       Saved before JS_ParseJSON so handlers can scrape fields without
       calling QuickJS property APIs (which can hang on ParseJSON'd objs). */
    char *raw_frame;
    size_t raw_frame_len;

    Breakpoint *bps;
    int bp_count;
    int bp_cap;
} Debugger;

/* ------------------------------------------------------------------ */
/* String / path helpers.                                             */
/* ------------------------------------------------------------------ */

static int strnicmp_(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        int ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return ca - cb;
        if (ca == 0)
            return 0;
    }
    return 0;
}

static void *memmem_(const void *hay, size_t hlen, const void *needle, size_t nlen)
{
    if (nlen == 0)
        return (void *)(intptr_t)hay;
    if (hlen < nlen)
        return NULL;
    const char *h = hay;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(h + i, needle, nlen) == 0)
            return (void *)(h + i);
    return NULL;
}

static const char *basename_of(const char *path)
{
    if (!path)
        return "";
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *p = slash;
    if (bslash && (!p || bslash > p))
        p = bslash;
    return p ? p + 1 : path;
}

/* Two source paths match if they are equal or share a basename. The runtime
 * reports module filenames that may differ in prefix from the path VS Code
 * sends, so basename matching keeps breakpoints robust. */
static bool path_matches(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    if (strcmp(a, b) == 0)
        return true;
    return strcmp(basename_of(a), basename_of(b)) == 0;
}

/* ------------------------------------------------------------------ */
/* Socket primitives.                                                 */
/* ------------------------------------------------------------------ */

static int dbg_listen(Debugger *dbg, const char *host, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
#ifdef _WIN32
    int opt = 1;
#else
    int opt = 1;
#endif
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    const char *host_str = (host && *host) ? host : "127.0.0.1";
    inet_pton(AF_INET, host_str, &addr.sin_addr);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CLOSE_SOCKET(s);
        return -1;
    }
    if (listen(s, 1) < 0) {
        CLOSE_SOCKET(s);
        return -1;
    }
    dbg->listen_sock = s;
    return 0;
}

static int dbg_accept(Debugger *dbg)
{
    if (dbg->client_sock >= 0)
        return 0;
    struct sockaddr_in client;
    socklen_t len = sizeof(client);
    int c = accept(dbg->listen_sock, (struct sockaddr *)&client, &len);
    if (c < 0)
        return -1;
    dbg->client_sock = c;
    return 0;
}

static int send_all(int sock, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0)
            return -1;
        total += n;
    }
    return total;
}

/* Read one complete DAP frame (`Content-Length: N\r\n\r\n` + N bytes JSON)
 * from the client socket. Returns a NUL-terminated malloc'd buffer (caller
 * frees) and writes its length to *out_len, or NULL on EOF / error. */
static char *dbg_read_frame(Debugger *dbg, int *out_len)
{
    for (;;) {
        char *hend = memmem_(dbg->rbuf, dbg->rbuf_len, "\r\n\r\n", 4);
        if (hend) {
            /* Parse Content-Length. */
            int content_len = -1;
            for (size_t i = 0; i + 15 <= dbg->rbuf_len; i++) {
                if (strnicmp_(dbg->rbuf + i, "Content-Length:", 15) == 0) {
                    const char *v = dbg->rbuf + i + 15;
                    while (*v == ' ' || *v == '\t')
                        v++;
                    content_len = atoi(v);
                    break;
                }
            }
            if (content_len < 0)
                return NULL;

            char *body = hend + 4;
            size_t body_offset = (size_t)(body - dbg->rbuf);
            if (dbg->rbuf_len - body_offset < (size_t)content_len) {
                /* Need more body bytes; grow once then recv directly. */
                size_t needed = body_offset + (size_t)content_len;
                if (needed > dbg->rbuf_cap) {
                    char *p = realloc(dbg->rbuf, needed + 1024);
                    if (!p)
                        return NULL;
                    dbg->rbuf = p;
                    dbg->rbuf_cap = needed + 1024;
                }
                while (dbg->rbuf_len < needed) {
                    int avail = (int)(needed - dbg->rbuf_len);
                    int n = recv(dbg->client_sock, dbg->rbuf + dbg->rbuf_len, avail, 0);
                    if (n <= 0)
                        return NULL;
                    dbg->rbuf_len += (size_t)n;
                }
                continue; /* rbuf did not move; re-extract */
            }

            char *out = malloc((size_t)content_len + 1);
            if (!out)
                return NULL;
            memcpy(out, body, (size_t)content_len);
            out[content_len] = 0;
            size_t remaining = dbg->rbuf_len - body_offset - (size_t)content_len;
            if (remaining)
                memmove(dbg->rbuf, body + content_len, remaining);
            dbg->rbuf_len = remaining;
            *out_len = content_len;
            return out;
        }

        /* Header not complete yet: ensure capacity then recv more. */
        if (dbg->rbuf_len == dbg->rbuf_cap) {
            size_t nc = dbg->rbuf_cap ? dbg->rbuf_cap * 2 : 8192;
            char *p = realloc(dbg->rbuf, nc);
            if (!p)
                return NULL;
            dbg->rbuf = p;
            dbg->rbuf_cap = nc;
        }
        int avail = (int)(dbg->rbuf_cap - dbg->rbuf_len);
        int n = recv(dbg->client_sock, dbg->rbuf + dbg->rbuf_len, avail, 0);
        if (n <= 0)
            return NULL;
        dbg->rbuf_len += (size_t)n;
    }
}

/* ------------------------------------------------------------------ */
/* JSON out — C-level serializer (avoids JS_JSONStringify which can   */
/* hang when the interrupt handler re-enters during JS execution).     */
/* ------------------------------------------------------------------ */

/* Grow buffer for incremental JSON writing. */
typedef struct JsonBuf {
    char  *buf;
    size_t len;
    size_t cap;
} JsonBuf;

static void jb_init(JsonBuf *jb)
{
    jb->cap = 1024;
    jb->buf = malloc(jb->cap);
    jb->len = 0;
    if (jb->buf)
        jb->buf[0] = '\0';
}

static void jb_free(JsonBuf *jb) { free(jb->buf); }

static void jb_append(JsonBuf *jb, const char *s, size_t slen)
{
    if (!jb->buf) return;
    while (jb->len + slen + 1 > jb->cap) {
        jb->cap *= 2;
        char *p = realloc(jb->buf, jb->cap);
        if (!p) { free(jb->buf); jb->buf = NULL; return; }
        jb->buf = p;
    }
    memcpy(jb->buf + jb->len, s, slen);
    jb->len += slen;
    jb->buf[jb->len] = '\0';
}

static void jb_append_str(JsonBuf *jb, const char *s)
{
    if (s) jb_append(jb, s, strlen(s));
}

static void jb_printf(JsonBuf *jb, const char *fmt, ...)
{
    if (!jb->buf) return;
    va_list ap;
    /* First: determine needed size */
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    /* Ensure capacity */
    while ((size_t)(jb->len + n + 1) > jb->cap) {
        jb->cap *= 2;
        char *p = realloc(jb->buf, jb->cap);
        if (!p) { free(jb->buf); jb->buf = NULL; return; }
        jb->buf = p;
    }
    /* Write directly into buffer */
    va_start(ap, fmt);
    vsnprintf(jb->buf + jb->len, (size_t)(n + 1), fmt, ap);
    va_end(ap);
    jb->len += (size_t)n;
}

/* Escape a string for JSON (handles " and \ and control chars). */
static void jb_json_string(JsonBuf *jb, const char *s)
{
    if (!s) { jb_append_str(jb, "\"\""); return; }
    jb_append(jb, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        switch (ch) {
            case '"':  jb_append_str(jb, "\\\""); break;
            case '\\': jb_append_str(jb, "\\\\"); break;
            case '\b': jb_append_str(jb, "\\b"); break;
            case '\f': jb_append_str(jb, "\\f"); break;
            case '\n': jb_append_str(jb, "\\n"); break;
            case '\r': jb_append_str(jb, "\\r"); break;
            case '\t': jb_append_str(jb, "\\t"); break;
            default:
                if (ch < 0x20) {
                    jb_printf(jb, "\\u%04x", ch);
                } else {
                    jb_append(jb, (char *)s, 1);
                }
                break;
        }
    }
    jb_append(jb, "\"", 1);
}

/* Forward declaration for recursive serialization. */
static void jsvalue_to_json_buf(JSContext *ctx, JsonBuf *jb, JSValue v, int depth);

static void jsvalue_to_json_buf(JSContext *ctx, JsonBuf *jb, JSValue v, int depth)
{
    if (depth > 16 || !jb->buf) { jb_append_str(jb, "null"); return; }

    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT || tag == JS_TAG_FLOAT64) {
        double d;
        JS_ToFloat64(ctx, &d, v);
        /* Print integers without decimal point when possible. */
        if (tag == JS_TAG_INT || d == (double)(int64_t)d) {
            int64_t i; JS_ToInt64(ctx, &i, v);
            jb_printf(jb, "%lld", (long long)i);
        } else {
            jb_printf(jb, "%.17g", d);
        }
        return;
    }
    if (tag == JS_TAG_STRING) {
        const char *s = JS_ToCString(ctx, v);
        jb_json_string(jb, s ? s : "");
        JS_FreeCString(ctx, s);
        return;
    }
    if (JS_IsNull(v) || JS_IsUndefined(v)) { jb_append_str(jb, "null"); return; }
    if (JS_IsBool(v)) {
        jb_append_str(jb, JS_ToBool(ctx, v) ? "true" : "false");
        return;
    }
    if (JS_IsArray(v)) {
        int64_t len;
        JS_GetLength(ctx, v, &len);
        jb_append(jb, "[", 1);
        for (int i = 0; i < len && i < DBG_MAX_VARS && jb->buf; i++) {
            if (i > 0) jb_append(jb, ",", 1);
            JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            jsvalue_to_json_buf(ctx, jb, el, depth + 1);
            JS_FreeValue(ctx, el);
        }
        jb_append(jb, "]", 1);
        return;
    }
    if (tag == JS_TAG_OBJECT) {
        /* Enumerate own string properties. */
        uint32_t prop_count = 0;
        JSPropertyEnum *props = NULL;
        JS_GetOwnPropertyNames(ctx, &props, &prop_count, v,
                               JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK);
        jb_append(jb, "{", 1);
        for (uint32_t i = 0; i < prop_count && jb->buf; i++) {
            if (i > 0) jb_append(jb, ",", 1);
            const char *key = JS_AtomToCString(ctx, props[i].atom);
            jb_json_string(jb, key ? key : "");
            JS_FreeCString(ctx, key);
            jb_append(jb, ":", 1);
            JSValue val = JS_GetProperty(ctx, v, props[i].atom);
            jsvalue_to_json_buf(ctx, jb, val, depth + 1);
            JS_FreeValue(ctx, val);
        }
        jb_append(jb, "}", 1);
        for (uint32_t i = 0; i < prop_count; i++)
            JS_FreeAtom(ctx, props[i].atom);
        free(props);
        return;
    }
    /* Fallback for any other type (function, symbol, etc.) */
    jb_append_str(jb, "null");
}

/* Convert a JSValue to a heap-allocated JSON string. Caller must free(). */
static char *jsvalue_to_json(JSContext *ctx, JSValue v)
{
    JsonBuf jb;
    jb_init(&jb);
    jsvalue_to_json_buf(ctx, &jb, v, 0);
    return jb.buf; /* transfer ownership */
}

static int send_json_string(Debugger *dbg, const char *json_str)
{
    if (dbg->client_sock < 0 || !json_str)
        return -1;
    size_t len = strlen(json_str);
    char header[64];
    int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
    int r = 0;
    if (send_all(dbg->client_sock, header, hlen) != hlen)
        r = -1;
    if (send_all(dbg->client_sock, json_str, (int)len) != (int)len)
        r = -1;
    return r;
}

static int send_json_value(Debugger *dbg, JSValue obj)
{
    JSContext *ctx = dbg->ctx;
    char *json_str = jsvalue_to_json(ctx, obj);
    int r = send_json_string(dbg, json_str);
    free(json_str);
    return r;
}

static void send_response(Debugger *dbg, int req_seq, const char *command, JSValue body)
{
    JSContext *ctx = dbg->ctx;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "seq", JS_NewInt32(ctx, ++dbg->seq));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "response"));
    JS_SetPropertyStr(ctx, obj, "request_seq", JS_NewInt32(ctx, req_seq));
    JS_SetPropertyStr(ctx, obj, "success", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "command", JS_NewString(ctx, command));
    if (JS_IsUndefined(body))
        body = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "body", body);
    send_json_value(dbg, obj);
    JS_FreeValue(ctx, obj);
}

static void send_event(Debugger *dbg, const char *name, JSValue body)
{
    JSContext *ctx = dbg->ctx;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "seq", JS_NewInt32(ctx, ++dbg->seq));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "event"));
    JS_SetPropertyStr(ctx, obj, "event", JS_NewString(ctx, name));
    if (JS_IsUndefined(body))
        body = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "body", body);
    send_json_value(dbg, obj);
    JS_FreeValue(ctx, obj);
}

static void send_output(Debugger *dbg, const char *text)
{
    /* Send output event using manual JSON to avoid JS object creation hang. */
    int seq = ++dbg->seq;
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"seq\":%d,\"type\":\"event\",\"event\":\"output\","
        "\"body\":{\"category\":\"stderr\",\"output\":\"%s\","
        "\"line\":1,\"column\":1}}",
        seq, text ? text : "");
    send_json_string(dbg, buf);
}

/* Send a DAP response with a pre-built JSON body string.
   This avoids creating JS objects inside dispatch handlers which can
   hang in quickjs-ng when called from the pause loop. */
static void send_raw_response(Debugger *dbg, int req_seq, const char *command,
                              const char *body_json)
{
    int seq = ++dbg->seq;
    char buf[8192];
    snprintf(buf, sizeof(buf),
        "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
        "\"command\":\"%s\",\"success\":true%s%s}",
        seq, req_seq, command,
        body_json ? ",\"body\":" : "",
        body_json ? body_json : "");
    send_json_string(dbg, buf);
}

/* Send a DAP event with a pre-built JSON body string. */
static void send_raw_event(Debugger *dbg, const char *event_name,
                            const char *body_json)
{
    int seq = ++dbg->seq;
    char buf[8192];
    snprintf(buf, sizeof(buf),
        "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\"%s%s}",
        seq, event_name,
        body_json ? ",\"body\":" : "",
        body_json ? body_json : "");
    send_json_string(dbg, buf);
}

/* ------------------------------------------------------------------ */
/* JSON value helpers.                                                */
/* ------------------------------------------------------------------ */

/* Escape a string for inclusion in a JSON string value.
   Writes result into dst (up to dst_size bytes including NUL terminator).
   Returns number of chars written (excluding NUL) or -1 on overflow. */
static int json_escape(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) return -1;
    size_t j = 0;
    for (; *src && j < dst_size - 1; src++) {
        unsigned char ch = (unsigned char)*src;
        switch (ch) {
            case '"':  if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = '"'; } else return -1; break;
            case '\\': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = '\\'; } else return -1; break;
            case '\n': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'n'; } else return -1; break;
            case '\r': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'r'; } else return -1; break;
            case '\t': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 't'; } else return -1; break;
            default:
                if (ch < 0x20) { /* control char → \uXXXX */
                    if (j + 6 < dst_size) {
                        j += snprintf(dst + j, dst_size - j, "\\u%04x", ch);
                    } else return -1;
                } else {
                    dst[j++] = (char)ch;
                }
                break;
        }
    }
    dst[j] = '\0';
    return (int)j;
}

static const char *js_str_prop(JSContext *ctx, JSValue obj, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    return s; /* caller must JS_FreeCString */
}

/* Return a human-readable QuickJS value type name. */
static const char *js_val_type_tag(JSValue v)
{
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT || tag == JS_TAG_FLOAT64) return "number";
    if (tag == JS_TAG_STRING) return "string";
    if (JS_IsNull(v)) return "null";
    if (JS_IsUndefined(v)) return "undefined";
    if (JS_IsBool(v)) return "boolean";
    if (tag == JS_TAG_OBJECT) return "object";
    if (JS_IsArray(v)) return "array";
    return "unknown";
}

/* Whether a variable name is safe to use as a JS function parameter. */
static int is_js_identifier(const char *s)
{
    if (!s || !*s)
        return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_' || s[0] == '$'))
        return 0;
    for (const char *p = s + 1; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$'))
            return 0;
    return 1;
}

static int js_int_prop(JSContext *ctx, JSValue obj, const char *name, int def)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int out = def;
    JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

static const char *tag_name(int tag)
{
    switch (tag) {
    case JS_TAG_UNDEFINED: return "undefined";
    case JS_TAG_NULL:      return "null";
    case JS_TAG_BOOL:      return "boolean";
    case JS_TAG_INT:       return "number";
    case JS_TAG_FLOAT64:   return "number";
    case JS_TAG_STRING:    return "string";
    case JS_TAG_SYMBOL:    return "symbol";
    case JS_TAG_OBJECT:    return "object";
    case JS_TAG_MODULE:    return "module";
    case JS_TAG_FUNCTION_BYTECODE: return "function";
    case JS_TAG_EXCEPTION: return "exception";
    default:               return "unknown";
    }
}

/* Build a DAP variable object from a name + JS value. */
static JSValue make_variable(JSContext *ctx, const char *name, JSValue val)
{
    JSValue v = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, v, "name", JS_NewString(ctx, name ? name : ""));
    JSValue str = JS_ToString(ctx, val);
    const char *s = JS_ToCString(ctx, str);
    JS_SetPropertyStr(ctx, v, "value", JS_NewString(ctx, s ? s : "<?>"));
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, str);
    JS_SetPropertyStr(ctx, v, "type", JS_NewString(ctx, tag_name(JS_VALUE_GET_TAG(val))));
    JS_SetPropertyStr(ctx, v, "variablesReference", JS_NewInt32(ctx, 0));
    return v;
}

static JSValue build_capabilities(JSContext *ctx)
{
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "supportsConfigurationDoneRequest", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "supportsStepOver", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "supportsStepIn", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "supportsStepOut", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "supportsEvaluateForHovers", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "supportsSetVariable", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, obj, "supportsRestartRequest", JS_NewBool(ctx, false));
    return obj;
}

/* ------------------------------------------------------------------ */
/* Breakpoint bookkeeping.                                            */
/* ------------------------------------------------------------------ */

static void add_breakpoint(Debugger *dbg, const char *path, int line)
{
    if (dbg->bp_count == dbg->bp_cap) {
        int nc = dbg->bp_cap ? dbg->bp_cap * 2 : 8;
        Breakpoint *p = realloc(dbg->bps, sizeof(Breakpoint) * nc);
        if (!p)
            return;
        dbg->bps = p;
        dbg->bp_cap = nc;
    }
    dbg->bps[dbg->bp_count].filename = strdup(path ? path : "");
    dbg->bps[dbg->bp_count].filename_atom = JS_NewAtom(dbg->ctx, path ? path : "");
    dbg->bps[dbg->bp_count].line = line;
    dbg->bp_count++;
}

static void remove_bps_for_source(Debugger *dbg, const char *path)
{
    int w = 0;
    for (int i = 0; i < dbg->bp_count; i++) {
        if (path_matches(dbg->bps[i].filename, path)) {
            free(dbg->bps[i].filename);
            JS_FreeAtom(dbg->ctx, dbg->bps[i].filename_atom);
            continue;
        }
        dbg->bps[w++] = dbg->bps[i];
    }
    dbg->bp_count = w;
}

static bool breakpoint_hit(Debugger *dbg, JSAtom fn_atom, int line)
{
    /* Fast path: exact atom match. The runtime reports the source filename
       as a JSAtom; when it equals the atom we stored for the breakpoint
       (same path string), this is an O(bp_count) integer compare with no
       allocation. This is the hot path hit on every interrupt poll. */
    for (int i = 0; i < dbg->bp_count; i++)
        if (dbg->bps[i].line == line && dbg->bps[i].filename_atom == fn_atom)
            return true;

    /* Slow path: the breakpoint path and the runtime's filename may differ
       only in a prefix, so fall back to basename matching. This requires a
       CString conversion but is rare (and cached by the caller). */
    const char *fn = JS_AtomToCString(dbg->ctx, fn_atom);
    bool hit = false;
    for (int i = 0; i < dbg->bp_count; i++) {
        if (dbg->bps[i].line == line && path_matches(dbg->bps[i].filename, fn)) {
            hit = true;
            break;
        }
    }
    JS_FreeCString(dbg->ctx, fn);
    return hit;
}

/* ------------------------------------------------------------------ */
/* Exception + interrupt handlers (the pause triggers).               */
/* ------------------------------------------------------------------ */

static void enter_pause_loop(Debugger *dbg, const char *reason);
static bool dbg_dispatch(Debugger *dbg, JSValue req);

static void exception_handler(JSContext *ctx, void *opaque)
{
    Debugger *dbg = opaque;
    dbg->exception_pending = true;
}

/* Called by the VM at frequent safe points (per-op once armed). Decides
 * whether to pause and, if so, runs the blocking message loop. */
static int debugger_check(JSRuntime *rt, void *opaque)
{
    Debugger *dbg = opaque;
    JSContext *ctx = dbg->ctx;

    if (dbg->abort)
        return 1; /* abort the script */
    if (!dbg->running || dbg->in_loop)
        return 0;

    /* Pause once at the very first line. */
    if (dbg->stop_at_entry) {
        dbg->stop_at_entry = false;
        enter_pause_loop(dbg, "entry");
        return 0;
    }

    if (dbg->pause_requested) {
        dbg->pause_requested = false;
        enter_pause_loop(dbg, "pause");
        return 0;
    }

    if (dbg->has_breakpoints) {
        JSDebugLocation loc;
        if (JS_GetCurrentLocation(ctx, &loc)) {
            bool hit;
            /* Don't re-pause on the line we just resumed from: the resume
               opcode is still on that line, so a poll there would hit the
               same breakpoint again and deadlock. Skip until we leave it. */
            if (loc.line == dbg->resume_line) {
                hit = false;
            } else {
                dbg->resume_line = 0;
                /* Use cache if we're on the same filename atom + line. */
                if (dbg->bp_cache_filename_atom == loc.filename &&
                    dbg->bp_cache_line == loc.line) {
                    hit = dbg->bp_cache_result;
                } else {
                    hit = breakpoint_hit(dbg, loc.filename, loc.line);
                    dbg->bp_cache_filename_atom = loc.filename;
                    dbg->bp_cache_line = loc.line;
                    dbg->bp_cache_result = hit;
                }
            }
            if (hit) {
                /* Remember the line so a continue from here won't re-hit it. */
                dbg->resume_line = loc.line;
                dbg->bp_cache_filename_atom = JS_ATOM_NULL;
                enter_pause_loop(dbg, "breakpoint");
                return 0;
            }
        }
    }

    if (dbg->step_mode != STEP_NONE) {
        JSDebugLocation loc;
        if (JS_GetCurrentLocation(ctx, &loc)) {
            const char *fn = JS_AtomToCString(ctx, loc.filename);
            int depth = JS_GetFrameDepth(ctx);
            bool stop = false;
            if (dbg->step_mode == STEP_OVER) {
                if (depth <= dbg->step_base_depth &&
                    (strcmp(fn, dbg->last_file ? dbg->last_file : "") != 0 ||
                     loc.line != dbg->last_line))
                    stop = true;
            } else if (dbg->step_mode == STEP_IN) {
                if (strcmp(fn, dbg->last_file ? dbg->last_file : "") != 0 ||
                    loc.line != dbg->last_line)
                    stop = true;
            } else if (dbg->step_mode == STEP_OUT) {
                if (depth < dbg->step_base_depth)
                    stop = true;
            }
            JS_FreeCString(ctx, fn);
            if (stop) {
                enter_pause_loop(dbg, "step");
                return 0;
            }
        }
    }

    if (dbg->exception_pending && dbg->stop_on_exception) {
        dbg->exception_pending = false;
        enter_pause_loop(dbg, "exception");
        return 0;
    }

    /* Re-arm the interrupt counter.
     * Step mode needs per-opcode granularity (counter=1).
     * Breakpoint-only checking uses a small-enough interval to catch
     * breakpoints in small functions (~5 opcodes), while the
     * breakpoint cache prevents redundant work on same-line opcodes. */
    if (dbg->step_mode != STEP_NONE)
        JS_DebugSetInterruptCounter(ctx, 1);        /* every opcode */
    else if (dbg->has_breakpoints)
        JS_DebugSetInterruptCounter(ctx, 5);         /* every ~5 opcodes */

    return 0;
}

/* ------------------------------------------------------------------ */
/* Message loop (runs both during the pre-run handshake and while     */
/* paused at a breakpoint).                                           */
/* ------------------------------------------------------------------ */

static void record_step_base(Debugger *dbg)
{
    JSContext *ctx = dbg->ctx;
    JSDebugLocation loc;
    if (JS_GetCurrentLocation(ctx, &loc)) {
        const char *fn = JS_AtomToCString(ctx, loc.filename);
        free(dbg->last_file);
        dbg->last_file = strdup(fn ? fn : "");
        dbg->last_line = loc.line;
        JS_FreeCString(ctx, fn);
    }
    dbg->step_base_depth = JS_GetFrameDepth(ctx);
}

/* The blocking loop entered whenever execution is suspended. Reads a single
 * DAP request, dispatches it, and either keeps looping (stay paused) or
 * returns (resume). */
static void enter_pause_loop(Debugger *dbg, const char *reason)
{
    JSContext *ctx = dbg->ctx;
    dbg->in_loop = true;

    /* Send stopped event using manual JSON. */
    JSDebugLocation loc;
    JS_GetCurrentLocation(ctx, &loc);
    char reason_esc[128];
    json_escape(reason ? reason : "", reason_esc, sizeof(reason_esc));
    char stopped_json[256];
    snprintf(stopped_json, sizeof(stopped_json),
        "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true,\"line\":%d}",
        reason_esc, loc.line);
    send_raw_event(dbg, "stopped", stopped_json);

    dbg->step_mode = STEP_NONE;

    bool resume = false;
    while (!resume && dbg->client_sock >= 0 && !dbg->abort) {
        int len;
        char *frame = dbg_read_frame(dbg, &len);
        if (!frame) {
            dbg->abort = true;
            break;
        }
        /* Save raw frame for handlers that need to scrape without
           using QuickJS property APIs on ParseJSON'd objects. */
        free(dbg->raw_frame);
        dbg->raw_frame = frame;
        dbg->raw_frame_len = (size_t)len;

        JSValue req = JS_ParseJSON(ctx, frame, (size_t)len, "<dap>");
        if (JS_IsException(req)) {
            JS_FreeValue(ctx, req);
            continue;
        }
        resume = dbg_dispatch(dbg, req);
        JS_FreeValue(ctx, req);
    }

    /* Re-arm so the VM keeps invoking debugger_check after we resume,
       allowing subsequent breakpoints / step requests to be honored.
       (Each pause branch in debugger_check returns before reaching its
       own re-arm logic, so we must re-arm here.) */
    if (dbg->has_breakpoints || dbg->step_mode != STEP_NONE ||
        dbg->pause_requested || dbg->stop_on_exception || dbg->exception_pending)
        JS_DebugSetInterruptCounter(ctx, 1);

    dbg->in_loop = false;
}

static int handle_set_breakpoints(Debugger *dbg, JSValue req, int req_seq)
{
    /* Build response entirely from raw JSON scraping + snprintf.
       Avoids creating JS objects inside this handler which can hang
       in quickjs-ng when called from deep within the handshake loop
       after ParseJSON objects have been enumerated. */

    const char *raw = dbg->raw_frame ? dbg->raw_frame : "";
    char path_buf[1024];
    path_buf[0] = '\0';

    /* Scrape source path from raw JSON. */
    const char *p = strstr(raw, "\"path\"");
    if (p) {
        p = strstr(p, ":");
        if (p) {
            while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
            if (*p == '"') { p++; }
            const char *end = strchr(p, '"');
            if (end) {
                size_t len = (size_t)(end - p);
                if (len >= sizeof(path_buf)) len = sizeof(path_buf) - 1;
                memcpy(path_buf, p, len);
                path_buf[len] = '\0';
            }
        }
    }

    remove_bps_for_source(dbg, path_buf);

    /* Scrape breakpoint lines and build response JSON directly. */
    int bps[256];
    int bp_count = 0;
    const char *bp_start = strstr(raw, "\"breakpoints\":[");
    if (bp_start) {
        const char *bp = bp_start;
        while ((bp = strstr(bp, "\"line\":")) != NULL && bp_count < 256) {
            bp += 7;
            int line = atoi(bp);
            if (line > 0) {
                bps[bp_count++] = line;
                add_breakpoint(dbg, path_buf, line);
            }
        }
    }

    /* Manually construct the DAP response JSON. */
    int seq = ++dbg->seq;
    char resp[4096];
    int pos = snprintf(resp, sizeof(resp),
        "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
        "\"command\":\"setBreakpoints\",\"success\":true,"
        "\"body\":{\"breakpoints\":[",
        seq, req_seq);
    for (int i = 0; i < bp_count; i++) {
        pos += snprintf(resp + pos, sizeof(resp) - (size_t)pos,
            "%s{\"id\":%d,\"verified\":true,\"line\":%d,"
            "\"source\":{\"path\":\"%s\"}}",
            i > 0 ? "," : "", i + 1, bps[i], path_buf);
    }
    pos += snprintf(resp + pos, sizeof(resp) - (size_t)pos, "]}}");

    /* Send the pre-built JSON directly. */
    send_json_string(dbg, resp);

    dbg->has_breakpoints = (dbg->bp_count > 0);
    if (dbg->has_breakpoints)
        JS_DebugSetInterruptCounter(dbg->ctx, 1);

    return 0;
}

static int handle_set_exception_bp(Debugger *dbg, JSValue req, int req_seq)
{
    JSContext *ctx = dbg->ctx;
    JSAtom args_atom = JS_NewAtom(ctx, "arguments");
    JSAtom filt_atom = JS_NewAtom(ctx, "filters");
    JSValue args = JS_GetProperty(ctx, req, args_atom);
    JSValue filters = JS_GetProperty(ctx, args, filt_atom);
    dbg->stop_on_exception = false;
    if (JS_IsArray(filters)) {
        int64_t n;
        JS_GetLength(ctx, filters, &n);
        for (uint32_t i = 0; i < n; i++) {
            JSValue f = JS_GetPropertyUint32(ctx, filters, i);
            const char *s = JS_ToCString(ctx, f);
            if (s && (strcmp(s, "uncaught") == 0 || strcmp(s, "all") == 0))
                dbg->stop_on_exception = true;
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, f);
        }
    }
    send_response(dbg, req_seq, "setExceptionBreakpoints", JS_UNDEFINED);
    if (dbg->stop_on_exception)
        JS_DebugSetInterruptCounter(ctx, 1);

    /* Clean up. */
    JS_FreeValue(ctx, filters);
    JS_FreeAtom(ctx, args_atom);
    JS_FreeAtom(ctx, filt_atom);
    return 0;
}

/* Returns true if the message loop should resume execution. */
static bool dbg_dispatch(Debugger *dbg, JSValue req)
{
    JSContext *ctx = dbg->ctx;
    const char *command = js_str_prop(ctx, req, "command");
    int req_seq = js_int_prop(ctx, req, "seq", 0);

    if (command && strcmp(command, "initialize") == 0) {
        send_response(dbg, req_seq, "initialize", build_capabilities(ctx));
    } else if (command && (strcmp(command, "launch") == 0 || strcmp(command, "attach") == 0)) {
        JSValue args = JS_GetPropertyStr(ctx, req, "arguments");
        bool stopOnEntry = (strcmp(command, "attach") != 0);
        JSValue soe = JS_GetPropertyStr(ctx, args, "stopOnEntry");
        if (!JS_IsUndefined(soe))
            stopOnEntry = JS_ToBool(ctx, soe);
        dbg->launch_stop_on_entry = stopOnEntry;
        /* args is a child of req — caller will free req, so we must NOT
           free args here to avoid double-free. */
        send_response(dbg, req_seq, command, JS_UNDEFINED);
        send_event(dbg, "initialized", JS_UNDEFINED);
    } else if (command && strcmp(command, "setBreakpoints") == 0) {
        handle_set_breakpoints(dbg, req, req_seq);
    } else if (command && strcmp(command, "setExceptionBreakpoints") == 0) {
        handle_set_exception_bp(dbg, req, req_seq);
    } else if (command && strcmp(command, "configurationDone") == 0) {
        send_response(dbg, req_seq, "configurationDone", JS_UNDEFINED);
        JS_FreeCString(ctx, command);
        return true; /* handshake complete */
    } else if (command && strcmp(command, "threads") == 0) {
        send_raw_response(dbg, req_seq, "threads",
            "{\"threads\":[{\"id\":1,\"name\":\"QuickJS Main\"}]}");
    } else if (command && strcmp(command, "stackTrace") == 0) {
        /* Use native debug API to get stack frames — these are C-level
           calls that don't create JS objects. */
        JSDebugFrame frames[DBG_MAX_FRAMES];
        int n = JS_GetStackFrames(ctx, frames, DBG_MAX_FRAMES);

        /* If no frames from the API (e.g., stopped at entry before any
           bytecode has executed), synthesize a top-level frame.  Always
           provide at least one frame so the debugger UI has something to show. */
        if (n == 0) {
            JSDebugLocation loc;
            if (JS_GetCurrentLocation(ctx, &loc)) {
                frames[0].line = loc.line;
                frames[0].col = loc.col;
                frames[0].filename = loc.filename;
            } else {
                /* No location info available — use defaults. */
                frames[0].line = 1;
                frames[0].col = 1;
                frames[0].filename = JS_NewAtom(ctx, "<eval>");
            }
            frames[0].func_name = JS_NewAtom(ctx, "<global>");
            n = 1;
        }

        /* Build JSON manually. */
        char body_buf[8192];
        int pos = snprintf(body_buf, sizeof(body_buf),
            "{\"stackFrames\":[");
        for (int i = 0; i < n && pos > 0; i++) {
            int line = frames[i].line;
            int col = frames[i].col;
            const char *fn = JS_AtomToCString(ctx, frames[i].filename);
            const char *func = JS_AtomToCString(ctx, frames[i].func_name);
            if (i == 0) {
                /* Use precise current location for top frame. */
                JSDebugLocation loc;
                if (JS_GetCurrentLocation(ctx, &loc)) {
                    line = loc.line;
                    col = loc.col;
                    const char *fn2 = JS_AtomToCString(ctx, loc.filename);
                    JS_FreeCString(ctx, fn);
                    fn = fn2;
                }
            }
            /* Escape strings for JSON. */
            char esc_fn[512], esc_func[512];
            json_escape(fn ? fn : "<unknown>", esc_fn, sizeof(esc_fn));
            json_escape(func ? func : "<anonymous>", esc_func, sizeof(esc_func));

            pos += snprintf(body_buf + pos, sizeof(body_buf) - (size_t)pos,
                "%s{\"id\":%d,\"name\":\"%s\","
                "\"source\":{\"path\":\"%s\"},"
                "\"line\":%d,\"column\":%d}",
                i > 0 ? "," : "", i, esc_func, esc_fn,
                line, col + 1);

            JS_FreeCString(ctx, fn);
            JS_FreeCString(ctx, func);
            /* frames[i].filename / func_name are borrowed atoms from
               JS_GetStackFrames (no refcount) -- do NOT free them. */
        }
        pos += snprintf(body_buf + pos, sizeof(body_buf) - (size_t)pos,
            "],\"totalFrames\":%d}", n);
        send_raw_response(dbg, req_seq, "stackTrace", body_buf);
    } else if (command && strcmp(command, "scopes") == 0) {
        /* Scrape frameId from raw JSON to avoid GetPropertyStr hang. */
        int frame_id = 0;
        const char *raw = dbg->raw_frame ? dbg->raw_frame : "";
        const char *fi = strstr(raw, "\"frameId\"");
        if (fi) {
            fi = strstr(fi, ":");
            if (fi) frame_id = atoi(fi + 1);
        }
        char body_buf[1024];
        snprintf(body_buf, sizeof(body_buf),
            "{\"scopes\":["
            "{\"name\":\"Locals\",\"presentationHint\":\"locals\","
            "\"variablesReference\":%d},"
            "{\"name\":\"Globals\",\"presentationHint\":\"globals\","
            "\"variablesReference\":%d}]}",
            SCOPE_LOCALS(frame_id), SCOPE_GLOBALS(frame_id));
        send_raw_response(dbg, req_seq, "scopes", body_buf);
    } else if (command && strcmp(command, "variables") == 0) {
        /* Scrape variablesReference from raw JSON. */
        int ref = 0;
        const char *raw = dbg->raw_frame ? dbg->raw_frame : "";
        const char *ri = strstr(raw, "\"variablesReference\"");
        if (ri) {
            ri = strstr(ri, ":");
            if (ri) ref = atoi(ri + 1);
        }

        /* Build variables array as JSON string. */
        char body_buf[8192];
        int pos = snprintf(body_buf, sizeof(body_buf), "{\"variables\":[");

        if (IS_LOCALS(ref)) {
            int idx = FRAME_OF(ref);
            JSDebugVariable vars[DBG_MAX_VARS];
            int n = JS_GetFrameVariables(ctx, idx, vars, DBG_MAX_VARS);
            int emitted = 0;
            for (int i = 0; i < n && pos > 0; i++) {
                /* `let`/`const` slots that are still in their temporal dead
                   zone hold JS_UNINITIALIZED; converting those to a string is
                   not meaningful (and upsets the runtime), so report them as
                   such instead. */
                bool uninit = JS_IsUninitialized(vars[i].value);

                const char *name = JS_AtomToCString(ctx, vars[i].name);
                /* Get a printable representation of the value. */
                char val_buf[256] = "<complex>";
                if (uninit) {
                    snprintf(val_buf, sizeof(val_buf), "<uninitialized>");
                } else {
                    const char *vstr = JS_ToCString(ctx, vars[i].value);
                    if (vstr) {
                        json_escape(vstr, val_buf, sizeof(val_buf));
                        JS_FreeCString(ctx, vstr);
                    }
                }

                char esc_name[256];
                json_escape(name ? name : "?", esc_name, sizeof(esc_name));

                pos += snprintf(body_buf + pos, sizeof(body_buf) - (size_t)pos,
                    "%s{\"name\":\"%s\",\"value\":\"%s\",\"type\":\"%s\","
                    "\"variablesReference\":0}",
                    emitted > 0 ? "," : "", esc_name, val_buf,
                    uninit ? "undefined" : js_val_type_tag(vars[i].value));
                emitted++;

                JS_FreeCString(ctx, name);
                /* Both vars[i].name and vars[i].value are borrowed from the
                   live stack frame (JS_GetFrameVariables does not add a
                   reference), so neither may be released here -- doing so
                   underflows the refcount and corrupts the frame. */
            }
        } else if (IS_GLOBALS(ref)) {
            /* For globals, use a simple subset to avoid enumeration hang. */
            pos += snprintf(body_buf + pos, sizeof(body_buf) - (size_t)pos,
                "{\"name\":\"(globals)\",\"value\":\"<...>\",\"type\":\"object\","
                "\"variablesReference\":0}");
        }

        pos += snprintf(body_buf + pos, sizeof(body_buf) - (size_t)pos, "]}");
        send_raw_response(dbg, req_seq, "variables", body_buf);
    } else if (command && strcmp(command, "evaluate") == 0) {
        /* Scrape expression from raw frame to avoid GetPropertyStr hang. */
        const char *raw = dbg->raw_frame ? dbg->raw_frame : "";
        char expr_buf[2048];
        expr_buf[0] = '\0';
        const char *ep = strstr(raw, "\"expression\"");
        if (ep) {
            ep = strstr(ep, ":");
            if (ep) {
                while (*ep && (*ep == ':' || *ep == ' ' || *ep == '\t')) ep++;
                if (*ep == '"') { ep++; }
                const char *eend = strchr(ep, '"');
                if (eend) {
                    size_t len = (size_t)(eend - ep);
                    if (len >= sizeof(expr_buf)) len = sizeof(expr_buf) - 1;
                    memcpy(expr_buf, ep, len);
                    expr_buf[len] = '\0';
                }
            }
        }

        /* Optional frameId -> evaluate within that stack frame's scope.
         * The client passes the raw 0-based frame index (as returned by
         * stackTrace); convert it the same way the variables path does. */
        int frame_idx = -1;
        const char *fp = strstr(raw, "\"frameId\"");
        if (fp) {
            fp = strstr(fp, ":");
            if (fp) {
                while (*fp && (*fp == ':' || *fp == ' ' || *fp == '\t')) fp++;
                long fid = strtol(fp, NULL, 10);
                if (fid >= 0)
                    frame_idx = FRAME_OF(SCOPE_LOCALS((int)fid));
            }
        }

        char result_val[1024] = "\"<no expression>\"";
        char result_type[64] = "\"string\"";

        if (expr_buf[0]) {
            JSValue result;
            if (frame_idx >= 0) {
                /* Evaluate the expression as a function whose parameters are
                   the frame's current variables, so it can read (and the
                   result reflects) the local scope. */
                JSDebugVariable fvars[DBG_MAX_VARS];
                int n = JS_GetFrameVariables(ctx, frame_idx, fvars, DBG_MAX_VARS);
                char src[8192];
                int spos = snprintf(src, sizeof(src), "(function(");
                for (int i = 0; i < n && spos < (int)sizeof(src) - 512; i++) {
                    const char *nm = JS_AtomToCString(ctx, fvars[i].name);
                    if (nm && is_js_identifier(nm)) {
                        if (spos > (int)strlen("(function("))
                            spos += snprintf(src + spos, sizeof(src) - spos, ",");
                        spos += snprintf(src + spos, sizeof(src) - spos, "%s", nm);
                    }
                    JS_FreeCString(ctx, nm);
                }
                spos += snprintf(src + spos, sizeof(src) - spos,
                                 "){ return (%s); })(", expr_buf);
                int first_val = 1;
                for (int i = 0; i < n && spos < (int)sizeof(src) - 512; i++) {
                    const char *nm = JS_AtomToCString(ctx, fvars[i].name);
                    if (nm && is_js_identifier(nm)) {
                        if (!first_val)
                            spos += snprintf(src + spos, sizeof(src) - spos, ",");
                        first_val = 0;
                        char *vstr = jsvalue_to_json(ctx, fvars[i].value);
                        if (vstr) {
                            spos += snprintf(src + spos, sizeof(src) - spos, "%s", vstr);
                            free(vstr);
                        } else {
                            spos += snprintf(src + spos, sizeof(src) - spos, "undefined");
                        }
                    }
                    JS_FreeCString(ctx, nm);
                    JS_FreeValue(ctx, fvars[i].value);
                }
                spos += snprintf(src + spos, sizeof(src) - spos, ")");
                result = JS_Eval(ctx, src, strlen(src), "<debug-eval>", JS_EVAL_TYPE_GLOBAL);
            } else {
                result = JS_Eval(ctx, expr_buf, strlen(expr_buf),
                                 "<debug-eval>", JS_EVAL_TYPE_GLOBAL);
            }
            if (JS_IsException(result)) {
                JSValue exc = JS_GetException(ctx);
                const char *msg = JS_ToCString(ctx, exc);
                char esc[512];
                json_escape(msg ? msg : "exception", esc, sizeof(esc));
                snprintf(result_val, sizeof(result_val), "\"%s\"", esc);
                snprintf(result_type, sizeof(result_type), "\"error\"");
                JS_FreeCString(ctx, msg);
                JS_FreeValue(ctx, exc);
            } else {
                const char *vstr = JS_ToCString(ctx, result);
                char esc[512];
                json_escape(vstr ? vstr : "", esc, sizeof(esc));
                snprintf(result_val, sizeof(result_val), "\"%s\"", esc);
                snprintf(result_type, sizeof(result_type), "\"%s\"",
                         js_val_type_tag(result));
                JS_FreeCString(ctx, vstr);
                JS_FreeValue(ctx, result);
            }
        }

        char body_buf[1024];
        snprintf(body_buf, sizeof(body_buf),
            "{\"result\":%s,\"type\":%s,\"variablesReference\":0}",
            result_val, result_type);
        send_raw_response(dbg, req_seq, "evaluate", body_buf);
    } else if (command && strcmp(command, "continue") == 0) {
        dbg->step_mode = STEP_NONE;
        /* Send response FIRST (client waits for it), then event. */
        send_raw_response(dbg, req_seq, "continue", "{}");
        send_raw_event(dbg, "continued",
            "{\"threadId\":1,\"allThreadsContinued\":true}");
        JS_FreeCString(ctx, command);
        return true;
    } else if (command && strcmp(command, "next") == 0) {
        dbg->step_mode = STEP_OVER;
        record_step_base(dbg);
        JS_DebugSetInterruptCounter(ctx, 1);
        send_raw_response(dbg, req_seq, "next", "{}");
        send_raw_event(dbg, "continued", "{\"threadId\":1}");
        JS_FreeCString(ctx, command);
        return true;
    } else if (command && strcmp(command, "stepIn") == 0) {
        dbg->step_mode = STEP_IN;
        record_step_base(dbg);
        JS_DebugSetInterruptCounter(ctx, 1);
        send_raw_response(dbg, req_seq, "stepIn", "{}");
        send_raw_event(dbg, "continued", "{\"threadId\":1}");
        JS_FreeCString(ctx, command);
        return true;
    } else if (command && strcmp(command, "stepOut") == 0) {
        dbg->step_mode = STEP_OUT;
        dbg->step_base_depth = JS_GetFrameDepth(ctx);
        JS_DebugSetInterruptCounter(ctx, 1);
        send_raw_response(dbg, req_seq, "stepOut", "{}");
        send_raw_event(dbg, "continued", "{\"threadId\":1}");
        JS_FreeCString(ctx, command);
        return true;
    } else if (command && strcmp(command, "pause") == 0) {
        dbg->pause_requested = true;
        JSValue body = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, body, "threadId", JS_NewInt32(ctx, 1));
        send_event(dbg, "continued", body);
        JS_FreeCString(ctx, command);
        return true;
    } else if (command && strcmp(command, "disconnect") == 0) {
        send_response(dbg, req_seq, "disconnect", JS_UNDEFINED);
        dbg->abort = true;
        if (dbg->client_sock >= 0) {
            CLOSE_SOCKET(dbg->client_sock);
            dbg->client_sock = -1;
        }
        JS_FreeCString(ctx, command);
        return true;
    } else {
        /* Unknown request: acknowledge to avoid deadlocks. */
        send_response(dbg, req_seq, command ? command : "unknown", JS_UNDEFINED);
    }

    JS_FreeCString(ctx, command);
    return false;
}

/* Pre-run handshake: accept the client, then consume launch/attach +
 * configurationDone (and any breakpoint requests in between). */
static int dbg_handshake(Debugger *dbg)
{
    JSContext *ctx = dbg->ctx;
    dbg->in_loop = true;
    bool done = false;
    while (!done) {
        int len;
        char *frame = dbg_read_frame(dbg, &len);
        if (!frame) {
            dbg->in_loop = false;
            return -1;
        }
        /* Save raw frame for handlers that need to scrape without
           using QuickJS property APIs on ParseJSON'd objects. */
        free(dbg->raw_frame);
        dbg->raw_frame = frame;
        dbg->raw_frame_len = (size_t)len;

        JSValue req = JS_ParseJSON(ctx, frame, (size_t)len, "<dap>");
        if (JS_IsException(req)) {
            JS_FreeValue(ctx, req);
            continue;
        }
        const char *command = js_str_prop(ctx, req, "command");
        int req_seq = js_int_prop(ctx, req, "seq", 0);

        if (command && strcmp(command, "initialize") == 0) {
            /* Send capabilities as pre-built JSON to avoid JS object creation. */
            send_raw_response(dbg, req_seq, "initialize",
                "{\"supportsConfigurationDoneRequest\":true,"
                "\"supportsEvaluateForHovers\":true,"
                "\"supportsSetVariable\":true,"
                "\"supportsFunctionBreakpoints\":false,"
                "\"exceptionBreakpointFilters\":["
                "{\"filter\":\"uncaught\",\"label\":\"Uncaught Exceptions\"},"
                "{\"filter\":\"all\",\"label\":\"All Exceptions\"}"
                "]}");
        } else if (command && (strcmp(command, "launch") == 0 || strcmp(command, "attach") == 0)) {
            /* Scrape stopOnEntry from raw JSON. */
            bool stopOnEntry = (strcmp(command, "attach") != 0);
            const char *raw = dbg->raw_frame ? dbg->raw_frame : "";
            if (strstr(raw, "\"stopOnEntry\":false"))
                stopOnEntry = false;
            else if (strstr(raw, "\"stopOnEntry\":true"))
                stopOnEntry = true;
            dbg->launch_stop_on_entry = stopOnEntry;
            send_raw_response(dbg, req_seq, command, "{}");
            send_raw_event(dbg, "initialized", "{}");
        } else if (command && strcmp(command, "setBreakpoints") == 0) {
            handle_set_breakpoints(dbg, req, req_seq);
        } else if (command && strcmp(command, "setExceptionBreakpoints") == 0) {
            handle_set_exception_bp(dbg, req, req_seq);
        } else if (command && strcmp(command, "configurationDone") == 0) {
            send_raw_response(dbg, req_seq, "configurationDone", "{}");
            done = true;
        } else if (command && strcmp(command, "disconnect") == 0) {
            send_response(dbg, req_seq, "disconnect", JS_UNDEFINED);
            dbg->abort = true;
            dbg->in_loop = false;
            JS_FreeCString(ctx, command);
            JS_FreeValue(ctx, req);
            return -1;
        } else {
            send_response(dbg, req_seq, command ? command : "unknown", JS_UNDEFINED);
        }

        JS_FreeCString(ctx, command);
        JS_FreeValue(ctx, req);
    }
    dbg->in_loop = false;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                        */
/* ------------------------------------------------------------------ */

JSDebugServer *JS_DebugServerInit(JSRuntime *rt, JSContext *ctx,
                                  const char *bind_host, int port)
{
    Debugger *dbg = calloc(1, sizeof(Debugger));
    if (!dbg)
        return NULL;
    dbg->rt = rt;
    dbg->ctx = ctx;
    dbg->listen_sock = -1;
    dbg->client_sock = -1;
    dbg->seq = 1;
    dbg->step_mode = STEP_NONE;

    dbg_socket_init();
    if (dbg_listen(dbg, bind_host, port) != 0) {
        free(dbg);
        return NULL;
    }

    JS_SetInterruptHandler(rt, debugger_check, dbg);
    JS_SetExceptionHandler(rt, exception_handler, dbg);
    return (JSDebugServer *)dbg;
}

int JS_DebugServerAttach(JSDebugServer *srv)
{
    Debugger *dbg = (Debugger *)srv;
    if (dbg_accept(dbg) != 0)
        return -1;
    return dbg_handshake(dbg);
}

void JS_DebugServerSetRunning(JSDebugServer *srv, bool running)
{
    Debugger *dbg = (Debugger *)srv;
    if (!dbg)
        return;
    dbg->running = running;
}

int JS_DebugServerRun(JSDebugServer *srv,
                      const char *code, size_t code_len,
                      const char *filename, int eval_flags)
{
    Debugger *dbg = (Debugger *)srv;

    if (JS_DebugServerAttach((JSDebugServer *)dbg) != 0)
        return -1;

    dbg->running = true;
    if (dbg->launch_stop_on_entry) {
        dbg->stop_at_entry = true;
        JS_DebugSetInterruptCounter(dbg->ctx, 1);
    }

    JSValue ret = JS_Eval(dbg->ctx, code, code_len, filename, eval_flags);
    dbg->running = false;

    if (JS_IsException(ret) && !dbg->abort) {
        JSValue exc = JS_GetException(dbg->ctx);
        const char *msg = JS_ToCString(dbg->ctx, exc);
        send_output(dbg, msg ? msg : "uncaught exception");
        JS_FreeCString(dbg->ctx, msg);
        JS_FreeValue(dbg->ctx, exc);
    }
    JS_FreeValue(dbg->ctx, ret);

    JSValue tb = JS_NewObject(dbg->ctx);
    JS_SetPropertyStr(dbg->ctx, tb, "restart", JS_NewBool(dbg->ctx, false));
    send_event(dbg, "terminated", tb);
    return 0;
}

void JS_DebugServerFree(JSDebugServer *srv)
{
    Debugger *dbg = (Debugger *)srv;
    if (!dbg)
        return;
    if (dbg->client_sock >= 0) {
        /* Gracefully flush any pending output (e.g. the 'terminated'
           event) to the client before tearing down the socket. */
        shutdown(dbg->client_sock, SD_SEND);
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
        CLOSE_SOCKET(dbg->client_sock);
    }
    if (dbg->listen_sock >= 0)
        CLOSE_SOCKET(dbg->listen_sock);
    for (int i = 0; i < dbg->bp_count; i++) {
        free(dbg->bps[i].filename);
        JS_FreeAtom(dbg->ctx, dbg->bps[i].filename_atom);
    }
    free(dbg->bps);
    free(dbg->rbuf);
    free(dbg->last_file);
    free(dbg);
}
