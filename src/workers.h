/* workers.h — small fixed pool for blocking Jellyfin work (browse, images,
 * playback negotiation). The HTTP event loop never blocks: it suspends the
 * connection, submits a job here, and the job resumes it when done.
 * Bounded queue: when full, callers answer 503 (retryable). */
#ifndef ZF_WORKERS_H
#define ZF_WORKERS_H

#include "jellyfin.h"

typedef struct zf_workers zf_workers;
/* conn is NULL when the job is cancelled during shutdown; the job must
 * still complete its bookkeeping (e.g. resume the HTTP connection). */
typedef void (*zf_job_fn)(void *arg, zf_jf_conn *conn);

zf_workers *zf_workers_new(int threads, int queue_cap);
int zf_workers_submit(zf_workers *w, zf_job_fn fn, void *arg); /* -1 when full */
void zf_workers_shutdown(zf_workers *w);
void zf_workers_free(zf_workers *w);

#endif
