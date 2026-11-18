#include "api.h"

#include <ctype.h>
#include <math.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat.h"
#include "netfilter.h"
#include "urlutil.h"
#include "version.h"

#define LIST_LIMIT_MAX    60
#define EPISODE_LIMIT_MAX 100
#define IMG_TYPES         "&enableImageTypes=Primary,Backdrop,Thumb&imageTypeLimit=1&enableUserData=true"

/* ---- small helpers -------------------------------------------------------------- */

static void err_from_jf(zf_req *r, zf_jf_err e)
{
    switch (e) {
    case ZF_JF_ERR_NOT_CONFIGURED: zf_req_set_error(r, 503, "not_configured", zf_jf_err_message(e), false); break;
    case ZF_JF_ERR_NETWORK:
    case ZF_JF_ERR_TLS:
    case ZF_JF_ERR_SERVER: zf_req_set_error(r, 502, "jellyfin_unreachable", zf_jf_err_message(e), true); break;
    case ZF_JF_ERR_AUTH: zf_req_set_error(r, 502, "jellyfin_auth", zf_jf_err_message(e), false); break;
    case ZF_JF_ERR_FORBIDDEN: zf_req_set_error(r, 403, "forbidden", zf_jf_err_message(e), false); break;
    case ZF_JF_ERR_NOT_FOUND: zf_req_set_error(r, 404, "not_found", zf_jf_err_message(e), false); break;
    default: zf_req_set_error(r, 502, "jellyfin_error", zf_jf_err_message(e), true); break;
    }
}

static bool user_id(zf_app *app, char out[64])
{
    zf_creds c;
    zf_jf_get_creds(app->jf, &c);
    bool ok = zf_creds_configured(&c);
    zf_strcpy(out, 64, c.user_id);
    memset(&c, 0, sizeof c);
    return ok;
}

/* Append item to a comma-separated list held in buf. */
static bool append_csv(char *buf, size_t cap, const char *item)
{
    size_t n = strlen(buf), l = strlen(item);
    if (n + (n ? 1 : 0) + l + 1 > cap)
        return false;
    if (n)
        buf[n++] = ',';
    memcpy(buf + n, item, l + 1);
    return true;
}

/* Append "&key=value" (value percent-encoded) to a query string. */
static bool qs_add(char *buf, size_t cap, const char *key, const char *val)
{
    char enc[512];
    if (zf_urlencode(val, enc, sizeof enc) < 0)
        return false;
    size_t n = strlen(buf);
    return zf_fmt(buf + n, cap - n, "&%s=%s", key, enc) == 0;
}

static bool parse_int_param(const char *v, int64_t min, int64_t max, int64_t dflt, int64_t *out)
{
    if (!v || !*v) {
        *out = dflt;
        return true;
    }
    return zf_parse_i64(v, min, max, out) == 0;
}

static bool text_param_ok(const char *v, size_t maxlen)
{
    return v && strlen(v) <= maxlen && !zf_has_ctl(v);
}

/* Match "/prefix/<id>[/rest]" and normalize the id. */
static bool match_id(const char *path, const char *prefix, char id[33], const char **rest)
{
    size_t pl = strlen(prefix);
    if (strncmp(path, prefix, pl) != 0)
        return false;
    const char *p = path + pl;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    char tmp[40];
    if (n == 0 || n >= sizeof tmp)
        return false;
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    if (!zf_jf_id_normalize(tmp, id, 33))
        return false;
    *rest = slash ? slash : "";
    return true;
}

static cJSON *parse_body(zf_req *r)
{
    if (!r->body.len)
        return cJSON_CreateObject();
    cJSON *o = cJSON_ParseWithLength(r->body.data, r->body.len);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

/* ---- player state ---------------------------------------------------------------- */

cJSON *zf_api_player_json(const zf_player_snapshot *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", ps_state_name(s->state));
    cJSON_AddNumberToObject(o, "positionMs", (double)s->position_ms);
    cJSON_AddNumberToObject(o, "durationMs", (double)s->duration_ms);
    cJSON_AddStringToObject(o, "positionSource", ps_source_name(s->source));
    cJSON_AddNumberToObject(o, "generation", s->generation);
    if (s->error[0])
        cJSON_AddStringToObject(o, "error", s->error);
    else
        cJSON_AddNullToObject(o, "error");
    cJSON *pl = cJSON_AddObjectToObject(o, "player");
    cJSON_AddStringToObject(pl, "status", zf_proc_state_name(s->proc));
    cJSON_AddBoolToObject(pl, "statusEvents", s->ext_events);
    cJSON_AddNumberToObject(pl, "restarts", s->restarts);
    if (s->player_error[0])
        cJSON_AddStringToObject(pl, "error", s->player_error);
    if (s->has_media) {
        cJSON *it = cJSON_AddObjectToObject(o, "item");
        cJSON_AddStringToObject(it, "id", s->media.item_id);
        cJSON_AddStringToObject(it, "title", s->media.title);
        cJSON_AddStringToObject(it, "subtitle", s->media.subtitle);
        cJSON_AddStringToObject(it, "type", s->media.kind);
        cJSON_AddNumberToObject(it, "runtimeMs", (double)s->media.runtime_ms);
        if (s->media.image_item_id[0] && s->media.image_tag[0]) {
            cJSON *img = cJSON_AddObjectToObject(it, "poster");
            cJSON_AddStringToObject(img, "id", s->media.image_item_id);
            cJSON_AddStringToObject(img, "type", "Primary");
            cJSON_AddStringToObject(img, "tag", s->media.image_tag);
        }
    } else {
        cJSON_AddNullToObject(o, "item");
    }
    return o;
}

static cJSON *player_json(zf_app *app)
{
    zf_player_snapshot s;
    zf_player_snapshot_get(app->player, &s);
    return zf_api_player_json(&s);
}

/* ---- async jobs -------------------------------------------------------------------- */

typedef enum { J_LIST, J_ARRAY, J_VIEWS, J_DETAIL, J_IMAGE, J_PLAY } job_kind;

typedef struct {
    zf_req *r;
    job_kind kind;
    char pathq[1024];
    char item_id[33];
    bool with_overview;
    bool resume;
    int64_t start_ms;
    bool has_start;
} job;

static void add_overview(cJSON *dst, const cJSON *src, size_t max)
{
    const cJSON *ov = cJSON_GetObjectItemCaseSensitive(src, "Overview");
    if (!cJSON_IsString(ov) || !ov->valuestring)
        return;
    char *t = strndup(ov->valuestring, max);
    if (t) {
        cJSON_AddStringToObject(dst, "overview", t);
        free(t);
    }
}

static cJSON *map_items(const cJSON *arr, bool with_overview)
{
    cJSON *out = cJSON_CreateArray();
    const cJSON *it;
    int n = 0;
    cJSON_ArrayForEach(it, arr)
    {
        if (n++ >= EPISODE_LIMIT_MAX)
            break;
        cJSON *s = zf_jf_item_summary(it);
        if (!s)
            continue;
        if (with_overview)
            add_overview(s, it, 400);
        cJSON_AddItemToArray(out, s);
    }
    return out;
}

static bool is_playable_type(const char *t)
{
    return t && (!strcmp(t, "Movie") || !strcmp(t, "Episode") || !strcmp(t, "Video") || !strcmp(t, "MusicVideo"));
}

static void build_subtitle(const cJSON *item, char *out, size_t cap)
{
    const cJSON *series = cJSON_GetObjectItemCaseSensitive(item, "SeriesName");
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(item, "IndexNumber");
    const cJSON *pidx = cJSON_GetObjectItemCaseSensitive(item, "ParentIndexNumber");
    const cJSON *year = cJSON_GetObjectItemCaseSensitive(item, "ProductionYear");
    if (cJSON_IsString(series)) {
        if (cJSON_IsNumber(idx) && cJSON_IsNumber(pidx))
            snprintf(out, cap, "%s · S%dE%d", series->valuestring, (int)pidx->valuedouble, (int)idx->valuedouble);
        else
            snprintf(out, cap, "%s", series->valuestring);
    } else if (cJSON_IsNumber(year)) {
        snprintf(out, cap, "%d", (int)year->valuedouble);
    } else {
        out[0] = '\0';
    }
}

static void do_play(job *j, zf_jf_conn *conn)
{
    zf_req *r = j->r;
    zf_app *app = r->app;
    char uid[64];
    if (!user_id(app, uid)) {
        err_from_jf(r, ZF_JF_ERR_NOT_CONFIGURED);
        return;
    }
    char path[512];
    cJSON *item = NULL;
    zf_fmt(path, sizeof path, "/Items/%s?userId=%s", j->item_id, uid);
    zf_jf_err e = zf_jf_get_json(app->jf, conn, path, &item);
    if (e != ZF_JF_OK) {
        err_from_jf(r, e);
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "Type");
    if (!is_playable_type(cJSON_IsString(type) ? type->valuestring : NULL)) {
        cJSON_Delete(item);
        zf_req_set_error(r, 422, "not_playable", "Only movies, episodes and videos can be played.", false);
        return;
    }

    int64_t start_ms = 0;
    if (j->has_start) {
        start_ms = j->start_ms;
    } else if (j->resume) {
        const cJSON *ud = cJSON_GetObjectItemCaseSensitive(item, "UserData");
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(ud, "PlaybackPositionTicks");
        if (cJSON_IsNumber(pos) && pos->valuedouble > 0)
            zf_ticks_to_ms((int64_t)pos->valuedouble, &start_ms);
    }
    int64_t start_ticks = 0;
    zf_ms_to_ticks(start_ms, &start_ticks);

    /* Negotiate: direct play only, our honest capability profile. */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "UserId", uid);
    cJSON_AddNumberToObject(body, "StartTimeTicks", (double)start_ticks);
    cJSON_AddNumberToObject(body, "MaxStreamingBitrate", (double)app->cfg.max_bitrate);
    cJSON_AddBoolToObject(body, "EnableDirectPlay", true);
    cJSON_AddBoolToObject(body, "EnableDirectStream", false);
    cJSON_AddBoolToObject(body, "EnableTranscoding", false);
    cJSON_AddBoolToObject(body, "AutoOpenLiveStream", false);
    cJSON *prof = cJSON_AddObjectToObject(body, "DeviceProfile");
    cJSON_AddStringToObject(prof, "Name", "Zeropfin");
    cJSON_AddNumberToObject(prof, "MaxStreamingBitrate", (double)app->cfg.max_bitrate);
    cJSON_AddNumberToObject(prof, "MaxStaticBitrate", (double)app->cfg.max_bitrate);
    cJSON *dpp = cJSON_AddArrayToObject(prof, "DirectPlayProfiles");
    cJSON *dp = cJSON_CreateObject();
    char containers[256] = "", audio[256] = "";
    for (int i = 0; i < app->cfg.n_containers; i++)
        append_csv(containers, sizeof containers, app->cfg.containers[i]);
    for (int i = 0; i < app->cfg.n_audio_codecs; i++)
        append_csv(audio, sizeof audio, app->cfg.audio_codecs[i]);
    cJSON_AddStringToObject(dp, "Type", "Video");
    cJSON_AddStringToObject(dp, "Container", containers);
    cJSON_AddStringToObject(dp, "VideoCodec", "h264");
    cJSON_AddStringToObject(dp, "AudioCodec", audio);
    cJSON_AddItemToArray(dpp, dp);
    cJSON_AddArrayToObject(prof, "TranscodingProfiles");
    cJSON *subs = cJSON_AddArrayToObject(prof, "SubtitleProfiles");
    cJSON *sp = cJSON_CreateObject();
    cJSON_AddStringToObject(sp, "Format", "srt");
    cJSON_AddStringToObject(sp, "Method", "Embed");
    cJSON_AddItemToArray(subs, sp);

    cJSON *pinfo = NULL;
    zf_fmt(path, sizeof path, "/Items/%s/PlaybackInfo?userId=%s", j->item_id, uid);
    e = zf_jf_post_json(app->jf, conn, path, body, &pinfo);
    cJSON_Delete(body);
    if (e != ZF_JF_OK) {
        cJSON_Delete(item);
        err_from_jf(r, e);
        return;
    }
    const cJSON *ecode = cJSON_GetObjectItemCaseSensitive(pinfo, "ErrorCode");
    if (cJSON_IsString(ecode) && !strcmp(ecode->valuestring, "NotAllowed")) {
        cJSON_Delete(item);
        cJSON_Delete(pinfo);
        zf_req_set_error(r, 403, "not_allowed", "Jellyfin does not allow this account to play this item.", false);
        return;
    }
    zf_compat c;
    int idx = zf_compat_choose(cJSON_GetObjectItemCaseSensitive(pinfo, "MediaSources"), &app->cfg, &c);
    if (idx < 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON *er = cJSON_AddObjectToObject(o, "error");
        cJSON_AddStringToObject(er, "code", "incompatible");
        cJSON_AddStringToObject(er, "message", c.n_reasons ? c.reasons[0] : "This media cannot be played here.");
        cJSON_AddBoolToObject(er, "retryable", false);
        cJSON_AddItemToObject(er, "playback", zf_compat_to_json(&c));
        cJSON_Delete(item);
        cJSON_Delete(pinfo);
        zf_req_set_json(r, 422, o);
        return;
    }
    const cJSON *psid = cJSON_GetObjectItemCaseSensitive(pinfo, "PlaySessionId");

    zf_media m;
    memset(&m, 0, sizeof m);
    zf_strcpy(m.item_id, sizeof m.item_id, j->item_id);
    zf_strcpy(m.media_source_id, sizeof m.media_source_id, c.media_source_id);
    if (cJSON_IsString(psid) && zf_token_syntax_ok(psid->valuestring))
        zf_strcpy(m.play_session_id, sizeof m.play_session_id, psid->valuestring);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "Name");
    zf_strcpy(m.title, sizeof m.title, cJSON_IsString(name) ? name->valuestring : "Untitled");
    build_subtitle(item, m.subtitle, sizeof m.subtitle);
    zf_strcpy(m.kind, sizeof m.kind, type->valuestring);
    const cJSON *rt = cJSON_GetObjectItemCaseSensitive(item, "RunTimeTicks");
    if (cJSON_IsNumber(rt) && rt->valuedouble > 0)
        zf_ticks_to_ms((int64_t)rt->valuedouble, &m.runtime_ms);
    if (m.runtime_ms > 0 && start_ms >= m.runtime_ms - 5000)
        start_ms = 0; /* resuming at the very end makes no sense: restart */
    m.start_ms = start_ms;
    m.audio_stream_index = c.audio_index;
    m.subtitle_stream_index = c.subtitle_index;
    cJSON *summary = zf_jf_item_summary(item);
    const cJSON *poster = cJSON_GetObjectItemCaseSensitive(summary, "poster");
    if (poster) {
        zf_strcpy(m.image_item_id, sizeof m.image_item_id, cJSON_GetObjectItemCaseSensitive(poster, "id")->valuestring);
        zf_strcpy(m.image_tag, sizeof m.image_tag, cJSON_GetObjectItemCaseSensitive(poster, "tag")->valuestring);
    }
    cJSON_Delete(summary);
    cJSON_Delete(item);

    char upstream[1024];
    zf_creds cr;
    zf_jf_get_creds(app->jf, &cr);
    zf_fmt(upstream, sizeof upstream, "/Videos/%s/stream?static=true", j->item_id);
    bool ok = qs_add(upstream, sizeof upstream, "mediaSourceId", m.media_source_id) &&
              qs_add(upstream, sizeof upstream, "deviceId", cr.device_id) &&
              (!m.play_session_id[0] || qs_add(upstream, sizeof upstream, "playSessionId", m.play_session_id));
    memset(&cr, 0, sizeof cr);
    cJSON_Delete(pinfo);
    if (!ok || zf_relay_grant(app->relay, upstream, 24LL * 3600 * 1000, m.relay_id, m.url, sizeof m.url) < 0) {
        zf_req_set_error(r, 500, "relay", "Could not prepare the media relay.", true);
        return;
    }
    int rc = zf_player_load(app->player, &m);
    if (rc < 0) {
        zf_relay_revoke(app->relay, m.relay_id);
        zf_req_set_error(r, 503, "player_busy", "The player is busy. Try again.", true);
        return;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "playback", zf_compat_to_json(&c));
    cJSON_AddNumberToObject(resp, "startMs", (double)start_ms);
    zf_req_set_json(r, 202, resp);
}

static void do_detail(job *j, zf_jf_conn *conn)
{
    zf_req *r = j->r;
    cJSON *item = NULL;
    zf_jf_err e = zf_jf_get_json(r->app->jf, conn, j->pathq, &item);
    if (e != ZF_JF_OK) {
        err_from_jf(r, e);
        return;
    }
    cJSON *o = zf_jf_item_summary(item);
    if (!o) {
        cJSON_Delete(item);
        zf_req_set_error(r, 502, "jellyfin_error", "Malformed item.", true);
        return;
    }
    add_overview(o, item, 2000);
    const cJSON *rating = cJSON_GetObjectItemCaseSensitive(item, "OfficialRating");
    if (cJSON_IsString(rating) && strlen(rating->valuestring) < 32)
        cJSON_AddStringToObject(o, "officialRating", rating->valuestring);
    const cJSON *cr = cJSON_GetObjectItemCaseSensitive(item, "CommunityRating");
    if (cJSON_IsNumber(cr))
        cJSON_AddNumberToObject(o, "communityRating", round(cr->valuedouble * 10) / 10);
    const cJSON *genres = cJSON_GetObjectItemCaseSensitive(item, "Genres");
    cJSON *g = cJSON_AddArrayToObject(o, "genres");
    const cJSON *gi;
    int n = 0;
    cJSON_ArrayForEach(gi, genres)
    {
        if (n++ >= 5)
            break;
        if (cJSON_IsString(gi) && strlen(gi->valuestring) < 64)
            cJSON_AddItemToArray(g, cJSON_CreateString(gi->valuestring));
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "Type");
    if (is_playable_type(cJSON_IsString(type) ? type->valuestring : NULL)) {
        zf_compat c;
        zf_compat_choose(cJSON_GetObjectItemCaseSensitive(item, "MediaSources"), &r->app->cfg, &c);
        cJSON_AddItemToObject(o, "playback", zf_compat_to_json(&c));
    }
    cJSON_Delete(item);
    zf_req_set_json(r, 200, o);
}

static void do_image(job *j, zf_jf_conn *conn)
{
    zf_req *r = j->r;
    zf_buf b;
    char ctype[128];
    zf_jf_err e = zf_jf_request(r->app->jf, conn, "GET", j->pathq, NULL, ZF_JF_IMAGE_LIMIT, &b, ctype, sizeof ctype);
    if (e != ZF_JF_OK || strncasecmp(ctype, "image/", 6) != 0 || strchr(ctype, '\n') || !b.data) {
        zf_buf_free(&b);
        if (e == ZF_JF_OK)
            e = ZF_JF_ERR_PROTOCOL;
        err_from_jf(r, e);
        return;
    }
    /* Only well-known raster types; never pass through SVG or HTML. */
    const char *ct = !strncasecmp(ctype, "image/jpeg", 10)   ? "image/jpeg"
                     : !strncasecmp(ctype, "image/png", 9)   ? "image/png"
                     : !strncasecmp(ctype, "image/webp", 10) ? "image/webp"
                     : !strncasecmp(ctype, "image/gif", 9)   ? "image/gif"
                                                             : NULL;
    if (!ct) {
        zf_buf_free(&b);
        zf_req_set_error(r, 502, "jellyfin_error", "Unsupported image type.", false);
        return;
    }
    zf_req_set_bytes(r, 200, b.data, b.len, ct, "private, max-age=604800, immutable");
    b.data = NULL;
}

static void job_main(void *arg, zf_jf_conn *conn)
{
    job *j = arg;
    zf_req *r = j->r;
    if (!conn) {
        zf_req_set_error(r, 503, "shutting_down", "The receiver is shutting down.", true);
    } else if (j->kind == J_PLAY) {
        do_play(j, conn);
    } else if (j->kind == J_DETAIL) {
        do_detail(j, conn);
    } else if (j->kind == J_IMAGE) {
        do_image(j, conn);
    } else {
        cJSON *resp = NULL;
        zf_jf_err e = zf_jf_get_json(r->app->jf, conn, j->pathq, &resp);
        if (e != ZF_JF_OK) {
            err_from_jf(r, e);
        } else {
            cJSON *o = cJSON_CreateObject();
            const cJSON *items = j->kind == J_ARRAY ? resp : cJSON_GetObjectItemCaseSensitive(resp, "Items");
            if (j->kind == J_VIEWS) {
                cJSON *arr = cJSON_AddArrayToObject(o, "items");
                const cJSON *v;
                cJSON_ArrayForEach(v, items)
                {
                    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(v, "CollectionType");
                    const char *t = cJSON_IsString(ct) ? ct->valuestring : "";
                    /* Video libraries only: this receiver plays video. */
                    if (*t && strcmp(t, "movies") && strcmp(t, "tvshows") && strcmp(t, "homevideos") &&
                        strcmp(t, "musicvideos") && strcmp(t, "boxsets") && strcmp(t, "mixed"))
                        continue;
                    cJSON *s = zf_jf_item_summary(v);
                    if (s)
                        cJSON_AddItemToArray(arr, s);
                }
            } else {
                cJSON_AddItemToObject(o, "items", map_items(items, j->with_overview));
                const cJSON *total = cJSON_GetObjectItemCaseSensitive(resp, "TotalRecordCount");
                const cJSON *start = cJSON_GetObjectItemCaseSensitive(resp, "StartIndex");
                if (cJSON_IsNumber(total))
                    cJSON_AddNumberToObject(o, "total", total->valuedouble);
                cJSON_AddNumberToObject(o, "start", cJSON_IsNumber(start) ? start->valuedouble : 0);
            }
            zf_req_set_json(r, 200, o);
        }
        cJSON_Delete(resp);
    }
    free(j);
    zf_req_complete_async(r);
}

static void go(zf_req *r, job *j)
{
    j->r = r;
    if (zf_req_go_async(r, job_main, j) < 0)
        free(j);
}

static job *new_job(job_kind k)
{
    job *j = calloc(1, sizeof *j);
    if (j)
        j->kind = k;
    return j;
}

/* ---- route handlers --------------------------------------------------------------- */

static void route_items(zf_req *r, const char *uid)
{
    int64_t start, limit;
    if (!parse_int_param(zf_req_query(r, "start"), 0, 1000000, 0, &start) ||
        !parse_int_param(zf_req_query(r, "limit"), 1, LIST_LIMIT_MAX, 30, &limit)) {
        zf_req_set_error(r, 400, "bad_request", "Invalid paging parameters.", false);
        return;
    }
    const char *parent = zf_req_query(r, "parentId");
    const char *search = zf_req_query(r, "search");
    const char *types = zf_req_query(r, "types");
    const char *sort = zf_req_query(r, "sort");
    const char *filter = zf_req_query(r, "filter");
    job *j = new_job(J_LIST);
    if (!j) {
        zf_req_set_error(r, 503, "busy", "Out of memory.", true);
        return;
    }
    zf_fmt(j->pathq, sizeof j->pathq,
           "/Items?userId=%s&recursive=true&startIndex=%lld&limit=%lld" IMG_TYPES "&enableTotalRecordCount=true", uid,
           (long long)start, (long long)limit);
    char pid[33];
    if (parent && *parent) {
        if (!zf_jf_id_normalize(parent, pid, sizeof pid))
            goto bad;
        qs_add(j->pathq, sizeof j->pathq, "parentId", pid);
    }
    if (search && *search) {
        if (!text_param_ok(search, 100))
            goto bad;
        qs_add(j->pathq, sizeof j->pathq, "searchTerm", search);
    }
    /* Item types: strict allowlist. */
    char tbuf[96] = "";
    if (types && *types) {
        char tmp[96];
        if (zf_strcpy(tmp, sizeof tmp, types) < 0)
            goto bad;
        char *save = NULL;
        for (char *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            if (strcmp(t, "Movie") && strcmp(t, "Series") && strcmp(t, "Episode") && strcmp(t, "Video") &&
                strcmp(t, "MusicVideo") && strcmp(t, "BoxSet"))
                goto bad;
            if (!append_csv(tbuf, sizeof tbuf, t))
                goto bad;
        }
    } else {
        zf_strcpy(tbuf, sizeof tbuf, "Movie,Series,Video,MusicVideo");
    }
    qs_add(j->pathq, sizeof j->pathq, "includeItemTypes", tbuf);
    const char *sort_by = "SortName", *order = "Ascending";
    if (sort && *sort) {
        if (!strcmp(sort, "name")) {
        } else if (!strcmp(sort, "added")) {
            sort_by = "DateCreated,SortName";
            order = "Descending";
        } else if (!strcmp(sort, "released")) {
            sort_by = "PremiereDate,SortName";
            order = "Descending";
        } else if (!strcmp(sort, "rating")) {
            sort_by = "CommunityRating,SortName";
            order = "Descending";
        } else {
            goto bad;
        }
    }
    qs_add(j->pathq, sizeof j->pathq, "sortBy", sort_by);
    qs_add(j->pathq, sizeof j->pathq, "sortOrder", order);
    if (filter && *filter) {
        if (strcmp(filter, "unplayed"))
            goto bad;
        qs_add(j->pathq, sizeof j->pathq, "filters", "IsUnplayed");
    }
    go(r, j);
    return;
bad:
    free(j);
    zf_req_set_error(r, 400, "bad_request", "Invalid query parameters.", false);
}

static void route_get(zf_req *r)
{
    zf_app *app = r->app;
    const char *p = r->path + 7; /* after "/api/v1" */
    if (!strcmp(p, "/status")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "version", ZF_VERSION);
        cJSON *jf = cJSON_AddObjectToObject(o, "jellyfin");
        zf_creds c;
        zf_jf_get_creds(app->jf, &c);
        bool configured = zf_creds_configured(&c);
        cJSON_AddBoolToObject(jf, "configured", configured);
        if (configured) {
            cJSON_AddStringToObject(jf, "serverName", c.server_name);
            cJSON_AddStringToObject(jf, "serverVersion", c.server_version);
            cJSON_AddStringToObject(jf, "userName", c.user_name);
        }
        memset(&c, 0, sizeof c);
        bool reachable;
        char err[160];
        zf_jf_get_health(app->jf, &reachable, err, sizeof err);
        cJSON_AddBoolToObject(jf, "reachable", reachable);
        if (err[0])
            cJSON_AddStringToObject(jf, "error", err);
        cJSON_AddItemToObject(o, "player", player_json(app));
        zf_req_set_json(r, 200, o);
        return;
    }
    if (!strcmp(p, "/player")) {
        zf_req_set_json(r, 200, player_json(app));
        return;
    }

    char uid[64];
    if (!user_id(app, uid)) {
        err_from_jf(r, ZF_JF_ERR_NOT_CONFIGURED);
        return;
    }
    char id[33];
    const char *rest;
    job *j = NULL;
    if (!strcmp(p, "/views")) {
        j = new_job(J_VIEWS);
        if (j)
            zf_fmt(j->pathq, sizeof j->pathq, "/UserViews?userId=%s", uid);
    } else if (!strcmp(p, "/items")) {
        route_items(r, uid);
        return;
    } else if (!strcmp(p, "/resume")) {
        j = new_job(J_LIST);
        if (j)
            zf_fmt(j->pathq, sizeof j->pathq, "/UserItems/Resume?userId=%s&limit=16&mediaTypes=Video" IMG_TYPES, uid);
    } else if (!strcmp(p, "/nextup")) {
        j = new_job(J_LIST);
        if (j)
            zf_fmt(j->pathq, sizeof j->pathq, "/Shows/NextUp?userId=%s&limit=16" IMG_TYPES, uid);
    } else if (!strcmp(p, "/latest")) {
        const char *parent = zf_req_query(r, "parentId");
        char pid[33];
        if (parent && *parent && !zf_jf_id_normalize(parent, pid, sizeof pid)) {
            zf_req_set_error(r, 400, "bad_request", "Invalid parentId.", false);
            return;
        }
        j = new_job(J_ARRAY);
        if (j) {
            zf_fmt(j->pathq, sizeof j->pathq, "/Items/Latest?userId=%s&limit=16" IMG_TYPES, uid);
            if (parent && *parent)
                qs_add(j->pathq, sizeof j->pathq, "parentId", pid);
        }
    } else if (match_id(p, "/items/", id, &rest)) {
        if (!*rest) {
            j = new_job(J_DETAIL);
            if (j)
                zf_fmt(j->pathq, sizeof j->pathq, "/Items/%s?userId=%s", id, uid);
        } else if (!strcmp(rest, "/seasons")) {
            j = new_job(J_LIST);
            if (j)
                zf_fmt(j->pathq, sizeof j->pathq, "/Shows/%s/Seasons?userId=%s" IMG_TYPES, id, uid);
        } else if (!strcmp(rest, "/episodes")) {
            int64_t start, limit;
            const char *season = zf_req_query(r, "seasonId");
            char sid[33];
            if (!parse_int_param(zf_req_query(r, "start"), 0, 100000, 0, &start) ||
                !parse_int_param(zf_req_query(r, "limit"), 1, EPISODE_LIMIT_MAX, 50, &limit) ||
                (season && *season && !zf_jf_id_normalize(season, sid, sizeof sid))) {
                zf_req_set_error(r, 400, "bad_request", "Invalid parameters.", false);
                return;
            }
            j = new_job(J_LIST);
            if (j) {
                zf_fmt(j->pathq, sizeof j->pathq,
                       "/Shows/%s/Episodes?userId=%s&startIndex=%lld&limit=%lld&fields=Overview" IMG_TYPES, id, uid,
                       (long long)start, (long long)limit);
                if (season && *season)
                    qs_add(j->pathq, sizeof j->pathq, "seasonId", sid);
                j->with_overview = true;
            }
        } else {
            zf_req_set_error(r, 404, "not_found", "Unknown endpoint.", false);
            return;
        }
    } else if (match_id(p, "/images/", id, &rest)) {
        const char *type = rest[0] == '/' ? rest + 1 : "";
        if (strcmp(type, "Primary") && strcmp(type, "Backdrop") && strcmp(type, "Thumb")) {
            zf_req_set_error(r, 404, "not_found", "Unknown image type.", false);
            return;
        }
        int64_t w;
        const char *tag = zf_req_query(r, "tag");
        if (!parse_int_param(zf_req_query(r, "w"), 40, 1920, 400, &w) ||
            (tag && (!text_param_ok(tag, 64) || !zf_token_syntax_ok(tag)))) {
            zf_req_set_error(r, 400, "bad_request", "Invalid image parameters.", false);
            return;
        }
        j = new_job(J_IMAGE);
        if (j) {
            zf_fmt(j->pathq, sizeof j->pathq, "/Items/%s/Images/%s?maxWidth=%lld&quality=82", id, type, (long long)w);
            if (tag)
                qs_add(j->pathq, sizeof j->pathq, "tag", tag);
        }
    } else {
        zf_req_set_error(r, 404, "not_found", "Unknown endpoint.", false);
        return;
    }
    if (!j) {
        zf_req_set_error(r, 503, "busy", "Out of memory.", true);
        return;
    }
    go(r, j);
}

static bool get_int_field(const cJSON *o, const char *k, int64_t min, int64_t max, int64_t *out, bool *present)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    *present = v != NULL && !cJSON_IsNull(v);
    if (!*present)
        return true;
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble) || v->valuedouble < (double)min ||
        v->valuedouble > (double)max)
        return false;
    *out = (int64_t)v->valuedouble;
    return true;
}

static void route_player_post(zf_req *r, const char *action, cJSON *body)
{
    zf_app *app = r->app;
    int rc = 0;
    if (!strcmp(action, "play")) {
        const cJSON *iid = cJSON_GetObjectItemCaseSensitive(body, "itemId");
        const cJSON *resume = cJSON_GetObjectItemCaseSensitive(body, "resume");
        int64_t start = 0;
        bool has_start = false;
        char id[33];
        if (!cJSON_IsString(iid) || !zf_jf_id_normalize(iid->valuestring, id, sizeof id) ||
            (resume && !cJSON_IsBool(resume)) ||
            !get_int_field(body, "startMs", 0, 7LL * 24 * 3600 * 1000, &start, &has_start)) {
            zf_req_set_error(r, 400, "bad_request",
                             "Expected {\"itemId\": string, \"resume\"?: bool, "
                             "\"startMs\"?: integer}.",
                             false);
            return;
        }
        job *j = new_job(J_PLAY);
        if (!j) {
            zf_req_set_error(r, 503, "busy", "Out of memory.", true);
            return;
        }
        zf_strcpy(j->item_id, sizeof j->item_id, id);
        j->resume = !resume || cJSON_IsTrue(resume);
        j->start_ms = start;
        j->has_start = has_start;
        go(r, j);
        return;
    }
    if (!strcmp(action, "pause"))
        rc = zf_player_pause(app->player);
    else if (!strcmp(action, "resume"))
        rc = zf_player_resume(app->player);
    else if (!strcmp(action, "stop"))
        rc = zf_player_stop(app->player);
    else if (!strcmp(action, "seek")) {
        int64_t pos = 0, delta = 0;
        bool has_pos, has_delta;
        if (!get_int_field(body, "positionMs", 0, 7LL * 24 * 3600 * 1000, &pos, &has_pos) ||
            !get_int_field(body, "deltaMs", -86400000LL, 86400000LL, &delta, &has_delta) || has_pos == has_delta) {
            zf_req_set_error(r, 400, "bad_request", "Expected exactly one of positionMs or deltaMs (integers).", false);
            return;
        }
        rc = has_pos ? zf_player_seek(app->player, pos) : zf_player_seek_relative(app->player, delta);
    } else {
        zf_req_set_error(r, 404, "not_found", "Unknown player action.", false);
        return;
    }
    if (rc == -1) {
        zf_req_set_error(r, 409, "invalid_state", "That control is not available in the current playback state.",
                         false);
        return;
    }
    if (rc == -2) {
        zf_req_set_error(r, 503, "player_busy", "The player is busy. Try again.", true);
        return;
    }
    zf_req_set_json(r, 202, player_json(app));
}

/* ---- entry point ---------------------------------------------------------------- */

static bool post_preconditions(zf_req *r)
{
    const char *ct = zf_req_header(r, "Content-Type");
    if (!ct || strncasecmp(ct, "application/json", 16) != 0) {
        zf_req_set_error(r, 415, "unsupported_media_type", "Content-Type must be application/json.", false);
        return false;
    }
    if (!zf_origin_allowed(zf_req_header(r, "Origin"), zf_req_header(r, "Host"))) {
        zf_req_set_error(r, 403, "bad_origin", "Cross-origin requests are not allowed.", false);
        return false;
    }
    return true;
}

static void set_session_cookie(zf_req *r, const char *token, int max_age)
{
    /* Secure only when a TLS-terminating proxy on this host says so. */
    const char *xfp = zf_req_header(r, "X-Forwarded-Proto");
    const union MHD_ConnectionInfo *ci = MHD_get_connection_info(r->conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    bool secure = false;
    if (xfp && !strcasecmp(xfp, "https") && ci && ci->client_addr) {
        const struct sockaddr *sa = ci->client_addr;
        secure = zf_cidr_match(sa, "127.0.0.0/8") == 1 || zf_cidr_match(sa, "::1/128") == 1;
    }
    zf_fmt(r->set_cookie, sizeof r->set_cookie, "zf_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d%s", token,
           max_age, secure ? "; Secure" : "");
}

void zf_api_handle(zf_req *r)
{
    zf_app *app = r->app;
    bool post = !strcmp(r->method, "POST");
    if (strncmp(r->path, "/api/v1/", 8) != 0) {
        zf_req_set_error(r, 404, "not_found", "Unknown API version.", false);
        return;
    }
    const char *tok = MHD_lookup_connection_value(r->conn, MHD_COOKIE_KIND, "zf_session");
    if (tok && strlen(tok) == 64 && zf_auth_check(app->auth, tok, r->csrf)) {
        r->authed = true;
        zf_strcpy(r->session_token, sizeof r->session_token, tok);
    }
    const char *p = r->path + 7;

    /* ---- public endpoints ---- */
    if (!post && !strcmp(p, "/session")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "authenticated", r->authed);
        if (r->authed)
            cJSON_AddStringToObject(o, "csrfToken", r->csrf);
        cJSON_AddBoolToObject(o, "pairingOnDisplay", app->cfg.pairing_show_on_display);
        zf_req_set_json(r, 200, o);
        return;
    }
    if (post && (!strcmp(p, "/pairing/request") || !strcmp(p, "/pairing/complete"))) {
        if (!post_preconditions(r))
            return;
        cJSON *body = parse_body(r);
        if (!body) {
            zf_req_set_error(r, 400, "bad_request", "Malformed JSON body.", false);
            return;
        }
        int64_t now = zf_mono_ms();
        if (!strcmp(p, "/pairing/request")) {
            char code[16];
            int rc = zf_auth_new_code(app->auth, now, false, code);
            cJSON_Delete(body);
            if (rc == ZF_AUTH_LIMITED) {
                zf_req_set_error(r, 429, "rate_limited", "A code was requested moments ago. Wait a few seconds.", true);
                return;
            }
            if (rc != ZF_AUTH_OK) {
                zf_req_set_error(r, 500, "internal", "Could not create a pairing code.", true);
                return;
            }
            LOGI("pairing code requested from the web UI: %s (valid for 10 minutes)", code);
            bool shown = false;
            if (app->cfg.pairing_show_on_display) {
                char text[96];
                zf_fmt(text, sizeof text, "Zeropfin pairing code: %s", code);
                shown = zf_player_osd(app->player, text, 120000) == 0;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "displayRequested", shown);
            cJSON_AddNumberToObject(o, "expiresInSec", ZF_PAIR_CODE_TTL_MS / 1000);
            zf_req_set_json(r, 200, o);
            return;
        }
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(body, "code");
        const cJSON *dname = cJSON_GetObjectItemCaseSensitive(body, "deviceName");
        char name[64] = "Remote";
        if (cJSON_IsString(dname) && dname->valuestring[0] && !zf_has_ctl(dname->valuestring))
            zf_strcpy(name, sizeof name, dname->valuestring);
        char token[65], csrf[33];
        int rc =
            cJSON_IsString(code) ? zf_auth_pair(app->auth, code->valuestring, name, now, token, csrf) : ZF_AUTH_INVALID;
        cJSON_Delete(body);
        if (rc == ZF_AUTH_LIMITED) {
            zf_req_set_error(r, 429, "rate_limited", "Too many attempts. Wait a minute and request a new code.", true);
            return;
        }
        if (rc != ZF_AUTH_OK) {
            zf_req_set_error(r, 401, "invalid_code", "That code is invalid or has expired.", false);
            return;
        }
        LOGI("paired a new remote ('%s')", name);
        set_session_cookie(r, token, app->cfg.session_days * 86400);
        memset(token, 0, sizeof token);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "csrfToken", csrf);
        zf_req_set_json(r, 200, o);
        return;
    }

    /* ---- everything else requires a paired session ---- */
    if (!r->authed) {
        zf_req_set_error(r, 401, "unauthenticated", "Pair this device first.", false);
        return;
    }
    if (!post) {
        route_get(r);
        return;
    }
    if (!post_preconditions(r))
        return;
    const char *csrf = zf_req_header(r, "X-CSRF-Token");
    if (!csrf || strlen(csrf) != 32 || !zf_ct_equal(csrf, r->csrf, 32)) {
        zf_req_set_error(r, 403, "csrf", "Missing or invalid CSRF token. Reload the page.", false);
        return;
    }
    cJSON *body = parse_body(r);
    if (!body) {
        zf_req_set_error(r, 400, "bad_request", "Malformed JSON body.", false);
        return;
    }
    if (!strcmp(p, "/session/logout")) {
        zf_auth_logout(app->auth, r->session_token);
        set_session_cookie(r, "", 0);
        zf_req_set_json(r, 200, cJSON_CreateObject());
    } else if (!strncmp(p, "/player/", 8)) {
        route_player_post(r, p + 8, body);
    } else {
        zf_req_set_error(r, 404, "not_found", "Unknown endpoint.", false);
    }
    cJSON_Delete(body);
}
