#include "workers.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct {
    zf_job_fn fn;
    void *arg;
} job;

struct zf_workers {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t *threads;
    int n_threads;
    job *q;
    int cap, head, len;
    bool stopping;
};

static void *worker_main(void *arg)
{
    zf_workers *w = arg;
    zf_jf_conn *conn = zf_jf_conn_new();
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->len && !w->stopping)
            pthread_cond_wait(&w->cond, &w->lock);
        if (!w->len)
            break;
        job j = w->q[w->head];
        w->head = (w->head + 1) % w->cap;
        w->len--;
        bool cancelled = w->stopping;
        pthread_mutex_unlock(&w->lock);
        j.fn(j.arg, cancelled ? NULL : conn);
        pthread_mutex_lock(&w->lock);
    }
    pthread_mutex_unlock(&w->lock);
    zf_jf_conn_free(conn);
    return NULL;
}

zf_workers *zf_workers_new(int threads, int queue_cap)
{
    zf_workers *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->q = calloc((size_t)queue_cap, sizeof *w->q);
    w->threads = calloc((size_t)threads, sizeof *w->threads);
    if (!w->q || !w->threads) {
        free(w->q);
        free(w->threads);
        free(w);
        return NULL;
    }
    w->cap = queue_cap;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    for (int i = 0; i < threads; i++) {
        if (pthread_create(&w->threads[i], NULL, worker_main, w) != 0)
            break;
        w->n_threads++;
    }
    if (!w->n_threads) {
        zf_workers_free(w);
        return NULL;
    }
    return w;
}

int zf_workers_submit(zf_workers *w, zf_job_fn fn, void *arg)
{
    pthread_mutex_lock(&w->lock);
    if (w->stopping || w->len >= w->cap) {
        pthread_mutex_unlock(&w->lock);
        return -1;
    }
    w->q[(w->head + w->len) % w->cap] = (job){fn, arg};
    w->len++;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    return 0;
}

void zf_workers_shutdown(zf_workers *w)
{
    if (!w)
        return;
    pthread_mutex_lock(&w->lock);
    w->stopping = true;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    for (int i = 0; i < w->n_threads; i++)
        pthread_join(w->threads[i], NULL);
    w->n_threads = 0;
}

void zf_workers_free(zf_workers *w)
{
    if (!w)
        return;
    zf_workers_shutdown(w);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
    free(w->q);
    free(w->threads);
    free(w);
}
