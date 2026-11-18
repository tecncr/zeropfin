#include "netfilter.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static bool v4_private(uint32_t a /* host order */)
{
    return (a >> 24) == 127 ||                 /* 127/8 loopback */
           (a >> 24) == 10 ||                  /* 10/8 */
           (a >> 20) == ((172u << 4) | 1) ||   /* 172.16/12 */
           (a >> 16) == ((192u << 8) | 168) || /* 192.168/16 */
           (a >> 16) == ((169u << 8) | 254) || /* 169.254/16 link-local */
           (a >> 22) == ((100u << 2) | 1);     /* 100.64/10 CGNAT (e.g. VPN overlays) */
}

static bool v6_is_mapped_v4(const struct in6_addr *a)
{
    static const uint8_t pfx[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return memcmp(a->s6_addr, pfx, 12) == 0;
}

bool zf_addr_is_private(const struct sockaddr *sa)
{
    if (!sa)
        return false;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)sa;
        return v4_private(ntohl(s4->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
        if (v6_is_mapped_v4(a)) {
            uint32_t v4 = (uint32_t)a->s6_addr[12] << 24 | (uint32_t)a->s6_addr[13] << 16 |
                          (uint32_t)a->s6_addr[14] << 8 | a->s6_addr[15];
            return v4_private(v4);
        }
        if (IN6_IS_ADDR_LOOPBACK(a))
            return true;
        if ((a->s6_addr[0] & 0xfe) == 0xfc) /* fc00::/7 ULA */
            return true;
        if (a->s6_addr[0] == 0xfe && (a->s6_addr[1] & 0xc0) == 0x80) /* fe80::/10 */
            return true;
    }
    return false;
}

static bool prefix_eq(const uint8_t *a, const uint8_t *b, int bits)
{
    int full = bits / 8, rem = bits % 8;
    if (memcmp(a, b, (size_t)full) != 0)
        return false;
    if (rem) {
        uint8_t mask = (uint8_t)(0xff << (8 - rem));
        if ((a[full] & mask) != (b[full] & mask))
            return false;
    }
    return true;
}

int zf_cidr_match(const struct sockaddr *sa, const char *cidr)
{
    char tmp[64];
    if (zf_strcpy(tmp, sizeof tmp, cidr) < 0)
        return -1;
    int bits = -1;
    char *slash = strchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        int64_t b;
        if (zf_parse_i64(slash + 1, 0, 128, &b) < 0)
            return -1;
        bits = (int)b;
    }
    struct in_addr n4;
    struct in6_addr n6;
    if (inet_pton(AF_INET, tmp, &n4) == 1) {
        if (bits < 0)
            bits = 32;
        if (bits > 32)
            return -1;
        uint8_t addr[4];
        if (sa->sa_family == AF_INET) {
            memcpy(addr, &((const struct sockaddr_in *)(const void *)sa)->sin_addr, 4);
        } else if (sa->sa_family == AF_INET6) {
            const struct in6_addr *a = &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
            if (!v6_is_mapped_v4(a))
                return 0;
            memcpy(addr, a->s6_addr + 12, 4);
        } else {
            return 0;
        }
        return prefix_eq(addr, (const uint8_t *)&n4, bits) ? 1 : 0;
    }
    if (inet_pton(AF_INET6, tmp, &n6) == 1) {
        if (bits < 0)
            bits = 128;
        if (sa->sa_family != AF_INET6)
            return 0;
        const struct in6_addr *a = &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
        return prefix_eq(a->s6_addr, n6.s6_addr, bits) ? 1 : 0;
    }
    return -1;
}

/* Split "host[:port]" / "[v6][:port]" into a lowercase host. */
static int host_only(const char *h, char *out, size_t cap)
{
    if (!h || !*h || zf_has_ctl(h) || strlen(h) >= cap)
        return -1;
    if (h[0] == '[') {
        const char *rb = strchr(h, ']');
        if (!rb)
            return -1;
        size_t n = (size_t)(rb - h + 1);
        memcpy(out, h, n);
        out[n] = '\0';
        const char *rest = rb + 1;
        if (*rest && *rest != ':')
            return -1;
    } else {
        size_t n = strcspn(h, ":");
        memcpy(out, h, n);
        out[n] = '\0';
        if (h[n] == ':') {
            int64_t port;
            if (zf_parse_i64(h + n + 1, 1, 65535, &port) < 0)
                return -1;
        }
    }
    for (char *p = out; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return out[0] ? 0 : -1;
}

bool zf_host_allowed(const char *host_header, const char *hostname, const zf_config *cfg)
{
    char h[256];
    if (host_only(host_header, h, sizeof h) < 0)
        return false;
    if (h[0] == '[') {
        char inner[256];
        size_t n = strlen(h);
        if (n < 3)
            return false;
        memcpy(inner, h + 1, n - 2);
        inner[n - 2] = '\0';
        struct in6_addr a6;
        return inet_pton(AF_INET6, inner, &a6) == 1;
    }
    struct in_addr a4;
    if (inet_pton(AF_INET, h, &a4) == 1)
        return true;
    if (!strcmp(h, "localhost"))
        return true;
    if (hostname && *hostname) {
        if (!strcasecmp(h, hostname))
            return true;
        char local[300];
        if (zf_fmt(local, sizeof local, "%s.local", hostname) == 0 && !strcasecmp(h, local))
            return true;
    }
    for (int i = 0; cfg && i < cfg->n_allowed_hosts; i++)
        if (!strcasecmp(h, cfg->allowed_hosts[i]))
            return true;
    return false;
}

bool zf_origin_allowed(const char *origin, const char *host_header)
{
    if (!origin)
        return true;
    if (!host_header)
        return false;
    const char *rest;
    if (!strncasecmp(origin, "http://", 7))
        rest = origin + 7;
    else if (!strncasecmp(origin, "https://", 8))
        rest = origin + 8;
    else
        return false; /* includes the literal "null" origin */
    return strcasecmp(rest, host_header) == 0;
}
