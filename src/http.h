/* http.h — LAN remote-control HTTP server (libmicrohttpd, one event thread).
 *
 * Responsibilities: client address filter, Host/Origin checks, bounded
 * request bodies, static file serving with safe paths, and the plumbing for
 * asynchronous API handlers (suspend → worker → resume). Routes live in api.c. */
#ifndef ZF_HTTP_H
#define ZF_HTTP_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "util.h"
#include "workers.h"

#define ZF_HTTP_MAX_BODY (16u * 1024)

struct MHD_Connection;

typedef enum { ZF_ASYNC_NONE, ZF_ASYNC_PENDING, ZF_ASYNC_DONE } zf_async_state;

typedef struct zf_req {
    zf_app *app;
    struct MHD_Connection *conn;
    const char *method;
    char path[512];
    zf_buf body;
    bool body_too_large;
    bool authed;
    char csrf[33];
    char session_token[65];
    zf_async_state async;

    /* prepared response */
    unsigned resp_status;
    char *resp_body;
    size_t resp_len;
    char resp_ctype[64];
    char resp_cache[96];
    char set_cookie[256];
} zf_req;

typedef struct zf_http zf_http;

zf_http *zf_http_start(zf_app *app);
void zf_http_quiesce(zf_http *h);
void zf_http_stop(zf_http *h);
uint16_t zf_http_port(zf_http *h);

/* Response helpers (callable from the event thread or from a worker job
 * before zf_req_complete_async). */
void zf_req_set_json(zf_req *r, unsigned status, cJSON *obj); /* takes ownership */
void zf_req_set_error(zf_req *r, unsigned status, const char *code, const char *message, bool retryable);
void zf_req_set_bytes(zf_req *r, unsigned status, char *data, size_t len, const char *ctype, const char *cache);

/* Suspend the connection and run fn(arg, conn) on a worker. Returns 0, or
 * -1 after setting a 503 response when the queue is full. */
int zf_req_go_async(zf_req *r, zf_job_fn fn, void *arg);
void zf_req_complete_async(zf_req *r);

const char *zf_req_header(zf_req *r, const char *name);
const char *zf_req_query(zf_req *r, const char *name);

/* MIME type for a static path (exposed for tests). */
const char *zf_mime_for(const char *path);
/* Static path validation (exposed for tests): only [A-Za-z0-9._-/], no
 * dot-segments or hidden files, must start with '/'. */
bool zf_static_path_ok(const char *path);

#endif
