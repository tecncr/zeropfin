#include "jellyfin.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "urlutil.h"
#include "version.h"

struct zf_jf {
    pthread_mutex_t lock;
    zf_config cfg;
    zf_creds creds;
    bool reachable;
    char last_error[160];
};

struct zf_jf_conn {
    CURL *easy;
};

const char *zf_jf_err_message(zf_jf_err e)
{
    switch (e) {
    case ZF_JF_OK: return "OK";
    case ZF_JF_ERR_NOT_CONFIGURED: return "Jellyfin is not set up on this receiver. Run `zeropfin setup`.";
    case ZF_JF_ERR_NETWORK: return "Cannot reach the Jellyfin server.";
    case ZF_JF_ERR_TLS: return "The Jellyfin server's TLS certificate could not be verified.";
    case ZF_JF_ERR_AUTH: return "Jellyfin rejected the stored access token. Run `zeropfin setup` again.";
    case ZF_JF_ERR_FORBIDDEN: return "This Jellyfin account is not allowed to do that.";
    case ZF_JF_ERR_NOT_FOUND: return "The requested item was not found on the Jellyfin server.";
    case ZF_JF_ERR_BAD_REQUEST: return "Jellyfin rejected the request.";
    case ZF_JF_ERR_SERVER: return "The Jellyfin server reported an internal error.";
    case ZF_JF_ERR_PROTOCOL: return "Unexpected response from the Jellyfin server.";
    case ZF_JF_ERR_TOO_LARGE: return "The Jellyfin response was too large.";
    }
    return "Unknown error";
}

zf_jf *zf_jf_new(const zf_config *cfg, const zf_creds *creds)
{
    zf_jf *j = calloc(1, sizeof *j);
    if (!j)
        return NULL;
    pthread_mutex_init(&j->lock, NULL);
    j->cfg = *cfg;
    if (creds)
        j->creds = *creds;
    return j;
}

void zf_jf_free(zf_jf *j)
{
    if (!j)
        return;
    pthread_mutex_destroy(&j->lock);
    memset(j, 0, sizeof *j);
    free(j);
}

void zf_jf_set_creds(zf_jf *j, const zf_creds *creds)
{
    pthread_mutex_lock(&j->lock);
    j->creds = *creds;
    j->reachable = false;
    j->last_error[0] = '\0';
    pthread_mutex_unlock(&j->lock);
}

bool zf_jf_configured(zf_jf *j)
{
    pthread_mutex_lock(&j->lock);
    bool ok = zf_creds_configured(&j->creds);
    pthread_mutex_unlock(&j->lock);
    return ok;
}

void zf_jf_get_creds(zf_jf *j, zf_creds *out)
{
    pthread_mutex_lock(&j->lock);
    *out = j->creds;
    pthread_mutex_unlock(&j->lock);
}

void zf_jf_get_health(zf_jf *j, bool *reachable, char *err, size_t errcap)
{
    pthread_mutex_lock(&j->lock);
    *reachable = j->reachable;
    zf_strcpy(err, errcap, j->last_error);
    pthread_mutex_unlock(&j->lock);
}

zf_jf_conn *zf_jf_conn_new(void)
{
    zf_jf_conn *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->easy = curl_easy_init();
    if (!c->easy) {
        free(c);
        return NULL;
    }
    return c;
}

void zf_jf_conn_free(zf_jf_conn *c)
{
    if (!c)
        return;
    curl_easy_cleanup(c->easy);
    free(c);
}

int zf_jf_auth_header(const char *device_name, const char *device_id, const char *token, char *out, size_t cap)
{
    /* Values are validated at config/creds load: no quotes, controls or
     * backslashes, so no escaping is needed. */
    if (token && *token)
        return zf_fmt(out, cap,
                      "Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", "
                      "Version=\"%s\", Token=\"%s\"",
                      ZF_JF_CLIENT_NAME, device_name, device_id, ZF_VERSION, token);
    return zf_fmt(out, cap, "Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"",
                  ZF_JF_CLIENT_NAME, device_name, device_id, ZF_VERSION);
}

typedef struct {
    zf_buf *buf;
    char *ctype;
    size_t ctcap;
} xfer;

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *ud)
{
    xfer *x = ud;
    size_t n = size * nmemb;
    if (zf_buf_append(x->buf, ptr, n) < 0)
        return 0; /* abort: over limit */
    return n;
}

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *ud)
{
    xfer *x = ud;
    size_t n = size * nmemb;
    if (x->ctype && n > 13 && !strncasecmp(ptr, "Content-Type:", 13)) {
        size_t s = 13;
        while (s < n && (ptr[s] == ' ' || ptr[s] == '\t'))
            s++;
        size_t e = n;
        while (e > s && (ptr[e - 1] == '\r' || ptr[e - 1] == '\n' || ptr[e - 1] == ' '))
            e--;
        size_t len = e - s < x->ctcap - 1 ? e - s : x->ctcap - 1;
        memcpy(x->ctype, ptr + s, len);
        x->ctype[len] = '\0';
    }
    return n;
}

static zf_jf_err do_request(zf_jf_conn *c, const char *method, const char *url, const char *auth_header,
                            const char *body_text, size_t limit, const char *ca_file, zf_buf *out, char *ctype,
                            size_t ctcap, long timeout_s)
{
    CURL *e = c->easy;
    curl_easy_reset(e);
    zf_buf_init(out, limit);
    if (ctype && ctcap)
        ctype[0] = '\0';
    xfer x = {.buf = out, .ctype = ctype, .ctcap = ctcap};
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_header);
    hdrs = curl_slist_append(hdrs, "Accept: application/json, image/*;q=0.9");
    if (body_text)
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    char ua[96];
    zf_fmt(ua, sizeof ua, "Zeropfin/%s", ZF_VERSION);

    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(e, CURLOPT_USERAGENT, ua);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(e, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    if (ca_file && *ca_file)
        curl_easy_setopt(e, CURLOPT_CAINFO, ca_file);
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, &x);
    if (!strcmp(method, "POST")) {
        curl_easy_setopt(e, CURLOPT_POST, 1L);
        curl_easy_setopt(e, CURLOPT_POSTFIELDS, body_text ? body_text : "");
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)(body_text ? strlen(body_text) : 0));
    }
    CURLcode rc = curl_easy_perform(e);
    long status = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
        if (out->overflow)
            return ZF_JF_ERR_TOO_LARGE;
        if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_SSL_CACERT_BADFILE)
            return ZF_JF_ERR_TLS;
        char redacted[256];
        zf_redact_url(url, redacted, sizeof redacted);
        LOGW("jellyfin %s %s failed: %s", method, redacted, curl_easy_strerror(rc));
        return ZF_JF_ERR_NETWORK;
    }
    if (status >= 200 && status < 300)
        return ZF_JF_OK;
    char redacted[256];
    zf_redact_url(url, redacted, sizeof redacted);
    LOGW("jellyfin %s %s returned HTTP %ld", method, redacted, status);
    if (status == 401)
        return ZF_JF_ERR_AUTH;
    if (status == 403)
        return ZF_JF_ERR_FORBIDDEN;
    if (status == 404)
        return ZF_JF_ERR_NOT_FOUND;
    if (status == 400)
        return ZF_JF_ERR_BAD_REQUEST;
    if (status >= 500)
        return ZF_JF_ERR_SERVER;
    return ZF_JF_ERR_PROTOCOL; /* includes redirects, which are never followed */
}

static void note_health(zf_jf *j, zf_jf_err e)
{
    pthread_mutex_lock(&j->lock);
    if (e == ZF_JF_OK || e == ZF_JF_ERR_NOT_FOUND || e == ZF_JF_ERR_BAD_REQUEST || e == ZF_JF_ERR_FORBIDDEN) {
        j->reachable = true;
        j->last_error[0] = '\0';
    } else {
        j->reachable = e == ZF_JF_ERR_AUTH || e == ZF_JF_ERR_SERVER;
        zf_strcpy(j->last_error, sizeof j->last_error, zf_jf_err_message(e));
    }
    pthread_mutex_unlock(&j->lock);
}

zf_jf_err zf_jf_request(zf_jf *j, zf_jf_conn *c, const char *method, const char *pathq, const cJSON *body, size_t limit,
                        zf_buf *out, char *ctype, size_t ctcap)
{
    zf_creds cr;
    zf_jf_get_creds(j, &cr);
    zf_buf_init(out, limit);
    if (!zf_creds_configured(&cr)) {
        memset(&cr, 0, sizeof cr);
        return ZF_JF_ERR_NOT_CONFIGURED;
    }
    char url[2048];
    char auth[512];
    if (pathq[0] != '/' || zf_fmt(url, sizeof url, "%s%s", cr.server_url, pathq) < 0 ||
        zf_jf_auth_header(j->cfg.device_name, cr.device_id, cr.access_token, auth, sizeof auth) < 0) {
        memset(&cr, 0, sizeof cr);
        return ZF_JF_ERR_PROTOCOL;
    }
    memset(&cr, 0, sizeof cr);
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    zf_jf_err e = do_request(c, method, url, auth, text, limit, j->cfg.jellyfin_ca_file, out, ctype, ctcap,
                             strcmp(method, "POST") ? 20L : 30L);
    memset(auth, 0, sizeof auth);
    free(text);
    note_health(j, e);
    return e;
}

static zf_jf_err parse_json(zf_buf *b, cJSON **out)
{
    if (!b->data) {
        *out = NULL;
        return ZF_JF_ERR_PROTOCOL;
    }
    *out = cJSON_ParseWithLength(b->data, b->len);
    return *out ? ZF_JF_OK : ZF_JF_ERR_PROTOCOL;
}

static void jitter_sleep(int base_ms)
{
    uint8_t r = 0;
    zf_random_bytes(&r, 1);
    int ms = base_ms + (r % (base_ms + 1));
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

zf_jf_err zf_jf_get_json(zf_jf *j, zf_jf_conn *c, const char *pathq, cJSON **out)
{
    *out = NULL;
    zf_buf b;
    zf_jf_err e = zf_jf_request(j, c, "GET", pathq, NULL, ZF_JF_JSON_LIMIT, &b, NULL, 0);
    if (e == ZF_JF_ERR_NETWORK || e == ZF_JF_ERR_SERVER) {
        zf_buf_free(&b);
        jitter_sleep(250);
        e = zf_jf_request(j, c, "GET", pathq, NULL, ZF_JF_JSON_LIMIT, &b, NULL, 0);
    }
    if (e == ZF_JF_OK)
        e = parse_json(&b, out);
    zf_buf_free(&b);
    return e;
}

zf_jf_err zf_jf_post_json(zf_jf *j, zf_jf_conn *c, const char *pathq, const cJSON *body, cJSON **out)
{
    if (out)
        *out = NULL;
    zf_buf b;
    zf_jf_err e = zf_jf_request(j, c, "POST", pathq, body, ZF_JF_JSON_LIMIT, &b, NULL, 0);
    if (e == ZF_JF_OK && out)
        e = parse_json(&b, out);
    zf_buf_free(&b);
    return e;
}

zf_jf_err zf_jf_public_info(zf_jf_conn *c, const char *server_url, const char *ca_file, cJSON **out)
{
    *out = NULL;
    char url[512];
    if (zf_fmt(url, sizeof url, "%s/System/Info/Public", server_url) < 0)
        return ZF_JF_ERR_PROTOCOL;
    char auth[256];
    zf_jf_auth_header("setup", "zeropfin-setup", NULL, auth, sizeof auth);
    zf_buf b;
    zf_jf_err e = do_request(c, "GET", url, auth, NULL, 64 * 1024, ca_file, &b, NULL, 0, 15L);
    if (e == ZF_JF_OK)
        e = parse_json(&b, out);
    zf_buf_free(&b);
    return e;
}

zf_jf_err zf_jf_authenticate(zf_jf_conn *c, const char *server_url, const char *ca_file, const char *device_name,
                             const char *device_id, const char *user, const char *password, cJSON **out)
{
    *out = NULL;
    char url[512];
    if (zf_fmt(url, sizeof url, "%s/Users/AuthenticateByName", server_url) < 0)
        return ZF_JF_ERR_PROTOCOL;
    char auth[512];
    if (zf_jf_auth_header(device_name, device_id, NULL, auth, sizeof auth) < 0)
        return ZF_JF_ERR_PROTOCOL;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "Username", user);
    cJSON_AddStringToObject(body, "Pw", password);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body); /* note: cJSON does not wipe; the copy below is wiped */
    if (!text)
        return ZF_JF_ERR_PROTOCOL;
    zf_buf b;
    zf_jf_err e = do_request(c, "POST", url, auth, text, 256 * 1024, ca_file, &b, NULL, 0, 30L);
    memset(text, 0, strlen(text));
    free(text);
    if (e == ZF_JF_OK)
        e = parse_json(&b, out);
    if (b.data)
        memset(b.data, 0, b.len);
    zf_buf_free(&b);
    return e;
}

int zf_jf_version_number(const char *v)
{
    if (!v)
        return 0;
    int a = 0, b = 0, c = 0;
    if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2)
        return 0;
    if (a < 0 || b < 0 || c < 0 || a > 999 || b > 99 || c > 99)
        return 0;
    return a * 10000 + b * 100 + c;
}

/* ---- item mapping --------------------------------------------------------------- */

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static void copy_str(cJSON *dst, const char *dk, const cJSON *src, const char *sk, size_t maxlen)
{
    const char *s = jstr(src, sk);
    if (!s)
        return;
    if (strlen(s) <= maxlen) {
        cJSON_AddStringToObject(dst, dk, s);
    } else {
        char *t = strndup(s, maxlen);
        if (t) {
            cJSON_AddStringToObject(dst, dk, t);
            free(t);
        }
    }
}

static void copy_num(cJSON *dst, const char *dk, const cJSON *src, const char *sk)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, sk);
    if (cJSON_IsNumber(v))
        cJSON_AddNumberToObject(dst, dk, v->valuedouble);
}

static cJSON *image_ref(const char *id, const char *type, const char *tag)
{
    char nid[33];
    if (!id || !tag || !zf_jf_id_normalize(id, nid, sizeof nid) || strlen(tag) > 64 || zf_has_ctl(tag))
        return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", nid);
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddStringToObject(o, "tag", tag);
    return o;
}

static const char *first_tag(const cJSON *arr)
{
    const cJSON *t = cJSON_GetArrayItem(arr, 0);
    return cJSON_IsString(t) ? t->valuestring : NULL;
}

cJSON *zf_jf_item_summary(const cJSON *it)
{
    const char *id = jstr(it, "Id");
    char nid[33];
    if (!id || !zf_jf_id_normalize(id, nid, sizeof nid))
        return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", nid);
    copy_str(o, "name", it, "Name", 300);
    copy_str(o, "type", it, "Type", 32);
    copy_str(o, "collectionType", it, "CollectionType", 32);
    copy_num(o, "year", it, "ProductionYear");
    copy_str(o, "seriesName", it, "SeriesName", 300);
    const char *sid = jstr(it, "SeriesId");
    char nsid[33];
    if (sid && zf_jf_id_normalize(sid, nsid, sizeof nsid))
        cJSON_AddStringToObject(o, "seriesId", nsid);
    const char *seasonid = jstr(it, "SeasonId");
    if (seasonid && zf_jf_id_normalize(seasonid, nsid, sizeof nsid))
        cJSON_AddStringToObject(o, "seasonId", nsid);
    copy_num(o, "index", it, "IndexNumber");
    copy_num(o, "parentIndex", it, "ParentIndexNumber");
    copy_num(o, "childCount", it, "ChildCount");
    const cJSON *rt = cJSON_GetObjectItemCaseSensitive(it, "RunTimeTicks");
    if (cJSON_IsNumber(rt) && rt->valuedouble > 0)
        cJSON_AddNumberToObject(o, "runtimeMs", (double)((int64_t)rt->valuedouble / ZF_TICKS_PER_MS));
    cJSON_AddBoolToObject(o, "isFolder", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "IsFolder")));

    const cJSON *ud = cJSON_GetObjectItemCaseSensitive(it, "UserData");
    if (cJSON_IsObject(ud)) {
        cJSON_AddBoolToObject(o, "played", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ud, "Played")));
        const cJSON *pp = cJSON_GetObjectItemCaseSensitive(ud, "PlayedPercentage");
        if (cJSON_IsNumber(pp))
            cJSON_AddNumberToObject(o, "playedPct", (double)(int)(pp->valuedouble * 10) / 10.0);
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(ud, "PlaybackPositionTicks");
        if (cJSON_IsNumber(pos) && pos->valuedouble > 0)
            cJSON_AddNumberToObject(o, "resumeMs", (double)((int64_t)pos->valuedouble / ZF_TICKS_PER_MS));
        copy_num(o, "unplayed", ud, "UnplayedItemCount");
    }

    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(it, "ImageTags");
    const char *primary = jstr(tags, "Primary");
    const char *thumb = jstr(tags, "Thumb");
    const char *type = jstr(it, "Type");
    bool episode = type && !strcmp(type, "Episode");

    cJSON *poster = NULL;
    if (episode && jstr(it, "SeriesPrimaryImageTag"))
        poster = image_ref(sid, "Primary", jstr(it, "SeriesPrimaryImageTag"));
    if (!poster && primary)
        poster = image_ref(id, "Primary", primary);
    if (poster)
        cJSON_AddItemToObject(o, "poster", poster);

    cJSON *still = NULL;
    if (episode && primary)
        still = image_ref(id, "Primary", primary);
    else if (thumb)
        still = image_ref(id, "Thumb", thumb);
    else if (jstr(it, "ParentThumbImageTag"))
        still = image_ref(jstr(it, "ParentThumbItemId"), "Thumb", jstr(it, "ParentThumbImageTag"));
    const char *bd = first_tag(cJSON_GetObjectItemCaseSensitive(it, "BackdropImageTags"));
    cJSON *backdrop = bd ? image_ref(id, "Backdrop", bd) : NULL;
    if (!backdrop) {
        const char *pbd = first_tag(cJSON_GetObjectItemCaseSensitive(it, "ParentBackdropImageTags"));
        if (pbd)
            backdrop = image_ref(jstr(it, "ParentBackdropItemId"), "Backdrop", pbd);
    }
    if (!still && backdrop)
        still = cJSON_Duplicate(backdrop, true);
    if (still)
        cJSON_AddItemToObject(o, "thumb", still);
    if (backdrop)
        cJSON_AddItemToObject(o, "backdrop", backdrop);
    return o;
}
