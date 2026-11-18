/* netfilter.h — LAN-only client filtering and Host/Origin validation
 * (DNS-rebinding and cross-site request defenses). */
#ifndef ZF_NETFILTER_H
#define ZF_NETFILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

#include "config.h"

/* Loopback, RFC 1918, link-local, CGNAT (100.64/10), IPv6 ULA/link-local,
 * and IPv4-mapped forms of those. */
bool zf_addr_is_private(const struct sockaddr *sa);
/* "a.b.c.d/n" or "x::y/n" (or a bare address). Returns -1 on bad syntax. */
int zf_cidr_match(const struct sockaddr *sa, const char *cidr);

/* Host header check. Accepts IP literals, "localhost", the machine host
 * name (and "<name>.local"), plus extra allowed names. Port is ignored. */
bool zf_host_allowed(const char *host_header, const char *hostname, const zf_config *cfg);

/* Origin must be absent or exactly http(s)://<Host header>. */
bool zf_origin_allowed(const char *origin, const char *host_header);

#endif
