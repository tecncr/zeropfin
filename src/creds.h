/* creds.h — Jellyfin account binding stored in <state_dir>/credentials.json (0600).
 * Holds the access token obtained by `zeropfin setup`; never the password. */
#ifndef ZF_CREDS_H
#define ZF_CREDS_H

#include <stdbool.h>

typedef struct {
    char server_url[256]; /* normalized origin + optional base path, no trailing '/' */
    char server_id[64];
    char server_name[128];
    char server_version[32];
    char user_id[64];
    char user_name[128];
    char access_token[128];
    char device_id[64];
} zf_creds;

/* 0 = loaded, 1 = no credentials file, -1 = error (corrupt/unsafe file). */
int zf_creds_load(const char *state_dir, zf_creds *out);
int zf_creds_save(const char *state_dir, const zf_creds *c);
bool zf_creds_configured(const zf_creds *c);
/* Generate a stable random device id if missing. */
int zf_creds_ensure_device_id(zf_creds *c);
/* Token/user-id syntax checks (alnum and '-' only). */
bool zf_token_syntax_ok(const char *s);

#endif
