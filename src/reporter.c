#include "reporter.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct zf_reporter {
    zf_jf *jf;
    pthread_t thread;
    bool started;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    zf_report q[ZF_REPORT_QUEUE];
    int head, len;
    bool stopping;
    int64_t drain_deadline;
    unsigned sent, failed, dropped;
};

const char *zf_report_path(zf_report_kind k)
{
    switch (k) {
    case ZF_REPORT_START: return "/Sessions/Playing";
    case ZF_REPORT_PROGRESS: return "/Sessions/Playing/Progress";
    case ZF_REPORT_STOP: return "/Sessions/Playing/Stopped";
    }
    return "/Sessions/Playing/Progress";
}

cJSON *zf_report_body(const zf_report *rep)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ItemId", rep->item_id);
    if (rep->media_source_id[0])
        cJSON_AddStringToObject(o, "MediaSourceId", rep->media_source_id);
    if (rep->play_session_id[0])
        cJSON_AddStringToObject(o, "PlaySessionId", rep->play_session_id);
    /* Position is omitted, never invented, when it is not trustworthy. */
    if (rep->has_position)
        cJSON_AddNumberToObject(o, "PositionTicks", (double)rep->position_ticks);
    if (rep->kind == ZF_REPORT_STOP) {
        cJSON_AddBoolToObject(o, "Failed", rep->failed);
        return o;
    }
    cJSON_AddBoolToObject(o, "CanSeek", true);
    cJSON_AddBoolToObject(o, "IsPaused", rep->paused);
    cJSON_AddBoolToObject(o, "IsMuted", false);
    cJSON_AddStringToObject(o, "PlayMethod", "DirectPlay");
    if (rep->audio_stream_index >= 0)
        cJSON_AddNumberToObject(o, "AudioStreamIndex", rep->audio_stream_index);
    if (rep->subtitle_stream_index >= 0)
        cJSON_AddNumberToObject(o, "SubtitleStreamIndex", rep->subtitle_stream_index);
    if (rep->kind == ZF_REPORT_PROGRESS && rep->event_name)
        cJSON_AddStringToObject(o, "EventName", rep->event_name);
    return o;
}

zf_reporter *zf_reporter_new(zf_jf *jf)
{
    zf_reporter *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->jf = jf;
    pthread_mutex_init(&r->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&r->cond, &ca);
    pthread_condattr_destroy(&ca);
    return r;
}

void zf_reporter_submit(zf_reporter *r, const zf_report *rep)
{
    if (!r || !rep->item_id[0])
        return;
    pthread_mutex_lock(&r->lock);
    if (r->stopping && rep->kind != ZF_REPORT_STOP) {
        pthread_mutex_unlock(&r->lock);
        return;
    }
    /* Coalesce: a newer progress report supersedes a queued one for the same session. */
    if (rep->kind == ZF_REPORT_PROGRESS) {
        for (int i = 0; i < r->len; i++) {
            zf_report *q = &r->q[(r->head + i) % ZF_REPORT_QUEUE];
            if (q->kind == ZF_REPORT_PROGRESS && !strcmp(q->play_session_id, rep->play_session_id) &&
                !strcmp(q->item_id, rep->item_id)) {
                *q = *rep;
                pthread_mutex_unlock(&r->lock);
                return;
            }
        }
    }
    if (r->len >= ZF_REPORT_QUEUE) {
        /* Prefer dropping the oldest progress report; never a start/stop. */
        int victim = -1;
        for (int i = 0; i < r->len; i++)
            if (r->q[(r->head + i) % ZF_REPORT_QUEUE].kind == ZF_REPORT_PROGRESS) {
                victim = i;
                break;
            }
        if (victim < 0 && rep->kind == ZF_REPORT_PROGRESS) {
            r->dropped++;
            pthread_mutex_unlock(&r->lock);
            return;
        }
        if (victim < 0)
            victim = 0;
        for (int i = victim; i < r->len - 1; i++)
            r->q[(r->head + i) % ZF_REPORT_QUEUE] = r->q[(r->head + i + 1) % ZF_REPORT_QUEUE];
        r->len--;
        r->dropped++;
        LOGW("playback report queue full; dropped one report");
    }
    r->q[(r->head + r->len) % ZF_REPORT_QUEUE] = *rep;
    r->len++;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

static void wait_ms(zf_reporter *r, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&r->cond, &r->lock, &ts);
}

static void *thread_main(void *arg)
{
    zf_reporter *r = arg;
    zf_jf_conn *c = zf_jf_conn_new();
    pthread_mutex_lock(&r->lock);
    for (;;) {
        while (!r->len && !r->stopping)
            pthread_cond_wait(&r->cond, &r->lock);
        if (!r->len && r->stopping)
            break;
        if (r->stopping && zf_mono_ms() > r->drain_deadline)
            break;
        zf_report rep = r->q[r->head];
        r->head = (r->head + 1) % ZF_REPORT_QUEUE;
        r->len--;
        pthread_mutex_unlock(&r->lock);

        int attempts = rep.kind == ZF_REPORT_PROGRESS ? 1 : 3;
        zf_jf_err e = ZF_JF_ERR_NETWORK;
        for (int a = 0; a < attempts; a++) {
            cJSON *body = zf_report_body(&rep);
            e = c ? zf_jf_post_json(r->jf, c, zf_report_path(rep.kind), body, NULL) : ZF_JF_ERR_NETWORK;
            cJSON_Delete(body);
            if (e == ZF_JF_OK || e == ZF_JF_ERR_AUTH || e == ZF_JF_ERR_BAD_REQUEST || e == ZF_JF_ERR_NOT_CONFIGURED ||
                e == ZF_JF_ERR_FORBIDDEN)
                break;
            pthread_mutex_lock(&r->lock);
            bool give_up = r->stopping && zf_mono_ms() > r->drain_deadline;
            if (!give_up && a + 1 < attempts) {
                uint8_t j = 0;
                zf_random_bytes(&j, 1);
                wait_ms(r, (1000 << a) + j * 2);
            }
            pthread_mutex_unlock(&r->lock);
            if (give_up)
                break;
        }
        pthread_mutex_lock(&r->lock);
        if (e == ZF_JF_OK) {
            r->sent++;
        } else {
            r->failed++;
            LOGW("playback report %s failed: %s", zf_report_path(rep.kind), zf_jf_err_message(e));
        }
    }
    pthread_mutex_unlock(&r->lock);
    zf_jf_conn_free(c);
    return NULL;
}

int zf_reporter_start(zf_reporter *r)
{
    if (pthread_create(&r->thread, NULL, thread_main, r) != 0)
        return -1;
    r->started = true;
    return 0;
}

void zf_reporter_shutdown(zf_reporter *r, int drain_ms)
{
    if (!r || !r->started)
        return;
    pthread_mutex_lock(&r->lock);
    r->stopping = true;
    r->drain_deadline = zf_mono_ms() + drain_ms;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->thread, NULL);
    r->started = false;
}

void zf_reporter_free(zf_reporter *r)
{
    if (!r)
        return;
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    free(r);
}

void zf_reporter_stats(zf_reporter *r, unsigned *sent, unsigned *failed, unsigned *dropped)
{
    pthread_mutex_lock(&r->lock);
    *sent = r->sent;
    *failed = r->failed;
    *dropped = r->dropped;
    pthread_mutex_unlock(&r->lock);
}
