#include "auth.h"

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"
#include "util.h"

/* Unambiguous alphabet: no 0/O, 1/I/L. 31 symbols, 8 chars ≈ 39.6 bits;
 * combined with 5 attempts/code and 10 failures/min that is ample for a
 * 10-minute one-time code. */
static const char ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

typedef struct {
    char id[13];   /* first 12 hex chars of the hash: public handle */
    char hash[65]; /* SHA-256(token) hex */
    char csrf[33];
    char name[64];
    int64_t created_wall;
    int64_t expires_wall;
} session;

struct zf_auth {
    pthread_mutex_t lock;
    char path[PATH_MAX];
    bool persist;
    int session_days;
    session sessions[ZF_MAX_SESSIONS];
    int n_sessions;

    char code[9];
    bool code_active;
    int64_t code_expires;
    int code_attempts;
    int64_t last_code_request;

    int64_t fail_window_start;
    int fail_count;
};

static void save_locked(zf_auth *a)
{
    if (!a->persist)
        return;
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return;
    for (int i = 0; i < a->n_sessions; i++) {
        session *s = &a->sessions[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s->id);
        cJSON_AddStringToObject(o, "hash", s->hash);
        cJSON_AddStringToObject(o, "csrf", s->csrf);
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddNumberToObject(o, "created", (double)s->created_wall);
        cJSON_AddNumberToObject(o, "expires", (double)s->expires_wall);
        cJSON_AddItemToArray(arr, o);
    }
    char *text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (text) {
        if (zf_write_file_atomic(a->path, text, strlen(text), 0600) < 0)
            LOGE("could not persist sessions");
        free(text);
    }
}

static void load(zf_auth *a)
{
    char *text = zf_read_file(a->path, 256 * 1024, NULL);
    if (!text)
        return;
    cJSON *arr = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        LOGW("ignoring malformed sessions file");
        return;
    }
    int64_t now = zf_wall_ms();
    const cJSON *o;
    cJSON_ArrayForEach(o, arr)
    {
        if (a->n_sessions >= ZF_MAX_SESSIONS)
            break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(o, "hash");
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "csrf");
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "name");
        const cJSON *cr = cJSON_GetObjectItemCaseSensitive(o, "created");
        const cJSON *ex = cJSON_GetObjectItemCaseSensitive(o, "expires");
        if (!cJSON_IsString(id) || !cJSON_IsString(h) || !cJSON_IsString(c) || !cJSON_IsNumber(ex) ||
            strlen(h->valuestring) != 64 || strlen(c->valuestring) != 32 || strlen(id->valuestring) != 12)
            continue;
        if ((int64_t)ex->valuedouble <= now)
            continue;
        session *s = &a->sessions[a->n_sessions++];
        memset(s, 0, sizeof *s);
        zf_strcpy(s->id, sizeof s->id, id->valuestring);
        zf_strcpy(s->hash, sizeof s->hash, h->valuestring);
        zf_strcpy(s->csrf, sizeof s->csrf, c->valuestring);
        if (cJSON_IsString(n) && !zf_has_ctl(n->valuestring))
            zf_strcpy(s->name, sizeof s->name, n->valuestring);
        s->created_wall = cJSON_IsNumber(cr) ? (int64_t)cr->valuedouble : 0;
        s->expires_wall = (int64_t)ex->valuedouble;
    }
    cJSON_Delete(arr);
}

zf_auth *zf_auth_new(const char *state_dir, int session_days)
{
    zf_auth *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    pthread_mutex_init(&a->lock, NULL);
    a->session_days = session_days > 0 ? session_days : 30;
    a->last_code_request = INT64_MIN / 2;
    if (state_dir) {
        if (zf_fmt(a->path, sizeof a->path, "%s/sessions.json", state_dir) == 0) {
            a->persist = true;
            load(a);
        }
    }
    return a;
}

void zf_auth_free(zf_auth *a)
{
    if (!a)
        return;
    pthread_mutex_destroy(&a->lock);
    memset(a, 0, sizeof *a);
    free(a);
}

bool zf_pair_code_normalize(const char *in, char out[9])
{
    size_t o = 0;
    if (!in)
        return false;
    for (const char *p = in; *p; p++) {
        if (*p == '-' || *p == ' ')
            continue;
        char ch = (char)toupper((unsigned char)*p);
        if (!strchr(ALPHABET, ch) || ch == '\0' || o >= 8)
            return false;
        out[o++] = ch;
    }
    out[o] = '\0';
    return o == 8;
}

int zf_auth_new_code(zf_auth *a, int64_t now_ms, bool force, char code_out[16])
{
    pthread_mutex_lock(&a->lock);
    if (!force && now_ms - a->last_code_request < ZF_PAIR_REQUEST_MIN_INTERVAL_MS) {
        pthread_mutex_unlock(&a->lock);
        return ZF_AUTH_LIMITED;
    }
    uint8_t rnd[8];
    if (zf_random_bytes(rnd, sizeof rnd) < 0) {
        pthread_mutex_unlock(&a->lock);
        return ZF_AUTH_ERROR;
    }
    /* Rejection sampling avoids modulo bias. */
    for (int i = 0; i < 8; i++) {
        uint8_t r = rnd[i];
        while (r >= 248) { /* 248 = 31 * 8 */
            if (zf_random_bytes(&r, 1) < 0) {
                pthread_mutex_unlock(&a->lock);
                return ZF_AUTH_ERROR;
            }
        }
        a->code[i] = ALPHABET[r % 31];
    }
    a->code[8] = '\0';
    a->code_active = true;
    a->code_expires = now_ms + ZF_PAIR_CODE_TTL_MS;
    a->code_attempts = 0;
    a->last_code_request = now_ms;
    zf_fmt(code_out, 16, "%.4s-%.4s", a->code, a->code + 4);
    pthread_mutex_unlock(&a->lock);
    return ZF_AUTH_OK;
}

static void record_failure_locked(zf_auth *a, int64_t now_ms)
{
    if (now_ms - a->fail_window_start > 60000) {
        a->fail_window_start = now_ms;
        a->fail_count = 0;
    }
    a->fail_count++;
    if (++a->code_attempts >= ZF_PAIR_MAX_ATTEMPTS_PER_CODE) {
        a->code_active = false; /* burn the code after too many guesses */
        memset(a->code, 0, sizeof a->code);
    }
}

int zf_auth_pair(zf_auth *a, const char *code, const char *device_name, int64_t now_ms, char token_out[65],
                 char csrf_out[33])
{
    char norm[9];
    pthread_mutex_lock(&a->lock);
    if (now_ms - a->fail_window_start <= 60000 && a->fail_count >= ZF_PAIR_MAX_FAILURES_PER_MIN) {
        pthread_mutex_unlock(&a->lock);
        return ZF_AUTH_LIMITED;
    }
    if (!a->code_active || now_ms > a->code_expires || !zf_pair_code_normalize(code, norm) ||
        !zf_ct_equal(norm, a->code, 8)) {
        if (a->code_active && now_ms > a->code_expires)
            a->code_active = false;
        record_failure_locked(a, now_ms);
        pthread_mutex_unlock(&a->lock);
        return ZF_AUTH_INVALID;
    }
    /* One-time use. */
    a->code_active = false;
    memset(a->code, 0, sizeof a->code);

    uint8_t raw[32];
    if (zf_random_bytes(raw, sizeof raw) < 0 || zf_random_hex(csrf_out, 33, 16) < 0) {
        pthread_mutex_unlock(&a->lock);
        return ZF_AUTH_ERROR;
    }
    zf_hex_encode(raw, sizeof raw, token_out);
    memset(raw, 0, sizeof raw);

    if (a->n_sessions >= ZF_MAX_SESSIONS) {
        /* Evict the oldest session. */
        int oldest = 0;
        for (int i = 1; i < a->n_sessions; i++)
            if (a->sessions[i].created_wall < a->sessions[oldest].created_wall)
                oldest = i;
        a->sessions[oldest] = a->sessions[--a->n_sessions];
    }
    session *s = &a->sessions[a->n_sessions++];
    memset(s, 0, sizeof *s);
    zf_sha256_hex(token_out, 64, s->hash);
    memcpy(s->id, s->hash, 12);
    s->id[12] = '\0';
    zf_strcpy(s->csrf, sizeof s->csrf, csrf_out);
    if (device_name && *device_name && !zf_has_ctl(device_name))
        zf_strcpy(s->name, sizeof s->name, device_name);
    else
        zf_strcpy(s->name, sizeof s->name, "Remote");
    s->created_wall = zf_wall_ms();
    s->expires_wall = s->created_wall + (int64_t)a->session_days * 86400000LL;
    save_locked(a);
    pthread_mutex_unlock(&a->lock);
    return ZF_AUTH_OK;
}

static int find_locked(zf_auth *a, const char *token)
{
    if (!token || strlen(token) != 64)
        return -1;
    for (const char *p = token; *p; p++)
        if (!isxdigit((unsigned char)*p))
            return -1;
    char hash[65];
    zf_sha256_hex(token, 64, hash);
    int64_t now = zf_wall_ms();
    int found = -1;
    for (int i = 0; i < a->n_sessions; i++)
        if (zf_ct_equal(hash, a->sessions[i].hash, 64) && a->sessions[i].expires_wall > now)
            found = i;
    return found;
}

bool zf_auth_check(zf_auth *a, const char *token, char csrf_out[33])
{
    pthread_mutex_lock(&a->lock);
    int i = find_locked(a, token);
    if (i >= 0 && csrf_out)
        zf_strcpy(csrf_out, 33, a->sessions[i].csrf);
    pthread_mutex_unlock(&a->lock);
    return i >= 0;
}

int zf_auth_logout(zf_auth *a, const char *token)
{
    pthread_mutex_lock(&a->lock);
    int i = find_locked(a, token);
    if (i >= 0) {
        a->sessions[i] = a->sessions[--a->n_sessions];
        save_locked(a);
    }
    pthread_mutex_unlock(&a->lock);
    return i >= 0 ? 0 : -1;
}

cJSON *zf_auth_list(zf_auth *a)
{
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < a->n_sessions; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", a->sessions[i].id);
        cJSON_AddStringToObject(o, "name", a->sessions[i].name);
        cJSON_AddNumberToObject(o, "created", (double)a->sessions[i].created_wall);
        cJSON_AddNumberToObject(o, "expires", (double)a->sessions[i].expires_wall);
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&a->lock);
    return arr;
}

int zf_auth_revoke(zf_auth *a, const char *id)
{
    int n = 0;
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < a->n_sessions;) {
        if (!strcmp(id, "all") || !strcmp(id, a->sessions[i].id)) {
            a->sessions[i] = a->sessions[--a->n_sessions];
            n++;
        } else {
            i++;
        }
    }
    if (n)
        save_locked(a);
    pthread_mutex_unlock(&a->lock);
    return n;
}
