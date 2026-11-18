/* reporter.h — asynchronous Jellyfin playback reporting
 * (/Sessions/Playing, /Sessions/Playing/Progress, /Sessions/Playing/Stopped).
 *
 * Bounded queue (no unbounded backlog): pending progress reports for the same
 * play session are coalesced; start/stop reports are retried a few times with
 * jittered backoff; nothing is persisted to disk. */
#ifndef ZF_REPORTER_H
#define ZF_REPORTER_H

#include "jellyfin.h"
#include "player.h"

#define ZF_REPORT_QUEUE 16

typedef struct zf_reporter zf_reporter;

zf_reporter *zf_reporter_new(zf_jf *jf);
int zf_reporter_start(zf_reporter *r);
/* Stop accepting reports, try to deliver what is queued for up to drain_ms. */
void zf_reporter_shutdown(zf_reporter *r, int drain_ms);
void zf_reporter_free(zf_reporter *r);
void zf_reporter_submit(zf_reporter *r, const zf_report *rep);

/* Build the JSON body for a report (exposed for unit tests). */
cJSON *zf_report_body(const zf_report *rep);
const char *zf_report_path(zf_report_kind k);

/* Counters for diagnostics/tests. */
void zf_reporter_stats(zf_reporter *r, unsigned *sent, unsigned *failed, unsigned *dropped);

#endif
