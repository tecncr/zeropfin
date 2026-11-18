/* jellyfin.h — minimal Jellyfin REST client (libcurl + cJSON).
 *
 * Authentication uses the documented header
 *   Authorization: MediaBrowser Client="Zeropfin", Device="…", DeviceId="…",
 *                  Version="…", Token="…"
 * (verified against Jellyfin 10.10.0 and 12.4 AuthorizationContext; legacy
 * X-Emby-* headers and api_key query parameters are never used).
 *
 * A zf_jf holds the shared, lock-protected account binding; a zf_jf_conn is
 * a per-thread curl handle (curl easy handles are not thread-safe). Response
 * bodies are bounded; nothing is cached beyond a single request. */
#ifndef ZF_JELLYFIN_H
#define ZF_JELLYFIN_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "creds.h"
#include "util.h"

typedef enum {
    ZF_JF_OK = 0,
    ZF_JF_ERR_NOT_CONFIGURED,
    ZF_JF_ERR_NETWORK,
    ZF_JF_ERR_TLS,
    ZF_JF_ERR_AUTH,
    ZF_JF_ERR_FORBIDDEN,
    ZF_JF_ERR_NOT_FOUND,
    ZF_JF_ERR_BAD_REQUEST,
    ZF_JF_ERR_SERVER,
    ZF_JF_ERR_PROTOCOL,
    ZF_JF_ERR_TOO_LARGE,
} zf_jf_err;

#define ZF_JF_JSON_LIMIT  (4u * 1024 * 1024)
#define ZF_JF_IMAGE_LIMIT (2u * 1024 * 1024)
#define ZF_JF_CLIENT_NAME "Zeropfin"

typedef struct zf_jf zf_jf;
typedef struct zf_jf_conn zf_jf_conn;

zf_jf *zf_jf_new(const zf_config *cfg, const zf_creds *creds);
void zf_jf_free(zf_jf *j);
void zf_jf_set_creds(zf_jf *j, const zf_creds *creds);
bool zf_jf_configured(zf_jf *j);
/* Copy of the binding (callers must wipe access_token after use). */
void zf_jf_get_creds(zf_jf *j, zf_creds *out);
void zf_jf_get_health(zf_jf *j, bool *reachable, char *err, size_t errcap);

zf_jf_conn *zf_jf_conn_new(void);
void zf_jf_conn_free(zf_jf_conn *c);

const char *zf_jf_err_message(zf_jf_err e);

/* Build the MediaBrowser Authorization header value (token optional). */
int zf_jf_auth_header(const char *device_name, const char *device_id, const char *token, char *out, size_t cap);

/* Low-level request against the configured server. `pathq` starts with '/'.
 * body may be NULL. Response body capped at `limit` bytes. */
zf_jf_err zf_jf_request(zf_jf *j, zf_jf_conn *c, const char *method, const char *pathq, const cJSON *body, size_t limit,
                        zf_buf *out, char *ctype, size_t ctcap);
zf_jf_err zf_jf_get_json(zf_jf *j, zf_jf_conn *c, const char *pathq, cJSON **out);
zf_jf_err zf_jf_post_json(zf_jf *j, zf_jf_conn *c, const char *pathq, const cJSON *body, cJSON **out);

/* Setup-time calls against an explicit server URL (no stored binding). */
zf_jf_err zf_jf_public_info(zf_jf_conn *c, const char *server_url, const char *ca_file, cJSON **out);
zf_jf_err zf_jf_authenticate(zf_jf_conn *c, const char *server_url, const char *ca_file, const char *device_name,
                             const char *device_id, const char *user, const char *password, cJSON **out);

/* Parse "10.10.3" / "12.4.0" into a comparable integer (major*10000+minor*100+patch). */
int zf_jf_version_number(const char *v);
#define ZF_JF_MIN_VERSION 101000

/* Map a Jellyfin BaseItemDto into the slim browser representation. */
cJSON *zf_jf_item_summary(const cJSON *item);

#endif
