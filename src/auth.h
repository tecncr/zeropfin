/* auth.h — local remote-control authentication: one-time pairing codes and
 * opaque session cookies. Independent of Jellyfin credentials: a paired
 * browser never receives the Jellyfin token.
 *
 * Sessions persist in <state_dir>/sessions.json storing only SHA-256 hashes
 * of the cookie value; written only on pair/logout/revoke (no SD churn). */
#ifndef ZF_AUTH_H
#define ZF_AUTH_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#define ZF_PAIR_CODE_TTL_MS             (10 * 60 * 1000)
#define ZF_PAIR_MAX_ATTEMPTS_PER_CODE   5
#define ZF_PAIR_MAX_FAILURES_PER_MIN    10
#define ZF_PAIR_REQUEST_MIN_INTERVAL_MS 5000
#define ZF_MAX_SESSIONS                 32

typedef struct zf_auth zf_auth;

enum { ZF_AUTH_OK = 0, ZF_AUTH_INVALID = -1, ZF_AUTH_LIMITED = -2, ZF_AUTH_ERROR = -3 };

/* state_dir may be NULL for an in-memory store (tests). */
zf_auth *zf_auth_new(const char *state_dir, int session_days);
void zf_auth_free(zf_auth *a);

/* Create (replacing any previous) one-time code "XXXX-XXXX". `now_ms` is
 * monotonic. Returns ZF_AUTH_LIMITED if requested too frequently and
 * `force` is false (CLI requests use force=true). */
int zf_auth_new_code(zf_auth *a, int64_t now_ms, bool force, char code_out[16]);
/* Exchange a code for a session. token_out: 64 hex chars, csrf_out: 32 hex. */
int zf_auth_pair(zf_auth *a, const char *code, const char *device_name, int64_t now_ms, char token_out[65],
                 char csrf_out[33]);
/* Validate a cookie token. On success copies the session's CSRF token. */
bool zf_auth_check(zf_auth *a, const char *token, char csrf_out[33]);
int zf_auth_logout(zf_auth *a, const char *token);
/* Admin operations (control socket). */
cJSON *zf_auth_list(zf_auth *a);
int zf_auth_revoke(zf_auth *a, const char *id); /* id or "all"; returns count */

/* Normalize user-typed code (case, dashes, spaces) into 8 chars. */
bool zf_pair_code_normalize(const char *in, char out[9]);

#endif
