#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "netfilter.h"

struct zf_http {
    zf_app *app;
    struct MHD_Daemon *d;
    uint16_t port;
};

static const char CSP[] = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                          "font-src 'self'; connect-src 'self'; manifest-src 'self'; object-src 'none'; "
                          "base-uri 'none'; frame-ancestors 'none'; form-action 'self'";

/* ---- response helpers --------------------------------------------------------- */

static void clear_resp(zf_req *r)
{
    free(r->resp_body);
    r->resp_body = NULL;
    r->resp_len = 0;
    r->resp_ctype[0] = '\0';
    r->resp_cache[0] = '\0';
}

void zf_req_set_bytes(zf_req *r, unsigned status, char *data, size_t len, const char *ctype, const char *cache)
{
    clear_resp(r);
    r->resp_status = status;
    r->resp_body = data;
    r->resp_len = len;
    zf_strcpy(r->resp_ctype, sizeof r->resp_ctype, ctype ? ctype : "application/octet-stream");
    zf_strcpy(r->resp_cache, sizeof r->resp_cache, cache ? cache : "no-store");
}

void zf_req_set_json(zf_req *r, unsigned status, cJSON *obj)
{
    char *text = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (!text) {
        zf_req_set_bytes(r, 500,
                         strdup("{\"error\":{\"code\":\"internal\",\"message\":\"Out of memory\","
                                "\"retryable\":true}}"),
                         0, "application/json; charset=utf-8", "no-store");
        if (r->resp_body)
            r->resp_len = strlen(r->resp_body);
        return;
    }
    zf_req_set_bytes(r, status, text, strlen(text), "application/json; charset=utf-8", "no-store");
}

void zf_req_set_error(zf_req *r, unsigned status, const char *code, const char *message, bool retryable)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *e = cJSON_AddObjectToObject(o, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    cJSON_AddBoolToObject(e, "retryable", retryable);
    zf_req_set_json(r, status, o);
}

const char *zf_req_header(zf_req *r, const char *name)
{
    return MHD_lookup_connection_value(r->conn, MHD_HEADER_KIND, name);
}

const char *zf_req_query(zf_req *r, const char *name)
{
    return MHD_lookup_connection_value(r->conn, MHD_GET_ARGUMENT_KIND, name);
}

/* ---- async plumbing ------------------------------------------------------------- */

int zf_req_go_async(zf_req *r, zf_job_fn fn, void *arg)
{
    r->async = ZF_ASYNC_PENDING;
    MHD_suspend_connection(r->conn);
    if (zf_workers_submit(r->app->workers, fn, arg) < 0) {
        MHD_resume_connection(r->conn);
        r->async = ZF_ASYNC_DONE;
        zf_req_set_error(r, 503, "busy", "The receiver is busy. Try again in a moment.", true);
        return -1;
    }
    return 0;
}

void zf_req_complete_async(zf_req *r)
{
    struct MHD_Connection *c = r->conn;
    r->async = ZF_ASYNC_DONE;
    /* After this call the request may be freed by the event thread. */
    MHD_resume_connection(c);
}

/* ---- static files ---------------------------------------------------------------- */

const char *zf_mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    static const struct {
        const char *ext, *mime;
    } map[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".webmanifest", "application/manifest+json"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},
        {".txt", "text/plain; charset=utf-8"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcasecmp(dot, map[i].ext))
            return map[i].mime;
    return "application/octet-stream";
}

bool zf_static_path_ok(const char *p)
{
    if (!p || p[0] != '/' || strlen(p) > 200)
        return false;
    bool seg_start = true;
    for (const char *c = p + 1; *c; c++) {
        char ch = *c;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' ||
                  ch == '-' || ch == '_' || ch == '/';
        if (!ok)
            return false;
        if (seg_start && (ch == '.' || ch == '/'))
            return false; /* hidden files, dot-segments, empty segments */
        seg_start = ch == '/';
    }
    return true;
}

static void add_common_headers(struct MHD_Response *resp)
{
    MHD_add_response_header(resp, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(resp, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(resp, "X-Frame-Options", "DENY");
    MHD_add_response_header(resp, "Cross-Origin-Opener-Policy", "same-origin");
    MHD_add_response_header(resp, "Cross-Origin-Resource-Policy", "same-origin");
}

static bool accepts_gzip(struct MHD_Connection *c)
{
    const char *ae = MHD_lookup_connection_value(c, MHD_HEADER_KIND, MHD_HTTP_HEADER_ACCEPT_ENCODING);
    return ae && strcasestr(ae, "gzip");
}

static enum MHD_Result queue_simple(struct MHD_Connection *c, unsigned status, const char *text)
{
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(strlen(text), (void *)(uintptr_t)text, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=utf-8");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    add_common_headers(resp);
    enum MHD_Result ret = MHD_queue_response(c, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result serve_static(zf_http *h, struct MHD_Connection *c, const char *path)
{
    if (!strcmp(path, "/"))
        path = "/index.html";
    if (!zf_static_path_ok(path))
        return queue_simple(c, MHD_HTTP_NOT_FOUND, "Not found\n");
    char full[PATH_MAX];
    if (zf_fmt(full, sizeof full, "%s%s", h->app->cfg.web_root, path) < 0)
        return queue_simple(c, MHD_HTTP_NOT_FOUND, "Not found\n");

    bool gz = false;
    int fd = -1;
    if (accepts_gzip(c)) {
        char gzp[PATH_MAX];
        if (zf_fmt(gzp, sizeof gzp, "%s.gz", full) == 0) {
            fd = open(gzp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            gz = fd >= 0;
        }
    }
    if (fd < 0)
        fd = open(full, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        return queue_simple(c, MHD_HTTP_NOT_FOUND, "Not found\n");
    }
    struct MHD_Response *resp = MHD_create_response_from_fd64((uint64_t)st.st_size, fd);
    if (!resp) {
        close(fd);
        return queue_simple(c, MHD_HTTP_INTERNAL_SERVER_ERROR, "Error\n");
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, zf_mime_for(path));
    if (gz)
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_ENCODING, "gzip");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_VARY, "Accept-Encoding");
    bool immutable = !strncmp(path, "/assets/", 8);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL,
                            immutable ? "public, max-age=31536000, immutable" : "no-cache");
    MHD_add_response_header(resp, "Content-Security-Policy", CSP);
    add_common_headers(resp);
    enum MHD_Result ret = MHD_queue_response(c, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---- request lifecycle ------------------------------------------------------------- */

static enum MHD_Result queue_prepared(zf_req *r)
{
    if (!r->resp_status)
        zf_req_set_error(r, 500, "internal", "No response was produced.", true);
    struct MHD_Response *resp;
    if (r->resp_body) {
        resp = MHD_create_response_from_buffer(r->resp_len, r->resp_body, MHD_RESPMEM_MUST_FREE);
        r->resp_body = NULL; /* owned by MHD now */
    } else {
        resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    }
    if (!resp)
        return MHD_NO;
    if (r->resp_ctype[0])
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, r->resp_ctype);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, r->resp_cache[0] ? r->resp_cache : "no-store");
    if (r->set_cookie[0])
        MHD_add_response_header(resp, MHD_HTTP_HEADER_SET_COOKIE, r->set_cookie);
    add_common_headers(resp);
    enum MHD_Result ret = MHD_queue_response(r->conn, r->resp_status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static bool client_allowed(zf_app *app, struct MHD_Connection *c)
{
    if (app->cfg.allow_any_client_address)
        return true;
    const union MHD_ConnectionInfo *ci = MHD_get_connection_info(c, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    const struct sockaddr *sa = ci ? ci->client_addr : NULL;
    if (!sa)
        return false;
    if (zf_addr_is_private(sa))
        return true;
    for (int i = 0; i < app->cfg.n_allowed_networks; i++)
        if (zf_cidr_match(sa, app->cfg.allowed_networks[i]) == 1)
            return true;
    return false;
}

static enum MHD_Result handler(void *cls, struct MHD_Connection *conn, const char *url, const char *method,
                               const char *version, const char *upload_data, size_t *upload_data_size, void **req_cls)
{
    (void)version;
    zf_http *h = cls;
    zf_req *r = *req_cls;
    if (!r) {
        r = calloc(1, sizeof *r);
        if (!r)
            return MHD_NO;
        r->app = h->app;
        r->conn = conn;
        r->method = method;
        zf_buf_init(&r->body, ZF_HTTP_MAX_BODY);
        *req_cls = r;
        return MHD_YES;
    }
    if (*upload_data_size) {
        if (!r->body_too_large && zf_buf_append(&r->body, upload_data, *upload_data_size) < 0)
            r->body_too_large = true;
        *upload_data_size = 0;
        return MHD_YES;
    }
    if (r->async == ZF_ASYNC_DONE)
        return queue_prepared(r);
    if (r->async == ZF_ASYNC_PENDING)
        return MHD_YES; /* spurious wakeup */

    if (!client_allowed(h->app, conn))
        return queue_simple(conn, MHD_HTTP_FORBIDDEN, "Forbidden: this receiver only accepts LAN clients\n");
    const char *host = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, MHD_HTTP_HEADER_HOST);
    if (!zf_host_allowed(host, h->app->hostname, &h->app->cfg))
        return queue_simple(conn, 421, "Misdirected request: unknown Host name (see allowed_hosts)\n");

    bool is_get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    bool is_post = !strcmp(method, "POST");
    if (!is_get && !is_post)
        return queue_simple(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "Method not allowed\n");
    if (zf_strcpy(r->path, sizeof r->path, url) < 0)
        return queue_simple(conn, MHD_HTTP_URI_TOO_LONG, "URI too long\n");

    if (!strcmp(r->path, "/healthz")) {
        zf_req_set_bytes(r, 200, strdup("{\"ok\":true}"), 11, "application/json; charset=utf-8", "no-store");
        return queue_prepared(r);
    }
    if (!strncmp(r->path, "/api/", 5)) {
        if (r->body_too_large) {
            zf_req_set_error(r, 413, "too_large", "Request body is too large.", false);
            return queue_prepared(r);
        }
        zf_api_handle(r);
        if (r->async == ZF_ASYNC_PENDING)
            return MHD_YES;
        return queue_prepared(r);
    }
    if (!is_get)
        return queue_simple(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "Method not allowed\n");
    return serve_static(h, conn, r->path);
}

static void completed(void *cls, struct MHD_Connection *conn, void **req_cls, enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)conn;
    (void)toe;
    zf_req *r = *req_cls;
    if (!r)
        return;
    zf_buf_free(&r->body);
    free(r->resp_body);
    memset(r->session_token, 0, sizeof r->session_token);
    free(r);
    *req_cls = NULL;
}

zf_http *zf_http_start(zf_app *app)
{
    zf_http *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->app = app;
    unsigned flags = MHD_USE_INTERNAL_POLLING_THREAD | MHD_ALLOW_SUSPEND_RESUME | MHD_USE_ITC | MHD_USE_ERROR_LOG;
#ifdef __linux__
    flags |= MHD_USE_EPOLL;
#endif
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    const char *addr = app->cfg.listen_address;
    struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
    if (inet_pton(AF_INET6, addr, &s6->sin6_addr) == 1) {
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(app->cfg.listen_port);
        flags |= MHD_USE_IPv6;
        if (IN6_IS_ADDR_UNSPECIFIED(&s6->sin6_addr))
            flags |= MHD_USE_DUAL_STACK;
    } else if (inet_pton(AF_INET, addr, &s4->sin_addr) == 1) {
        s4->sin_family = AF_INET;
        s4->sin_port = htons(app->cfg.listen_port);
    } else {
        LOGE("listen_address '%s' is not a literal IPv4/IPv6 address", addr);
        free(h);
        return NULL;
    }
    struct MHD_OptionItem opts[] = {
        {MHD_OPTION_SOCK_ADDR, 0, &ss},
        {MHD_OPTION_CONNECTION_LIMIT, 64, NULL},
        {MHD_OPTION_PER_IP_CONNECTION_LIMIT, 16, NULL},
        {MHD_OPTION_CONNECTION_TIMEOUT, 30, NULL},
        {MHD_OPTION_CONNECTION_MEMORY_LIMIT, 32 * 1024, NULL},
        {MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)completed, NULL},
        {MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)zf_mhd_logger, NULL},
        {MHD_OPTION_END, 0, NULL},
    };
    h->d =
        MHD_start_daemon(flags, app->cfg.listen_port, NULL, NULL, handler, h, MHD_OPTION_ARRAY, opts, MHD_OPTION_END);
    if (!h->d && (flags & MHD_USE_DUAL_STACK)) {
        /* IPv6 disabled on this host: fall back to IPv4 any. */
        LOGW("dual-stack listen failed; falling back to 0.0.0.0");
        memset(&ss, 0, sizeof ss);
        s4->sin_family = AF_INET;
        s4->sin_port = htons(app->cfg.listen_port);
        flags &= ~(unsigned)(MHD_USE_IPv6 | MHD_USE_DUAL_STACK);
        h->d = MHD_start_daemon(flags, app->cfg.listen_port, NULL, NULL, handler, h, MHD_OPTION_ARRAY, opts,
                                MHD_OPTION_END);
    }
    if (!h->d) {
        LOGE("could not listen on %s port %u: %s", addr, (unsigned)app->cfg.listen_port, strerror(errno));
        free(h);
        return NULL;
    }
    const union MHD_DaemonInfo *di = MHD_get_daemon_info(h->d, MHD_DAEMON_INFO_BIND_PORT);
    h->port = di ? di->port : app->cfg.listen_port;
    LOGI("remote control listening on %s port %u", addr, (unsigned)h->port);
    return h;
}

uint16_t zf_http_port(zf_http *h)
{
    return h->port;
}

void zf_http_quiesce(zf_http *h)
{
    if (h && h->d) {
        MHD_socket s = MHD_quiesce_daemon(h->d);
        if (s != MHD_INVALID_SOCKET)
            close(s);
    }
}

void zf_http_stop(zf_http *h)
{
    if (!h)
        return;
    if (h->d)
        MHD_stop_daemon(h->d);
    free(h);
}
