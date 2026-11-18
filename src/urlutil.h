/* urlutil.h — strict URL parsing, origin checks, encoding and id validation. */
#ifndef ZF_URLUTIL_H
#define ZF_URLUTIL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char scheme[8];  /* "http" or "https" */
    char host[256];  /* lowercased; IPv6 literals keep their brackets */
    int port;        /* explicit or scheme default */
    char path[1024]; /* starts with '/', includes the query string if any */
} zf_url;

/* Parse an absolute http(s) URL. Rejects userinfo, fragments, whitespace,
 * control bytes, and other schemes. Returns 0 or -1. */
int zf_url_parse(const char *url, zf_url *out);
bool zf_url_same_origin(const zf_url *a, const zf_url *b);

/* Normalize a user-entered Jellyfin base URL: http(s) only, no query,
 * fragment or credentials; trailing '/' removed. */
int zf_normalize_server_url(const char *in, char *out, size_t cap);

/* Resolve a redirect Location against the URL that produced it. Only
 * absolute URLs and absolute paths are accepted. */
int zf_url_resolve_location(const char *base, const char *location, char *out, size_t cap);

/* RFC 3986 percent-encoding of everything except unreserved characters. */
int zf_urlencode(const char *in, char *out, size_t cap);

/* Jellyfin item/user ids: 32 hex digits, optionally dashed (GUID form).
 * Writes the canonical 32 lowercase hex form into out (cap >= 33). */
bool zf_jf_id_normalize(const char *in, char *out, size_t cap);

#endif
