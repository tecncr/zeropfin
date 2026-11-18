#include "relay.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "netfilter.h"
#include "urlutil.h"
#include "util.h"
#include "version.h"

#define HEADER_WAIT_MS 20000
#define STALL_MS       60000
#define UNPAUSE_FREE   (64u * 1024)

typedef struct {
    bool used;
    char id[33];
    char pathq[1024];
    int64_t expires;
    int readers;
    uint32_t serial;
} grant;

struct zf_relay {
    zf_jf *jf;
    char device_name[64];
    char ca_file[256];
    struct MHD_Daemon *d;
    uint16_t port;
    pthread_mutex_t lock;
    grant g[ZF_RELAY_MAX_GRANTS];
    uint32_t serial;
    unsigned readers;
    unsigned long long bytes_out;
    unsigned resumes;
};

typedef struct {
    zf_relay *r;
    int gi;
    uint32_t serial;
    char server_url[256];
    char auth[512];
    char ca_file[256];
    char url[2048];
    bool head;

    CURLM *multi;
    CURL *easy;
    struct curl_slist *hdrs;
    bool attached;

    uint8_t *ring;
    size_t rhead, rlen;
    bool paused, done, headers_done;
    CURLcode result;
    long status;
    char ctype[128];
    char crange[128];
    char location[1024];
    int64_t clen;
    bool accept_ranges;
    int64_t last_data;

    int64_t abs_next;     /* absolute file offset of the next byte to hand out */
    int64_t expected_end; /* inclusive absolute end, -1 = unknown */
    int resumes;
} upstream;

/* ---- range helpers ------------------------------------------------------------ */

static int parse_num(const char **p, int64_t *out)
{
    if (!isdigit((unsigned char)**p))
        return -1;
    int64_t v = 0;
    while (isdigit((unsigned char)**p)) {
        int d = **p - '0';
        if (v > (INT64_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
        (*p)++;
    }
    *out = v;
    return 0;
}

int zf_range_parse(const char *hdr, int64_t *start, int64_t *end)
{
    if (!hdr || strncasecmp(hdr, "bytes=", 6) != 0)
        return -1;
    const char *p = hdr + 6;
    while (*p == ' ')
        p++;
    if (strchr(p, ','))
        return -1; /* multi-range: unsupported, ignore */
    if (*p == '-') {
        p++;
        int64_t n;
        if (parse_num(&p, &n) < 0 || n == 0)
            return -1;
        while (*p == ' ')
            p++;
        if (*p)
            return -1;
        *start = -1;
        *end = n;
        return 0;
    }
    int64_t a, b = -1;
    if (parse_num(&p, &a) < 0 || *p != '-')
        return -1;
    p++;
    if (*p && *p != ' ') {
        if (parse_num(&p, &b) < 0 || b < a)
            return -1;
    }
    while (*p == ' ')
        p++;
    if (*p)
        return -1;
    *start = a;
    *end = b;
    return 0;
}

int zf_content_range_parse(const char *hdr, int64_t *a, int64_t *b, int64_t *total)
{
    if (!hdr || strncasecmp(hdr, "bytes ", 6) != 0)
        return -1;
    const char *p = hdr + 6;
    if (*p == '*') {
        p++;
        *a = *b = -1;
    } else {
        if (parse_num(&p, a) < 0 || *p != '-')
            return -1;
        p++;
        if (parse_num(&p, b) < 0 || *b < *a)
            return -1;
    }
    if (*p != '/')
        return -1;
    p++;
    if (*p == '*') {
        p++;
        *total = -1;
    } else if (parse_num(&p, total) < 0) {
        return -1;
    }
    while (*p == ' ' || *p == '\r' || *p == '\n')
        p++;
    return *p ? -1 : 0;
}

/* ---- grants --------------------------------------------------------------------- */

zf_relay *zf_relay_new(zf_jf *jf, const zf_config *cfg)
{
    zf_relay *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->jf = jf;
    zf_strcpy(r->device_name, sizeof r->device_name, cfg->device_name);
    zf_strcpy(r->ca_file, sizeof r->ca_file, cfg->jellyfin_ca_file);
    pthread_mutex_init(&r->lock, NULL);
    return r;
}

int zf_relay_grant(zf_relay *r, const char *pathq, int64_t ttl_ms, char id_out[33], char *url_out, size_t url_cap)
{
    if (!pathq || pathq[0] != '/' || zf_has_ctl(pathq) || strlen(pathq) >= sizeof r->g[0].pathq)
        return -1;
    char id[33];
    if (zf_random_hex(id, sizeof id, 16) < 0)
        return -1;
    pthread_mutex_lock(&r->lock);
    int64_t now = zf_mono_ms();
    int slot = -1;
    for (int i = 0; i < ZF_RELAY_MAX_GRANTS; i++) {
        if (r->g[i].used && r->g[i].expires <= now && r->g[i].readers == 0)
            r->g[i].used = false;
        if (!r->g[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        /* Evict the oldest idle grant. */
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < ZF_RELAY_MAX_GRANTS; i++)
            if (r->g[i].readers == 0 && r->g[i].serial < oldest) {
                oldest = r->g[i].serial;
                slot = i;
            }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&r->lock);
        return -1;
    }
    grant *g = &r->g[slot];
    memset(g, 0, sizeof *g);
    g->used = true;
    zf_strcpy(g->id, sizeof g->id, id);
    zf_strcpy(g->pathq, sizeof g->pathq, pathq);
    g->expires = now + ttl_ms;
    g->serial = ++r->serial;
    uint16_t port = r->port;
    pthread_mutex_unlock(&r->lock);
    zf_strcpy(id_out, 33, id);
    return zf_fmt(url_out, url_cap, "http://127.0.0.1:%u/media/%s", (unsigned)port, id);
}

void zf_relay_revoke(zf_relay *r, const char *id)
{
    if (!r || !id || !*id)
        return;
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < ZF_RELAY_MAX_GRANTS; i++)
        if (r->g[i].used && zf_ct_equal(r->g[i].id, id, 32) && strlen(id) == 32) {
            r->g[i].used = false; /* readers notice via serial mismatch */
            r->g[i].serial = 0;
        }
    pthread_mutex_unlock(&r->lock);
}

static bool grant_alive(upstream *u)
{
    pthread_mutex_lock(&u->r->lock);
    grant *g = &u->r->g[u->gi];
    bool ok = g->used && g->serial == u->serial;
    pthread_mutex_unlock(&u->r->lock);
    return ok;
}

void zf_relay_stats(zf_relay *r, unsigned *active, unsigned long long *bytes, unsigned *resumes)
{
    pthread_mutex_lock(&r->lock);
    *active = r->readers;
    *bytes = r->bytes_out;
    *resumes = r->resumes;
    pthread_mutex_unlock(&r->lock);
}

/* ---- upstream transfer -------------------------------------------------------------- */

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *ud)
{
    upstream *u = ud;
    size_t n = size * nmemb;
    char line[1100];
    size_t l = n < sizeof line - 1 ? n : sizeof line - 1;
    memcpy(line, ptr, l);
    line[l] = '\0';
    while (l && (line[l - 1] == '\r' || line[l - 1] == '\n'))
        line[--l] = '\0';
    if (!strncmp(line, "HTTP/", 5)) {
        const char *sp = strchr(line, ' ');
        u->status = sp ? strtol(sp + 1, NULL, 10) : 0;
        u->headers_done = false;
        u->ctype[0] = u->crange[0] = u->location[0] = '\0';
        u->clen = -1;
        u->accept_ranges = false;
        return n;
    }
    if (l == 0) {
        if (u->status >= 200)
            u->headers_done = true;
        return n;
    }
    char *colon = strchr(line, ':');
    if (!colon)
        return n;
    *colon = '\0';
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    if (!strcasecmp(line, "Content-Type"))
        zf_strcpy(u->ctype, sizeof u->ctype, v);
    else if (!strcasecmp(line, "Content-Range"))
        zf_strcpy(u->crange, sizeof u->crange, v);
    else if (!strcasecmp(line, "Location"))
        zf_strcpy(u->location, sizeof u->location, v);
    else if (!strcasecmp(line, "Accept-Ranges"))
        u->accept_ranges = strcasestr(v, "bytes") != NULL;
    else if (!strcasecmp(line, "Content-Length")) {
        int64_t cl;
        if (zf_parse_i64(v, 0, INT64_MAX, &cl) == 0)
            u->clen = cl;
    }
    return n;
}

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *ud)
{
    upstream *u = ud;
    size_t n = size * nmemb;
    if (u->status != 200 && u->status != 206)
        return n; /* discard redirect/error bodies */
    if (ZF_RELAY_RING - u->rlen < n) {
        u->paused = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    size_t tail = (u->rhead + u->rlen) % ZF_RELAY_RING;
    size_t first = n < ZF_RELAY_RING - tail ? n : ZF_RELAY_RING - tail;
    memcpy(u->ring + tail, ptr, first);
    memcpy(u->ring, ptr + first, n - first);
    u->rlen += n;
    u->last_data = zf_mono_ms();
    return n;
}

static void detach(upstream *u)
{
    if (u->attached) {
        curl_multi_remove_handle(u->multi, u->easy);
        u->attached = false;
    }
    if (u->hdrs) {
        for (struct curl_slist *s = u->hdrs; s; s = s->next)
            if (s->data)
                memset(s->data, 0, strlen(s->data));
        curl_slist_free_all(u->hdrs);
        u->hdrs = NULL;
    }
}

static int start_transfer(upstream *u, const char *range)
{
    detach(u);
    curl_easy_reset(u->easy);
    u->status = 0;
    u->headers_done = u->done = u->paused = false;
    u->result = CURLE_OK;
    u->clen = -1;
    u->ctype[0] = u->crange[0] = u->location[0] = '\0';
    u->accept_ranges = false;
    u->last_data = zf_mono_ms();

    u->hdrs = curl_slist_append(NULL, u->auth);
    if (range) {
        char rh[96];
        zf_fmt(rh, sizeof rh, "Range: %s", range);
        u->hdrs = curl_slist_append(u->hdrs, rh);
    }
    char ua[96];
    zf_fmt(ua, sizeof ua, "Zeropfin/%s (relay)", ZF_VERSION);
    CURL *e = u->easy;
    curl_easy_setopt(e, CURLOPT_URL, u->url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, u->hdrs);
    curl_easy_setopt(e, CURLOPT_USERAGENT, ua);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(e, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    if (u->ca_file[0])
        curl_easy_setopt(e, CURLOPT_CAINFO, u->ca_file);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, u);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, u);
    if (u->head)
        curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
    if (curl_multi_add_handle(u->multi, e) != CURLM_OK)
        return -1;
    u->attached = true;
    return 0;
}

static void pump(upstream *u, int wait_ms)
{
    if (u->paused && ZF_RELAY_RING - u->rlen >= UNPAUSE_FREE) {
        u->paused = false;
        curl_easy_pause(u->easy, CURLPAUSE_CONT); /* may deliver data synchronously */
    }
    int running = 0;
    curl_multi_perform(u->multi, &running);
    int left;
    CURLMsg *m;
    while ((m = curl_multi_info_read(u->multi, &left))) {
        if (m->msg == CURLMSG_DONE && m->easy_handle == u->easy) {
            u->done = true;
            u->result = m->data.result;
        }
    }
    if (!u->done && !u->paused && wait_ms > 0)
        curl_multi_poll(u->multi, NULL, 0, wait_ms, NULL);
}

static bool wait_headers(upstream *u)
{
    int64_t deadline = zf_mono_ms() + HEADER_WAIT_MS;
    while (!u->headers_done && !u->done && zf_mono_ms() < deadline)
        pump(u, 200);
    return u->headers_done;
}

/* Issue the request, following same-origin redirects only. Returns upstream status or 0. */
static long open_upstream(upstream *u, const char *range)
{
    zf_url origin;
    if (zf_url_parse(u->server_url, &origin) < 0)
        return 0;
    for (int hop = 0; hop < 4; hop++) {
        if (start_transfer(u, range) < 0 || !wait_headers(u)) {
            if (u->done && u->result != CURLE_OK)
                LOGW("relay upstream failed: %s", curl_easy_strerror(u->result));
            else
                LOGW("relay upstream did not answer in time");
            return 0;
        }
        if (u->status < 300 || u->status >= 400)
            return u->status;
        char next[2048];
        zf_url nu;
        if (!u->location[0] || zf_url_resolve_location(u->url, u->location, next, sizeof next) < 0 ||
            zf_url_parse(next, &nu) < 0 || !zf_url_same_origin(&origin, &nu)) {
            LOGW("relay refused a redirect away from the Jellyfin origin");
            return 0;
        }
        zf_strcpy(u->url, sizeof u->url, next);
    }
    LOGW("relay: too many redirects");
    return 0;
}

/* Re-open the upstream at abs_next after a premature end. */
static bool try_resume(upstream *u)
{
    if (u->resumes >= ZF_RELAY_MAX_RESUMES || u->expected_end < 0 || u->abs_next > u->expected_end)
        return false;
    if (!grant_alive(u))
        return false;
    char range[64];
    zf_fmt(range, sizeof range, "bytes=%lld-%lld", (long long)u->abs_next, (long long)u->expected_end);
    u->resumes++;
    pthread_mutex_lock(&u->r->lock);
    u->r->resumes++;
    pthread_mutex_unlock(&u->r->lock);
    LOGI("relay: resuming upstream at byte %lld (attempt %d)", (long long)u->abs_next, u->resumes);
    u->rhead = u->rlen = 0;
    long st = open_upstream(u, range);
    int64_t a, b, t;
    if (st != 206 || zf_content_range_parse(u->crange, &a, &b, &t) < 0 || a != u->abs_next) {
        LOGW("relay: upstream resume failed (HTTP %ld)", st);
        return false;
    }
    return true;
}

/* ---- MHD glue ---------------------------------------------------------------------- */

static ssize_t reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    upstream *u = cls;
    for (;;) {
        if (!grant_alive(u))
            return MHD_CONTENT_READER_END_WITH_ERROR;
        if (u->rlen) {
            size_t n = u->rlen < max ? u->rlen : max;
            size_t contig = ZF_RELAY_RING - u->rhead;
            if (n > contig)
                n = contig;
            memcpy(buf, u->ring + u->rhead, n);
            u->rhead = (u->rhead + n) % ZF_RELAY_RING;
            u->rlen -= n;
            u->abs_next += (int64_t)n;
            pthread_mutex_lock(&u->r->lock);
            u->r->bytes_out += n;
            pthread_mutex_unlock(&u->r->lock);
            return (ssize_t)n;
        }
        if (u->done) {
            bool complete = u->result == CURLE_OK && (u->expected_end < 0 || u->abs_next > u->expected_end);
            if (complete)
                return MHD_CONTENT_READER_END_OF_STREAM;
            if (try_resume(u))
                continue;
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }
        if (!u->paused && zf_mono_ms() - u->last_data > STALL_MS) {
            LOGW("relay: upstream stalled");
            if (try_resume(u))
                continue;
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }
        pump(u, 500);
    }
}

static void upstream_free(upstream *u)
{
    if (!u)
        return;
    detach(u);
    if (u->easy)
        curl_easy_cleanup(u->easy);
    if (u->multi)
        curl_multi_cleanup(u->multi);
    if (u->gi >= 0) {
        pthread_mutex_lock(&u->r->lock);
        if (u->r->g[u->gi].serial == u->serial && u->r->g[u->gi].readers > 0)
            u->r->g[u->gi].readers--;
        if (u->r->readers)
            u->r->readers--;
        pthread_mutex_unlock(&u->r->lock);
    }
    memset(u->auth, 0, sizeof u->auth);
    free(u->ring);
    free(u);
}

static void reader_free(void *cls)
{
    upstream_free(cls);
}

static enum MHD_Result respond_empty(struct MHD_Connection *c, unsigned status)
{
    struct MHD_Response *r = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(r, "Cache-Control", "no-store");
    enum MHD_Result ret = MHD_queue_response(c, status, r);
    MHD_destroy_response(r);
    return ret;
}

static enum MHD_Result handle(void *cls, struct MHD_Connection *conn, const char *url, const char *method,
                              const char *version, const char *upload_data, size_t *upload_data_size, void **req_cls)
{
    (void)version;
    (void)upload_data;
    zf_relay *r = cls;
    if (*req_cls == NULL) {
        static int marker;
        *req_cls = &marker;
        return MHD_YES;
    }
    if (*upload_data_size) {
        *upload_data_size = 0;
        return MHD_YES;
    }
    const union MHD_ConnectionInfo *ci = MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    const struct sockaddr *sa = ci ? ci->client_addr : NULL;
    if (!sa || sa->sa_family != AF_INET ||
        (ntohl(((const struct sockaddr_in *)(const void *)sa)->sin_addr.s_addr) >> 24) != 127)
        return respond_empty(conn, MHD_HTTP_FORBIDDEN);

    bool head = !strcmp(method, "HEAD");
    if (!head && strcmp(method, "GET"))
        return respond_empty(conn, MHD_HTTP_METHOD_NOT_ALLOWED);
    if (strncmp(url, "/media/", 7) != 0 || strlen(url + 7) != 32)
        return respond_empty(conn, MHD_HTTP_NOT_FOUND);
    const char *id = url + 7;
    for (const char *p = id; *p; p++)
        if (!isxdigit((unsigned char)*p))
            return respond_empty(conn, MHD_HTTP_NOT_FOUND);

    upstream *u = calloc(1, sizeof *u);
    if (!u)
        return respond_empty(conn, MHD_HTTP_SERVICE_UNAVAILABLE);
    u->r = r;
    u->gi = -1;
    u->head = head;
    u->expected_end = -1;

    char pathq[1024] = "";
    pthread_mutex_lock(&r->lock);
    int64_t now = zf_mono_ms();
    for (int i = 0; i < ZF_RELAY_MAX_GRANTS; i++) {
        grant *g = &r->g[i];
        if (g->used && g->expires > now && zf_ct_equal(g->id, id, 32)) {
            if (g->readers >= ZF_RELAY_MAX_READERS_PER_GRANT || r->readers >= ZF_RELAY_MAX_READERS) {
                pthread_mutex_unlock(&r->lock);
                free(u);
                LOGW("relay: reader limit reached");
                return respond_empty(conn, MHD_HTTP_SERVICE_UNAVAILABLE);
            }
            g->readers++;
            r->readers++;
            u->gi = i;
            u->serial = g->serial;
            zf_strcpy(pathq, sizeof pathq, g->pathq);
        }
    }
    pthread_mutex_unlock(&r->lock);
    if (u->gi < 0) {
        free(u);
        return respond_empty(conn, MHD_HTTP_NOT_FOUND);
    }

    zf_creds cr;
    zf_jf_get_creds(r->jf, &cr);
    bool ok = zf_creds_configured(&cr) && zf_strcpy(u->server_url, sizeof u->server_url, cr.server_url) == 0 &&
              zf_fmt(u->url, sizeof u->url, "%s%s", cr.server_url, pathq) == 0;
    if (ok) {
        ok = zf_jf_auth_header(r->device_name, cr.device_id, cr.access_token, u->auth, sizeof u->auth) == 0;
        zf_strcpy(u->ca_file, sizeof u->ca_file, r->ca_file);
    }
    memset(&cr, 0, sizeof cr);
    u->ring = ok ? malloc(ZF_RELAY_RING) : NULL;
    u->multi = u->ring ? curl_multi_init() : NULL;
    u->easy = u->multi ? curl_easy_init() : NULL;
    if (!u->easy) {
        upstream_free(u);
        return respond_empty(conn, MHD_HTTP_SERVICE_UNAVAILABLE);
    }

    const char *range_hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, MHD_HTTP_HEADER_RANGE);
    int64_t rs = 0, re = 0;
    bool use_range = range_hdr && zf_range_parse(range_hdr, &rs, &re) == 0;
    long st = open_upstream(u, use_range ? range_hdr : NULL);

    if (st == 416) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
        if (u->crange[0])
            MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE, u->crange);
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_RANGE_NOT_SATISFIABLE, resp);
        MHD_destroy_response(resp);
        upstream_free(u);
        return ret;
    }
    if (st != 200 && st != 206) {
        if (st)
            LOGW("relay: upstream answered HTTP %ld", st);
        upstream_free(u);
        return respond_empty(conn, MHD_HTTP_BAD_GATEWAY);
    }

    int64_t a = 0, b = -1, total = -1;
    if (st == 206) {
        if (zf_content_range_parse(u->crange, &a, &b, &total) < 0 || a < 0) {
            upstream_free(u);
            return respond_empty(conn, MHD_HTTP_BAD_GATEWAY);
        }
        u->abs_next = a;
        u->expected_end = b;
        u->accept_ranges = true;
    } else {
        u->abs_next = 0;
        u->expected_end = u->clen > 0 ? u->clen - 1 : -1;
    }
    /* Without range support we cannot resume: make that explicit. */
    if (!u->accept_ranges)
        u->resumes = ZF_RELAY_MAX_RESUMES;

    uint64_t size = MHD_SIZE_UNKNOWN;
    if (st == 206)
        size = (uint64_t)(b - a + 1);
    else if (u->clen >= 0)
        size = (uint64_t)u->clen;

    char ctype[128];
    char crange[128];
    zf_strcpy(ctype, sizeof ctype, u->ctype[0] ? u->ctype : "application/octet-stream");
    zf_strcpy(crange, sizeof crange, u->crange);
    bool ranges = u->accept_ranges;
    if (head) {
        /* Nothing to stream for HEAD; release the upstream now and make the
         * reader (should MHD ever call it) end immediately. */
        detach(u);
        u->done = true;
        u->result = CURLE_OK;
        u->expected_end = -1;
    }
    struct MHD_Response *resp = MHD_create_response_from_callback(size, 64 * 1024, reader, u, reader_free);
    if (!resp) {
        upstream_free(u);
        return respond_empty(conn, MHD_HTTP_SERVICE_UNAVAILABLE);
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, ctype);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    if (ranges)
        MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
    if (st == 206)
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE, crange);
    enum MHD_Result ret = MHD_queue_response(conn, (unsigned)st, resp);
    MHD_destroy_response(resp);
    return ret;
}

int zf_relay_start(zf_relay *r, uint16_t port)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    r->d =
        MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG, 0, NULL,
                         NULL, handle, r, MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&sin, MHD_OPTION_CONNECTION_LIMIT,
                         (unsigned)(ZF_RELAY_MAX_READERS + 2), MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)300,
                         MHD_OPTION_EXTERNAL_LOGGER, zf_mhd_logger, NULL, MHD_OPTION_END);
    if (!r->d) {
        LOGE("could not start media relay on 127.0.0.1:%u", (unsigned)port);
        return -1;
    }
    const union MHD_DaemonInfo *di = MHD_get_daemon_info(r->d, MHD_DAEMON_INFO_BIND_PORT);
    pthread_mutex_lock(&r->lock);
    r->port = di ? di->port : port;
    pthread_mutex_unlock(&r->lock);
    LOGI("media relay listening on 127.0.0.1:%u", (unsigned)r->port);
    return 0;
}

uint16_t zf_relay_port(zf_relay *r)
{
    pthread_mutex_lock(&r->lock);
    uint16_t p = r->port;
    pthread_mutex_unlock(&r->lock);
    return p;
}

void zf_relay_stop(zf_relay *r)
{
    if (!r || !r->d)
        return;
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < ZF_RELAY_MAX_GRANTS; i++) {
        r->g[i].used = false;
        r->g[i].serial = 0;
    }
    pthread_mutex_unlock(&r->lock);
    MHD_stop_daemon(r->d);
    r->d = NULL;
}

void zf_relay_free(zf_relay *r)
{
    if (!r)
        return;
    zf_relay_stop(r);
    pthread_mutex_destroy(&r->lock);
    free(r);
}
