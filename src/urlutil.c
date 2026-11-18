#include "urlutil.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

int zf_url_parse(const char *url, zf_url *out)
{
    memset(out, 0, sizeof *out);
    if (!url || zf_has_ctl(url) || strpbrk(url, " \\#"))
        return -1;
    const char *p;
    if (!strncasecmp(url, "http://", 7)) {
        zf_strcpy(out->scheme, sizeof out->scheme, "http");
        out->port = 80;
        p = url + 7;
    } else if (!strncasecmp(url, "https://", 8)) {
        zf_strcpy(out->scheme, sizeof out->scheme, "https");
        out->port = 443;
        p = url + 8;
    } else {
        return -1;
    }
    const char *auth_end = p + strcspn(p, "/?");
    const char *at = memchr(p, '@', (size_t)(auth_end - p));
    if (at)
        return -1; /* credentials in URLs are never accepted */
    const char *host_end;
    const char *port_start = NULL;
    if (*p == '[') {
        const char *rb = memchr(p, ']', (size_t)(auth_end - p));
        if (!rb)
            return -1;
        host_end = rb + 1;
        if (host_end < auth_end) {
            if (*host_end != ':')
                return -1;
            port_start = host_end + 1;
        }
    } else {
        const char *colon = memchr(p, ':', (size_t)(auth_end - p));
        host_end = colon ? colon : auth_end;
        if (colon)
            port_start = colon + 1;
    }
    size_t hl = (size_t)(host_end - p);
    if (hl == 0 || hl >= sizeof out->host)
        return -1;
    for (size_t i = 0; i < hl; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (!(isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == '[' || ch == ']' || ch == ':'))
            return -1;
        out->host[i] = (char)tolower(ch);
    }
    out->host[hl] = '\0';
    if (port_start) {
        size_t pl = (size_t)(auth_end - port_start);
        if (pl == 0 || pl > 5)
            return -1;
        int port = 0;
        for (size_t i = 0; i < pl; i++) {
            if (!isdigit((unsigned char)port_start[i]))
                return -1;
            port = port * 10 + (port_start[i] - '0');
        }
        if (port < 1 || port > 65535)
            return -1;
        out->port = port;
    }
    if (*auth_end == '\0')
        zf_strcpy(out->path, sizeof out->path, "/");
    else if (*auth_end == '?') {
        if (zf_fmt(out->path, sizeof out->path, "/%s", auth_end) < 0)
            return -1;
    } else if (zf_strcpy(out->path, sizeof out->path, auth_end) < 0)
        return -1;
    return 0;
}

bool zf_url_same_origin(const zf_url *a, const zf_url *b)
{
    return strcmp(a->scheme, b->scheme) == 0 && strcmp(a->host, b->host) == 0 && a->port == b->port;
}

int zf_normalize_server_url(const char *in, char *out, size_t cap)
{
    char tmp[512];
    size_t n = strlen(in);
    while (n && isspace((unsigned char)in[n - 1]))
        n--;
    while (n && isspace((unsigned char)*in)) {
        in++;
        n--;
    }
    if (n == 0 || n >= sizeof tmp)
        return -1;
    memcpy(tmp, in, n);
    tmp[n] = '\0';
    zf_url u;
    if (zf_url_parse(tmp, &u) < 0 || strchr(u.path, '?'))
        return -1;
    size_t pl = strlen(u.path);
    while (pl > 1 && u.path[pl - 1] == '/')
        u.path[--pl] = '\0';
    bool default_port = (!strcmp(u.scheme, "http") && u.port == 80) || (!strcmp(u.scheme, "https") && u.port == 443);
    char portbuf[8] = "";
    if (!default_port)
        zf_fmt(portbuf, sizeof portbuf, ":%d", u.port);
    return zf_fmt(out, cap, "%s://%s%s%s", u.scheme, u.host, portbuf, strcmp(u.path, "/") ? u.path : "");
}

int zf_url_resolve_location(const char *base, const char *location, char *out, size_t cap)
{
    if (!location || !*location || zf_has_ctl(location))
        return -1;
    if (!strncasecmp(location, "http://", 7) || !strncasecmp(location, "https://", 8))
        return zf_strcpy(out, cap, location);
    if (location[0] != '/' || location[1] == '/')
        return -1; /* scheme-relative and relative paths are refused */
    zf_url b;
    if (zf_url_parse(base, &b) < 0)
        return -1;
    return zf_fmt(out, cap, "%s://%s:%d%s", b.scheme, b.host, b.port, location);
}

int zf_urlencode(const char *in, char *out, size_t cap)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (o + 1 >= cap)
                return -1;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= cap)
                return -1;
            out[o++] = '%';
            out[o++] = hx[*p >> 4];
            out[o++] = hx[*p & 15];
        }
    }
    if (o >= cap)
        return -1;
    out[o] = '\0';
    return 0;
}

bool zf_jf_id_normalize(const char *in, char *out, size_t cap)
{
    if (!in || cap < 33)
        return false;
    size_t n = strlen(in);
    if (n != 32 && n != 36)
        return false;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = in[i];
        if (n == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (ch != '-')
                return false;
            continue;
        }
        if (!isxdigit((unsigned char)ch))
            return false;
        out[o++] = (char)tolower((unsigned char)ch);
    }
    out[o] = '\0';
    return o == 32;
}
