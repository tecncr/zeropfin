/* relay.h — loopback-only authenticated media relay.
 *
 * ZeroPlay is handed http://127.0.0.1:<port>/media/<128-bit random id>. The
 * relay maps that id to one internally chosen Jellyfin path, adds the
 * MediaBrowser Authorization header itself, and streams the response back
 * through a small bounded ring buffer with backpressure (curl pause/unpause).
 * The Jellyfin token never appears in a URL, argv, or the browser.
 *
 *  - Binds 127.0.0.1 only; rejects non-loopback peers defensively.
 *  - GET and HEAD; single byte-range passthrough (200/206/416, Content-Range,
 *    Content-Length, Accept-Ranges, Content-Type).
 *  - Redirects are followed manually and only to the configured Jellyfin
 *    origin; anything else fails closed.
 *  - Mid-stream upstream failures (e.g. the server dropping an idle paused
 *    connection) are resumed transparently with a Range request when the
 *    upstream supports ranges. */
#ifndef ZF_RELAY_H
#define ZF_RELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jellyfin.h"

#define ZF_RELAY_MAX_GRANTS            4
#define ZF_RELAY_MAX_READERS           4 /* concurrent upstream transfers */
#define ZF_RELAY_MAX_READERS_PER_GRANT 3
#define ZF_RELAY_RING                  (128u * 1024) /* bytes buffered per reader */
#define ZF_RELAY_MAX_RESUMES           8

typedef struct zf_relay zf_relay;

zf_relay *zf_relay_new(zf_jf *jf, const zf_config *cfg);
int zf_relay_start(zf_relay *r, uint16_t port); /* 0 = ephemeral */
uint16_t zf_relay_port(zf_relay *r);
void zf_relay_stop(zf_relay *r);
void zf_relay_free(zf_relay *r);

/* Register an upstream Jellyfin path+query (relative to the server URL).
 * Writes the opaque id and the full loopback URL. */
int zf_relay_grant(zf_relay *r, const char *upstream_pathq, int64_t ttl_ms, char id_out[33], char *url_out,
                   size_t url_cap);
void zf_relay_revoke(zf_relay *r, const char *id);
void zf_relay_stats(zf_relay *r, unsigned *active_readers, unsigned long long *bytes_out, unsigned *resumes);

/* Helpers exposed for unit tests. Single "bytes=" ranges only:
 *   bytes=A-  -> start=A, end=-1      bytes=A-B -> start=A, end=B
 *   bytes=-N  -> start=-1, end=N (suffix length)
 * Returns 0 if valid, -1 otherwise (caller then ignores the header). */
int zf_range_parse(const char *hdr, int64_t *start, int64_t *end);
/* "bytes A-B/T" or "bytes A-B/ *" (T=-1) or "bytes * /T" (A=B=-1). */
int zf_content_range_parse(const char *hdr, int64_t *a, int64_t *b, int64_t *total);

#endif
