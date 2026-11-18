#include "creds.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

bool zf_token_syntax_ok(const char *s)
{
    if (!s || !*s || strlen(s) > 127)
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-')
            return false;
    return true;
}

bool zf_creds_configured(const zf_creds *c)
{
    return c->server_url[0] && c->access_token[0] && c->user_id[0] && c->device_id[0];
}

int zf_creds_ensure_device_id(zf_creds *c)
{
    if (c->device_id[0])
        return 0;
    char hex[33];
    if (zf_random_hex(hex, sizeof hex, 16) < 0)
        return -1;
    return zf_fmt(c->device_id, sizeof c->device_id, "zeropfin-%s", hex);
}

static void get_str(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring && !zf_has_ctl(v->valuestring))
        zf_strcpy(dst, cap, v->valuestring);
}

int zf_creds_load(const char *state_dir, zf_creds *out)
{
    memset(out, 0, sizeof *out);
    char path[PATH_MAX];
    if (zf_fmt(path, sizeof path, "%s/credentials.json", state_dir) < 0)
        return -1;
    struct stat st;
    if (stat(path, &st) < 0)
        return errno == ENOENT ? 1 : -1;
    if (st.st_mode & 0077) {
        LOGE("%s is accessible by group/other (mode %03o); refusing to use it", path, (unsigned)(st.st_mode & 0777));
        return -1;
    }
    char *text = zf_read_file(path, 16 * 1024, NULL);
    if (!text)
        return -1;
    cJSON *root = cJSON_Parse(text);
    memset(text, 0, strlen(text));
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        LOGE("credentials file is not valid JSON");
        return -1;
    }
    get_str(root, "serverUrl", out->server_url, sizeof out->server_url);
    get_str(root, "serverId", out->server_id, sizeof out->server_id);
    get_str(root, "serverName", out->server_name, sizeof out->server_name);
    get_str(root, "serverVersion", out->server_version, sizeof out->server_version);
    get_str(root, "userId", out->user_id, sizeof out->user_id);
    get_str(root, "userName", out->user_name, sizeof out->user_name);
    get_str(root, "accessToken", out->access_token, sizeof out->access_token);
    get_str(root, "deviceId", out->device_id, sizeof out->device_id);
    cJSON_Delete(root);
    if ((out->access_token[0] && !zf_token_syntax_ok(out->access_token)) ||
        (out->user_id[0] && !zf_token_syntax_ok(out->user_id)) ||
        (out->device_id[0] && !zf_token_syntax_ok(out->device_id))) {
        LOGE("credentials file contains malformed values");
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}

int zf_creds_save(const char *state_dir, const zf_creds *c)
{
    char path[PATH_MAX];
    if (zf_fmt(path, sizeof path, "%s/credentials.json", state_dir) < 0)
        return -1;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return -1;
    cJSON_AddStringToObject(o, "serverUrl", c->server_url);
    cJSON_AddStringToObject(o, "serverId", c->server_id);
    cJSON_AddStringToObject(o, "serverName", c->server_name);
    cJSON_AddStringToObject(o, "serverVersion", c->server_version);
    cJSON_AddStringToObject(o, "userId", c->user_id);
    cJSON_AddStringToObject(o, "userName", c->user_name);
    cJSON_AddStringToObject(o, "accessToken", c->access_token);
    cJSON_AddStringToObject(o, "deviceId", c->device_id);
    char *text = cJSON_Print(o);
    cJSON_Delete(o);
    if (!text)
        return -1;
    int rc = zf_write_file_atomic(path, text, strlen(text), 0600);
    memset(text, 0, strlen(text));
    free(text);
    return rc;
}
