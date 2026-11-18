/* Component test: the real ZeroPlay supervisor (zf_player) driving the mock
 * player binary through the actual pipe protocol. No video, no Pi. */
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "player.h"
#include "util.h"
#include "zt.h"

#ifndef MOCK_ZEROPLAY
#error "MOCK_ZEROPLAY must be defined"
#endif

typedef struct {
    pthread_mutex_t lock;
    zf_report reports[64];
    int n;
    int released;
} sink;

static void on_report(void *ctx, const zf_report *r)
{
    sink *s = ctx;
    pthread_mutex_lock(&s->lock);
    if (s->n < 64)
        s->reports[s->n++] = *r;
    pthread_mutex_unlock(&s->lock);
}

static void on_released(void *ctx, const zf_media *m)
{
    (void)m;
    sink *s = ctx;
    pthread_mutex_lock(&s->lock);
    s->released++;
    pthread_mutex_unlock(&s->lock);
}

static int count(sink *s, zf_report_kind k)
{
    pthread_mutex_lock(&s->lock);
    int c = 0;
    for (int i = 0; i < s->n; i++)
        c += s->reports[i].kind == k;
    pthread_mutex_unlock(&s->lock);
    return c;
}

static zf_report last(sink *s, zf_report_kind k)
{
    zf_report r;
    memset(&r, 0, sizeof r);
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n; i++)
        if (s->reports[i].kind == k)
            r = s->reports[i];
    pthread_mutex_unlock(&s->lock);
    return r;
}

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

typedef bool (*pred_fn)(const zf_player_snapshot *s, const void *arg);

static bool wait_for(zf_player *p, pred_fn fn, const void *arg, int timeout_ms, zf_player_snapshot *out)
{
    int64_t end = zf_mono_ms() + timeout_ms;
    zf_player_snapshot s;
    do {
        zf_player_snapshot_get(p, &s);
        if (fn(&s, arg)) {
            if (out)
                *out = s;
            return true;
        }
        sleep_ms(20);
    } while (zf_mono_ms() < end);
    if (out)
        *out = s;
    fprintf(stderr, "  timeout: state=%s proc=%s\n", ps_state_name(s.state), zf_proc_state_name(s.proc));
    return false;
}

static bool is_ready(const zf_player_snapshot *s, const void *a)
{
    (void)a;
    return s->proc == ZF_PROC_READY;
}

static bool in_state(const zf_player_snapshot *s, const void *a)
{
    return s->state == *(const ps_state *)a;
}

static bool verified(const zf_player_snapshot *s, const void *a)
{
    (void)a;
    return s->source == POS_VERIFIED;
}

static zf_config base_cfg(void)
{
    zf_config c;
    zf_config_defaults(&c);
    zf_strcpy(c.zeroplay_path, sizeof c.zeroplay_path, MOCK_ZEROPLAY);
    c.player_ready_timeout_s = 3;
    c.player_load_timeout_s = 4;
    c.progress_interval_s = 5;
    return c;
}

static zf_media media(const char *id, int64_t runtime_ms, int64_t start_ms, const char *url)
{
    zf_media m;
    memset(&m, 0, sizeof m);
    zf_strcpy(m.item_id, sizeof m.item_id, id);
    zf_strcpy(m.media_source_id, sizeof m.media_source_id, "ms1");
    zf_strcpy(m.play_session_id, sizeof m.play_session_id, "ps1");
    zf_strcpy(m.title, sizeof m.title, "Test");
    zf_strcpy(m.url, sizeof m.url, url ? url : "http://127.0.0.1:9/media/0123456789abcdef0123456789abcdef");
    m.runtime_ms = runtime_ms;
    m.start_ms = start_ms;
    m.audio_stream_index = 1;
    m.subtitle_stream_index = -1;
    return m;
}

static zf_player *start_player(sink *s, const char *flags, zf_config *cfg)
{
    setenv("MOCK_ZEROPLAY_FLAGS", flags, 1);
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->lock, NULL);
    zf_player_hooks h = {.ctx = s, .report = on_report, .released = on_released};
    zf_player *p = zf_player_new(cfg, h);
    zf_player_start(p);
    return p;
}

static void stop_player(zf_player *p)
{
    zf_player_shutdown(p);
    zf_player_free(p);
}

static void ext_lifecycle(void)
{
    sink s;
    zf_config cfg = base_cfg();
    zf_player *p = start_player(&s, "duration-ms=4000", &cfg);
    zf_player_snapshot snap;
    ZT_CHECK(wait_for(p, is_ready, NULL, 3000, &snap));
    ZT_CHECK(snap.ext_events);
    ZT_EQ_INT(zf_player_pause(p), -1); /* nothing playing */
    zf_media m = media("a0000000000000000000000000000001", 4000, 0, NULL);
    ZT_EQ_INT(zf_player_load(p, &m), 0);
    ps_state want = PS_PLAYING;
    ZT_CHECK(wait_for(p, in_state, &want, 2000, &snap));
    ZT_EQ_INT(snap.duration_ms, 4000);
    ZT_EQ_INT(count(&s, ZF_REPORT_START), 1);
    ZT_CHECK(wait_for(p, verified, NULL, 2500, &snap));
    ZT_EQ_INT(zf_player_pause(p), 0);
    want = PS_PAUSED;
    ZT_CHECK(wait_for(p, in_state, &want, 1000, &snap));
    int64_t frozen = snap.position_ms;
    sleep_ms(400);
    zf_player_snapshot_get(p, &snap);
    ZT_CHECK(snap.position_ms - frozen < 50);
    ZT_EQ_STR(last(&s, ZF_REPORT_PROGRESS).event_name, "Pause");
    ZT_CHECK(last(&s, ZF_REPORT_PROGRESS).paused);
    ZT_EQ_INT(zf_player_seek(p, 3000), 0);
    ZT_EQ_INT(zf_player_resume(p), 0);
    want = PS_ENDED;
    ZT_CHECK(wait_for(p, in_state, &want, 3000, &snap));
    ZT_EQ_INT(count(&s, ZF_REPORT_STOP), 1);
    zf_report stop = last(&s, ZF_REPORT_STOP);
    ZT_CHECK(!stop.failed);
    ZT_CHECK(stop.has_position);
    ZT_EQ_INT(stop.position_ticks, 4000LL * 10000); /* full runtime -> Jellyfin marks played */
    ZT_EQ_INT(s.released, 1);
    stop_player(p);
}

static void crash_is_not_completion(void)
{
    sink s;
    zf_config cfg = base_cfg();
    zf_player *p = start_player(&s, "duration-ms=60000 crash-after-ms=900", &cfg);
    ZT_CHECK(wait_for(p, is_ready, NULL, 3000, NULL));
    zf_media m = media("a0000000000000000000000000000002", 60000, 10000, NULL);
    zf_player_load(p, &m);
    ps_state want = PS_ERROR;
    zf_player_snapshot snap;
    ZT_CHECK(wait_for(p, in_state, &want, 4000, &snap));
    ZT_CHECK(strstr(snap.error, "unexpectedly") != NULL);
    zf_report stop = last(&s, ZF_REPORT_STOP);
    ZT_CHECK(stop.failed);
    ZT_CHECK(!stop.has_position || stop.position_ticks < 60000LL * 10000);
    ZT_CHECK(stop.position_ticks >= 10000LL * 10000 || !stop.has_position); /* resumed from 10 s */
    ZT_EQ_INT(s.released, 1);
    /* supervisor restarts the player */
    ZT_CHECK(wait_for(p, is_ready, NULL, 4000, &snap));
    ZT_EQ_INT(snap.restarts, 1);
    stop_player(p);
}

static void stock_player(void)
{
    sink s;
    zf_config cfg = base_cfg();
    char logpath[] = "/tmp/zfmocklogXXXXXX";
    int fd = mkstemp(logpath);
    close(fd);
    char flags[128];
    snprintf(flags, sizeof flags, "stock duration-ms=60000 log=%s", logpath);
    zf_player *p = start_player(&s, flags, &cfg);
    zf_player_snapshot snap;
    ZT_CHECK(wait_for(p, is_ready, NULL, 4000, &snap));
    ZT_CHECK(!snap.ext_events);
    zf_media m = media("a0000000000000000000000000000003", 60000, 5000, NULL);
    zf_player_load(p, &m);
    ps_state want = PS_PLAYING;
    ZT_CHECK(wait_for(p, in_state, &want, 4000, &snap));
    ZT_EQ_INT(snap.source, POS_ESTIMATED);
    ZT_CHECK(snap.position_ms >= 5000);
    ZT_EQ_INT(snap.duration_ms, 60000); /* from Jellyfin runtime, not the player */
    char *log = zf_read_file(logpath, 65536, NULL);
    ZT_CHECK(log && strstr(log, "events on\nload http://127.0.0.1:9/media/") && strstr(log, "\nseek 5000\n"));
    ZT_CHECK(log && !strstr(log, "loadat"));
    free(log);
    stop_player(p);
    zf_report stop = last(&s, ZF_REPORT_STOP);
    ZT_CHECK(!stop.failed); /* shutdown stop is orderly */
    unlink(logpath);
}

static void load_failure(void)
{
    sink s;
    zf_config cfg = base_cfg();
    zf_player *p = start_player(&s, "duration-ms=60000", &cfg);
    ZT_CHECK(wait_for(p, is_ready, NULL, 3000, NULL));
    zf_media m = media("a0000000000000000000000000000004", 60000, 0,
                       "http://127.0.0.1:9/media/failfailfailfailfailfailfailfail");
    zf_player_load(p, &m);
    ps_state want = PS_ERROR;
    zf_player_snapshot snap;
    ZT_CHECK(wait_for(p, in_state, &want, 3000, &snap));
    ZT_CHECK(strstr(snap.error, "could not open") != NULL);
    ZT_EQ_INT(count(&s, ZF_REPORT_START), 0);
    ZT_EQ_INT(count(&s, ZF_REPORT_STOP), 0);
    ZT_EQ_INT(s.released, 1);
    /* invalid URL rejected synchronously */
    zf_media bad = media("a0000000000000000000000000000004", 0, 0, "http://evil.example/x");
    ZT_EQ_INT(zf_player_load(p, &bad), -1);
    stop_player(p);
}

static void replacement(void)
{
    sink s;
    zf_config cfg = base_cfg();
    zf_player *p = start_player(&s, "duration-ms=60000", &cfg);
    ZT_CHECK(wait_for(p, is_ready, NULL, 3000, NULL));
    zf_media a = media("a0000000000000000000000000000005", 60000, 0, NULL);
    zf_media b = media("a0000000000000000000000000000006", 60000, 0, NULL);
    zf_player_load(p, &a);
    ps_state want = PS_PLAYING;
    ZT_CHECK(wait_for(p, in_state, &want, 2000, NULL));
    zf_player_load(p, &b);
    zf_player_snapshot snap;
    sleep_ms(300);
    ZT_CHECK(wait_for(p, in_state, &want, 2000, &snap));
    ZT_EQ_INT(snap.generation, 2);
    ZT_EQ_STR(snap.media.item_id, "a0000000000000000000000000000006");
    ZT_EQ_INT(count(&s, ZF_REPORT_START), 2);
    zf_report stop = last(&s, ZF_REPORT_STOP);
    ZT_EQ_STR(stop.item_id, "a0000000000000000000000000000005");
    ZT_CHECK(!stop.failed);
    ZT_EQ_INT(s.released, 1);
    ZT_EQ_INT(zf_player_stop(p), 0);
    want = PS_STOPPED;
    ZT_CHECK(wait_for(p, in_state, &want, 1000, NULL));
    ZT_EQ_INT(s.released, 2);
    stop_player(p);
}

static void start_failure(void)
{
    sink s;
    zf_config cfg = base_cfg();
    cfg.player_ready_timeout_s = 1;
    cfg.player_load_timeout_s = 1;
    zf_player *p = start_player(&s, "fail-start", &cfg);
    sleep_ms(700);
    zf_player_snapshot snap;
    zf_player_snapshot_get(p, &snap);
    ZT_CHECK(snap.proc == ZF_PROC_BACKOFF || snap.proc == ZF_PROC_STARTING);
    ZT_CHECK(snap.restarts >= 1);
    ZT_CHECK(strstr(snap.player_error, "exited with status 1") != NULL);
    zf_media m = media("a0000000000000000000000000000007", 1000, 0, NULL);
    zf_player_load(p, &m);
    ps_state want = PS_ERROR;
    ZT_CHECK(wait_for(p, in_state, &want, 5000, &snap));
    ZT_EQ_INT(count(&s, ZF_REPORT_START), 0);
    stop_player(p);

    /* never says ready -> killed after the ready timeout and retried */
    p = start_player(&s, "no-ready", &cfg);
    sleep_ms(1600);
    zf_player_snapshot_get(p, &snap);
    ZT_CHECK(strstr(snap.player_error, "ready") || strstr(snap.player_error, "signal 9"));
    stop_player(p);
}

static void osd_before_ready(void)
{
    /* An OSD request made while ZeroPlay is still starting must not be lost. */
    sink s;
    zf_config cfg = base_cfg();
    char logpath[] = "/tmp/zfmocklogXXXXXX";
    int fd = mkstemp(logpath);
    close(fd);
    char flags[128];
    snprintf(flags, sizeof flags, "duration-ms=60000 log=%s", logpath);
    zf_player *p = start_player(&s, flags, &cfg);
    ZT_EQ_INT(zf_player_osd(p, "Zeropfin pairing code: ABCD-EFGH", 120000), 0); /* before ready */
    ZT_CHECK(wait_for(p, is_ready, NULL, 3000, NULL));
    sleep_ms(300);
    char *log = zf_read_file(logpath, 65536, NULL);
    ZT_CHECK(log && strstr(log, " Zeropfin pairing code: ABCD-EFGH\n"));
    free(log);
    stop_player(p);
    unlink(logpath);
}

int main(void)
{
    ZT_RUN(ext_lifecycle);
    ZT_RUN(crash_is_not_completion);
    ZT_RUN(stock_player);
    ZT_RUN(load_failure);
    ZT_RUN(replacement);
    ZT_RUN(osd_before_ready);
    ZT_RUN(start_failure);
    ZT_DONE();
}
