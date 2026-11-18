#include "player.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

extern char **environ;

#define CMDQ_CAP          16
#define PROBE_TIMEOUT_MS  1500
#define STOCK_ASSUME_MS   2000
#define STATUS_POLL_MS    1000
#define QUIT_GRACE_MS     3000
#define TERM_GRACE_MS     2000
#define BACKOFF_MIN_MS    1000
#define BACKOFF_MAX_MS    30000
#define HEALTHY_UPTIME_MS 60000
#define OUTBUF_CAP        8192

typedef enum { C_LOAD, C_PAUSE, C_RESUME, C_SEEK, C_SEEK_REL, C_STOP, C_OSD } cmd_kind;

typedef struct {
    cmd_kind kind;
    int64_t arg;
    zf_media *media;
    char text[128];
} cmd;

struct zf_player {
    zf_config cfg;
    zf_player_hooks hooks;
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t lock;
    int wake_fd;
    cmd q[CMDQ_CAP];
    int qhead, qlen;
    bool shutting_down;

    /* --- owned by the controller thread; mutated under `lock` --- */
    ps_machine ps;
    zf_media media;
    bool has_media;
    bool load_pending; /* media chosen, waiting for the player to be ready */
    zf_proc_state proc;
    pid_t pid;
    int in_fd, out_fd, err_fd, pid_fd;
    bool out_eof;
    zp_linebuf out_lb, err_lb;
    bool ext, osd_supported;
    char pending_osd[128];     /* OSD requested while the player was not ready */
    int64_t pending_osd_until; /* monotonic expiry of that request */
    int64_t proc_deadline;
    int64_t proc_started;
    int64_t backoff_ms;
    int64_t backoff_until;
    int restarts;
    int64_t load_deadline;
    int64_t assume_at;
    int64_t next_status;
    int64_t next_progress;
    int64_t last_status_pos;
    bool have_status_pos;
    int64_t kill_stage_at;
    int kill_stage;
    char player_error[160];
    char outbuf[OUTBUF_CAP];
    size_t outlen;
};

const char *zf_proc_state_name(zf_proc_state s)
{
    switch (s) {
    case ZF_PROC_DOWN: return "down";
    case ZF_PROC_STARTING: return "starting";
    case ZF_PROC_PROBING: return "starting";
    case ZF_PROC_READY: return "ready";
    case ZF_PROC_BACKOFF: return "restarting";
    }
    return "unknown";
}

/* ---- child I/O ------------------------------------------------------------ */

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void flush_out(zf_player *p)
{
    while (p->outlen && p->in_fd >= 0) {
        ssize_t w = write(p->in_fd, p->outbuf, p->outlen);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            /* EPIPE etc.: the child is gone; exit handling follows. */
            LOGW("player stdin write failed: %s", strerror(errno));
            p->outlen = 0;
            close_fd(&p->in_fd);
            return;
        }
        memmove(p->outbuf, p->outbuf + w, p->outlen - (size_t)w);
        p->outlen -= (size_t)w;
    }
}

/* Queue a fully formatted protocol line (must end in '\n'). */
static int send_line(zf_player *p, const char *line)
{
    size_t n = strlen(line);
    if (p->in_fd < 0 || n == 0 || line[n - 1] != '\n')
        return -1;
    if (n > OUTBUF_CAP - p->outlen) {
        /* The player stopped reading stdin: treat as hung. */
        LOGE("player is not consuming commands; restarting it");
        if (p->pid > 0)
            kill(p->pid, SIGKILL);
        return -1;
    }
    memcpy(p->outbuf + p->outlen, line, n);
    p->outlen += n;
    flush_out(p);
    return 0;
}

/* ---- reporting helpers ------------------------------------------------------ */

static void fill_report(zf_player *p, zf_report *r, zf_report_kind kind, int64_t now)
{
    memset(r, 0, sizeof *r);
    r->kind = kind;
    zf_strcpy(r->item_id, sizeof r->item_id, p->media.item_id);
    zf_strcpy(r->media_source_id, sizeof r->media_source_id, p->media.media_source_id);
    zf_strcpy(r->play_session_id, sizeof r->play_session_id, p->media.play_session_id);
    r->audio_stream_index = p->media.audio_stream_index;
    r->subtitle_stream_index = p->media.subtitle_stream_index;
    r->paused = p->ps.state == PS_PAUSED;
    ps_pos_source src = ps_source(&p->ps, now);
    int64_t pos = ps_position(&p->ps, now);
    bool ok = src == POS_VERIFIED || (src == POS_ESTIMATED && p->cfg.report_estimated_position);
    if (ok && zf_ms_to_ticks(pos, &r->position_ticks) == 0)
        r->has_position = true;
}

static void report(zf_player *p, const zf_report *r)
{
    if (p->hooks.report)
        p->hooks.report(p->hooks.ctx, r);
}

static void release_media(zf_player *p)
{
    if (p->has_media && p->hooks.released)
        p->hooks.released(p->hooks.ctx, &p->media);
    p->load_pending = false;
}

/* Finish the active item for `why`, emitting a stop report when playback had
 * actually started. Crash/failed stops never claim completion. */
static void finish_active(zf_player *p, int64_t now, ps_end_reason why, const char *msg)
{
    bool was_started = p->ps.started;
    bool ext_verified = p->ext && p->have_status_pos;
    int64_t crash_pos = p->last_status_pos;
    if (!ps_finish(&p->ps, now, why, msg))
        return;
    if (was_started) {
        zf_report r;
        fill_report(p, &r, ZF_REPORT_STOP, now);
        r.failed = (why == PS_END_CRASH || why == PS_END_LOAD_FAILED || why == PS_END_LOAD_TIMEOUT);
        if (why == PS_END_CRASH) {
            /* Report only the last trustworthy position. */
            r.has_position = false;
            if (ext_verified)
                r.has_position = zf_ms_to_ticks(crash_pos, &r.position_ticks) == 0;
            else if (p->cfg.report_estimated_position)
                r.has_position = zf_ms_to_ticks(ps_position(&p->ps, now), &r.position_ticks) == 0;
        }
        report(p, &r);
    }
    release_media(p);
}

/* ---- process lifecycle ------------------------------------------------------ */

static int spawn_child(zf_player *p)
{
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    if (access(p->cfg.zeroplay_path, X_OK) < 0) {
        zf_fmt(p->player_error, sizeof p->player_error, "ZeroPlay not executable at %s", p->cfg.zeroplay_path);
        LOGE("%s: %s", p->player_error, strerror(errno));
        return -1;
    }
    if (pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0 || pipe2(err, O_CLOEXEC) < 0) {
        LOGE("pipe2: %s", strerror(errno));
        goto fail;
    }
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    sigset_t empty, defs;
    sigemptyset(&empty);
    sigemptyset(&defs);
    sigaddset(&defs, SIGPIPE);
    sigaddset(&defs, SIGINT);
    sigaddset(&defs, SIGTERM);
    sigaddset(&defs, SIGHUP);
    sigaddset(&defs, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &defs);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

    char *argv[ZF_MAX_PLAYER_ARGS + 4];
    int ac = 0;
    argv[ac++] = p->cfg.zeroplay_path;
    argv[ac++] = (char *)(uintptr_t)"--control";
    for (int i = 0; i < p->cfg.n_zeroplay_args; i++)
        argv[ac++] = p->cfg.zeroplay_args[i];
    argv[ac] = NULL;

    pid_t pid;
    int rc = posix_spawn(&pid, p->cfg.zeroplay_path, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        zf_fmt(p->player_error, sizeof p->player_error, "Could not start ZeroPlay: %s", strerror(rc));
        LOGE("%s", p->player_error);
        goto fail;
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    p->pid = pid;
    p->in_fd = in[1];
    p->out_fd = out[0];
    p->err_fd = err[0];
    p->out_eof = false;
    fcntl(p->in_fd, F_SETFL, fcntl(p->in_fd, F_GETFL) | O_NONBLOCK);
    fcntl(p->out_fd, F_SETFL, fcntl(p->out_fd, F_GETFL) | O_NONBLOCK);
    fcntl(p->err_fd, F_SETFL, fcntl(p->err_fd, F_GETFL) | O_NONBLOCK);
#ifdef SYS_pidfd_open
    p->pid_fd = (int)syscall(SYS_pidfd_open, pid, 0);
#else
    p->pid_fd = -1;
#endif
    zp_linebuf_init(&p->out_lb);
    zp_linebuf_init(&p->err_lb);
    p->outlen = 0;
    p->ext = false;
    p->osd_supported = false;
    p->proc = ZF_PROC_STARTING;
    p->proc_started = zf_mono_ms();
    p->proc_deadline = p->proc_started + (int64_t)p->cfg.player_ready_timeout_s * 1000;
    p->kill_stage = 0;
    LOGI("started ZeroPlay (pid %ld)", (long)pid);
    return 0;
fail:
    for (int i = 0; i < 2; i++) {
        if (in[i] >= 0)
            close(in[i]);
        if (out[i] >= 0)
            close(out[i]);
        if (err[i] >= 0)
            close(err[i]);
    }
    return -1;
}

static void enter_backoff(zf_player *p, int64_t now)
{
    if (p->shutting_down) {
        p->proc = ZF_PROC_DOWN;
        return;
    }
    if (p->proc_started && now - p->proc_started > HEALTHY_UPTIME_MS)
        p->backoff_ms = BACKOFF_MIN_MS;
    else
        p->backoff_ms = p->backoff_ms ? p->backoff_ms * 2 : BACKOFF_MIN_MS;
    if (p->backoff_ms > BACKOFF_MAX_MS)
        p->backoff_ms = BACKOFF_MAX_MS;
    p->backoff_until = now + p->backoff_ms;
    p->proc = ZF_PROC_BACKOFF;
}

static void on_child_exit(zf_player *p, int status)
{
    int64_t now = zf_mono_ms();
    if (WIFEXITED(status))
        zf_fmt(p->player_error, sizeof p->player_error, "ZeroPlay exited with status %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        zf_fmt(p->player_error, sizeof p->player_error, "ZeroPlay was killed by signal %d", WTERMSIG(status));
    if (!p->shutting_down)
        LOGW("%s", p->player_error);
    else
        p->player_error[0] = '\0';
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    close_fd(&p->err_fd);
    close_fd(&p->pid_fd);
    p->pid = -1;
    p->outlen = 0;
    if (ps_is_active(&p->ps)) {
        finish_active(p, now, p->shutting_down ? PS_END_SHUTDOWN : PS_END_CRASH, "The player stopped unexpectedly");
    }
    if (!p->shutting_down)
        p->restarts++;
    enter_backoff(p, now);
}

static void try_reap(zf_player *p)
{
    if (p->pid <= 0)
        return;
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid)
        on_child_exit(p, status);
}

/* ---- protocol actions -------------------------------------------------------- */

static void write_load(zf_player *p, int64_t now)
{
    char line[ZP_MAX_URL + 64];
    if (zp_fmt_load(line, sizeof line, p->media.url, p->media.start_ms, p->ext) < 0) {
        ps_on_load_error(&p->ps, now, PS_END_LOAD_FAILED, "Invalid media URL");
        release_media(p);
        return;
    }
    send_line(p, line);
    if (!p->ext && p->media.start_ms > 0) {
        /* Stock ZeroPlay: `load` then `seek` in the same read batch. */
        char seek[64];
        if (zp_fmt_seek(seek, sizeof seek, p->media.start_ms) == 0)
            send_line(p, seek);
    }
    p->load_pending = false;
    p->load_deadline = now + (int64_t)p->cfg.player_load_timeout_s * 1000;
    p->assume_at = p->ext ? 0 : now + STOCK_ASSUME_MS;
    p->have_status_pos = false;
}

static void on_started(zf_player *p, int64_t now)
{
    zf_report r;
    fill_report(p, &r, ZF_REPORT_START, now);
    report(p, &r);
    p->next_progress = now + (int64_t)p->cfg.progress_interval_s * 1000;
    p->next_status = now + STATUS_POLL_MS;
}

static void on_player_ready(zf_player *p, int64_t now)
{
    p->proc = ZF_PROC_READY;
    p->player_error[0] = '\0';
    LOGI("ZeroPlay ready (%s)", p->ext ? "status extensions available" : "stock control protocol");
    if (p->load_pending && p->ps.state == PS_LOADING) {
        /* The generation was created before the player was ready; reconcile
         * its extension mode with what the player actually supports. */
        p->ps.ext = p->ext;
        p->ps.awaiting_loaded = p->ext;
        p->ps.quarantine_until = p->ext ? 0 : now + PS_STOCK_QUARANTINE_MS;
        write_load(p, now);
    }
    /* Show an OSD message (e.g. a pairing code) that arrived during start-up. */
    if (p->pending_osd[0] && now < p->pending_osd_until) {
        char line[160];
        if (zp_fmt_osd(line, sizeof line, (int)(p->pending_osd_until - now), p->pending_osd) == 0)
            send_line(p, line);
    }
    p->pending_osd[0] = '\0';
}

static void progress_event(zf_player *p, int64_t now, const char *event)
{
    if (!p->ps.started)
        return;
    zf_report r;
    fill_report(p, &r, ZF_REPORT_PROGRESS, now);
    r.event_name = event;
    report(p, &r);
    p->next_progress = now + (int64_t)p->cfg.progress_interval_s * 1000;
}

static void on_out_line(void *ctx, const char *line)
{
    zf_player *p = ctx;
    int64_t now = zf_mono_ms();
    zp_event ev;
    zp_parse_event(line, &ev);
    switch (ev.type) {
    case ZP_EV_READY:
        if (p->proc == ZF_PROC_STARTING) {
            p->proc = ZF_PROC_PROBING;
            p->proc_deadline = now + PROBE_TIMEOUT_MS;
            send_line(p, "events on\n");
        }
        break;
    case ZP_EV_EVENTS_ACK:
        if (p->proc == ZF_PROC_PROBING) {
            p->ext = ev.events_on;
            on_player_ready(p, now);
        }
        break;
    case ZP_EV_LOADED:
        if (ps_on_loaded(&p->ps, now, ev.dur_ms))
            on_started(p, now);
        break;
    case ZP_EV_LOAD_ERROR:
        if (ps_on_load_error(&p->ps, now, PS_END_LOAD_FAILED,
                             "The player could not open this media (codec, container or network problem)"))
            release_media(p);
        break;
    case ZP_EV_ENDED:
        if (ps_on_ended(&p->ps, now)) {
            zf_report r;
            fill_report(p, &r, ZF_REPORT_STOP, now);
            /* Real EOF: report the full runtime so Jellyfin marks it played. */
            int64_t end_ms = p->media.runtime_ms > 0 ? p->media.runtime_ms : p->ps.duration_ms;
            r.has_position = end_ms > 0 && zf_ms_to_ticks(end_ms, &r.position_ticks) == 0;
            r.paused = false;
            report(p, &r);
            release_media(p);
        } else {
            LOGD("ignored stale 'ended' event");
        }
        break;
    case ZP_EV_STATUS:
        if (ps_on_status(&p->ps, now, (int)ev.state, ev.pos_ms, ev.dur_ms)) {
            p->last_status_pos = ev.pos_ms;
            p->have_status_pos = true;
        }
        break;
    case ZP_EV_OSD_OK: p->osd_supported = true; break;
    case ZP_EV_UNKNOWN: LOGD("ignored player output line"); break;
    }
}

static void on_err_line(void *ctx, const char *line)
{
    zf_player *p = ctx;
    char clean[512];
    zf_scrub_media_ids(line, clean, sizeof clean);
    LOGI("zeroplay: %s", clean);
    int64_t now = zf_mono_ms();
    if (!p->ext && zp_stderr_is_load_failure(line) && now <= p->load_deadline) {
        bool was_started = p->ps.started;
        if (ps_on_load_error(&p->ps, now, PS_END_LOAD_FAILED,
                             "The player could not open this media (codec, container or network problem)")) {
            if (was_started) {
                zf_report r;
                fill_report(p, &r, ZF_REPORT_STOP, now);
                r.failed = true;
                r.has_position = false;
                report(p, &r);
            }
            release_media(p);
        }
    }
}

static void drain(int *fd, zp_linebuf *lb, zp_line_cb cb, zf_player *p, bool *eof)
{
    char buf[4096];
    for (int iter = 0; iter < 16 && *fd >= 0; iter++) {
        ssize_t r = read(*fd, buf, sizeof buf);
        if (r > 0) {
            zp_linebuf_feed(lb, buf, (size_t)r, cb, p);
            continue;
        }
        if (r < 0 && (errno == EINTR))
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        close_fd(fd);
        if (eof)
            *eof = true;
        return;
    }
}

/* ---- commands ------------------------------------------------------------------- */

static void do_load(zf_player *p, zf_media *m, int64_t now)
{
    if (ps_is_active(&p->ps)) {
        finish_active(p, now, PS_END_REPLACED, NULL);
        if (p->proc == ZF_PROC_READY)
            send_line(p, "stop\n");
    }
    p->media = *m;
    p->has_media = true;
    bool ext = p->proc == ZF_PROC_READY ? p->ext : false;
    ps_load(&p->ps, now, m->start_ms, m->runtime_ms, ext);
    p->load_pending = true;
    if (p->proc == ZF_PROC_READY) {
        write_load(p, now);
    } else {
        /* Wait for (re)start; bound the wait by the ready+load timeouts. */
        p->load_deadline = now + (int64_t)(p->cfg.player_ready_timeout_s + p->cfg.player_load_timeout_s) * 1000;
        if (p->proc == ZF_PROC_BACKOFF || p->proc == ZF_PROC_DOWN)
            p->backoff_until = now; /* start right away */
    }
}

static void exec_cmd(zf_player *p, cmd *c, int64_t now)
{
    char line[160];
    switch (c->kind) {
    case C_LOAD:
        do_load(p, c->media, now);
        free(c->media);
        c->media = NULL;
        break;
    case C_PAUSE:
        if (ps_pause(&p->ps, now) == 0 && p->proc == ZF_PROC_READY) {
            send_line(p, "pause\n");
            progress_event(p, now, "Pause");
        }
        break;
    case C_RESUME:
        if (ps_resume(&p->ps, now) == 0 && p->proc == ZF_PROC_READY) {
            send_line(p, "resume\n");
            progress_event(p, now, "Unpause");
        }
        break;
    case C_SEEK_REL:
    case C_SEEK: {
        int64_t target = c->arg;
        if (c->kind == C_SEEK_REL) {
            target = ps_position(&p->ps, now) + c->arg;
            if (target < 0)
                target = 0;
        }
        if (p->ps.duration_ms > 0 && target > p->ps.duration_ms)
            target = p->ps.duration_ms;
        if (ps_seek(&p->ps, now, target) == 0 && p->proc == ZF_PROC_READY &&
            zp_fmt_seek(line, sizeof line, target) == 0) {
            send_line(p, line);
            p->have_status_pos = false;
            progress_event(p, now, "TimeUpdate");
        }
        break;
    }
    case C_STOP:
        if (ps_is_active(&p->ps)) {
            finish_active(p, now, PS_END_USER_STOP, NULL);
            if (p->proc == ZF_PROC_READY)
                send_line(p, "stop\n");
        }
        break;
    case C_OSD:
        if (p->proc == ZF_PROC_READY) {
            if (zp_fmt_osd(line, sizeof line, (int)c->arg, c->text) == 0)
                send_line(p, line);
        } else {
            /* Not ready yet (starting or restarting): show it once ready. */
            zf_strcpy(p->pending_osd, sizeof p->pending_osd, c->text);
            p->pending_osd_until = now + c->arg;
        }
        break;
    }
}

static int submit(zf_player *p, cmd *c)
{
    pthread_mutex_lock(&p->lock);
    if (p->qlen >= CMDQ_CAP || p->shutting_down) {
        pthread_mutex_unlock(&p->lock);
        return -2;
    }
    p->q[(p->qhead + p->qlen) % CMDQ_CAP] = *c;
    p->qlen++;
    pthread_mutex_unlock(&p->lock);
    uint64_t one = 1;
    ssize_t w = write(p->wake_fd, &one, sizeof one);
    (void)w;
    return 0;
}

int zf_player_load(zf_player *p, const zf_media *m)
{
    if (!zp_url_ok(m->url))
        return -1;
    cmd c = {.kind = C_LOAD};
    c.media = malloc(sizeof *m);
    if (!c.media)
        return -2;
    *c.media = *m;
    int rc = submit(p, &c);
    if (rc < 0)
        free(c.media);
    return rc;
}

static bool state_in(zf_player *p, ps_state a, ps_state b)
{
    pthread_mutex_lock(&p->lock);
    bool ok = p->ps.state == a || p->ps.state == b;
    pthread_mutex_unlock(&p->lock);
    return ok;
}

int zf_player_pause(zf_player *p)
{
    if (!state_in(p, PS_PLAYING, PS_PAUSED))
        return -1;
    cmd c = {.kind = C_PAUSE};
    return submit(p, &c);
}

int zf_player_resume(zf_player *p)
{
    if (!state_in(p, PS_PLAYING, PS_PAUSED))
        return -1;
    cmd c = {.kind = C_RESUME};
    return submit(p, &c);
}

int zf_player_seek(zf_player *p, int64_t pos_ms)
{
    if (pos_ms < 0 || pos_ms > INT64_MAX / ZF_TICKS_PER_MS || !state_in(p, PS_PLAYING, PS_PAUSED))
        return -1;
    cmd c = {.kind = C_SEEK, .arg = pos_ms};
    return submit(p, &c);
}

int zf_player_seek_relative(zf_player *p, int64_t delta_ms)
{
    if (delta_ms < -86400000LL || delta_ms > 86400000LL || !state_in(p, PS_PLAYING, PS_PAUSED))
        return -1;
    cmd c = {.kind = C_SEEK_REL, .arg = delta_ms};
    return submit(p, &c);
}

int zf_player_stop(zf_player *p)
{
    cmd c = {.kind = C_STOP};
    return submit(p, &c);
}

int zf_player_osd(zf_player *p, const char *text, int duration_ms)
{
    cmd c = {.kind = C_OSD, .arg = duration_ms};
    if (zf_strcpy(c.text, sizeof c.text, text) < 0)
        return -1;
    return submit(p, &c);
}

/* ---- controller thread ------------------------------------------------------------- */

static int64_t min_deadline(int64_t a, int64_t b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    return a < b ? a : b;
}

static void run_timers(zf_player *p, int64_t now)
{
    /* process supervision */
    if ((p->proc == ZF_PROC_STARTING) && now >= p->proc_deadline) {
        zf_fmt(p->player_error, sizeof p->player_error, "ZeroPlay did not report ready within %ds",
               p->cfg.player_ready_timeout_s);
        LOGE("%s", p->player_error);
        if (p->pid > 0)
            kill(p->pid, SIGKILL);
        p->proc_deadline = 0;
    }
    if (p->proc == ZF_PROC_PROBING && now >= p->proc_deadline) {
        p->ext = false;
        on_player_ready(p, now);
    }
    if ((p->proc == ZF_PROC_BACKOFF || p->proc == ZF_PROC_DOWN) && !p->shutting_down && now >= p->backoff_until) {
        if (spawn_child(p) < 0)
            enter_backoff(p, now);
    }
    /* load supervision */
    if (p->ps.state == PS_LOADING && p->load_deadline && now >= p->load_deadline) {
        const char *msg = p->load_pending ? "The player is not available" : "The player did not start playback in time";
        if (ps_on_load_error(&p->ps, now, PS_END_LOAD_TIMEOUT, msg) ||
            ps_finish(&p->ps, now, PS_END_LOAD_TIMEOUT, msg)) {
            if (p->proc == ZF_PROC_READY && !p->load_pending)
                send_line(p, "stop\n");
            release_media(p);
        }
    }
    if (p->ps.state == PS_LOADING && p->assume_at && now >= p->assume_at) {
        p->assume_at = 0;
        if (ps_assume_started(&p->ps, now))
            on_started(p, now);
    }
    /* telemetry */
    if ((p->ps.state == PS_PLAYING || p->ps.state == PS_PAUSED)) {
        if (p->ext && p->proc == ZF_PROC_READY && now >= p->next_status) {
            send_line(p, "status\n");
            p->next_status = now + STATUS_POLL_MS;
        }
        if (now >= p->next_progress)
            progress_event(p, now, "TimeUpdate");
    }
    /* shutdown escalation */
    if (p->shutting_down && p->pid > 0 && p->kill_stage_at && now >= p->kill_stage_at) {
        if (p->kill_stage == 0) {
            LOGW("ZeroPlay did not quit; sending SIGTERM");
            kill(p->pid, SIGTERM);
            p->kill_stage = 1;
            p->kill_stage_at = now + TERM_GRACE_MS;
        } else {
            LOGW("ZeroPlay did not terminate; sending SIGKILL");
            kill(p->pid, SIGKILL);
            p->kill_stage_at = 0;
        }
    }
}

static int64_t next_deadline(zf_player *p)
{
    int64_t d = 0;
    if (p->proc == ZF_PROC_STARTING || p->proc == ZF_PROC_PROBING)
        d = min_deadline(d, p->proc_deadline);
    if ((p->proc == ZF_PROC_BACKOFF || p->proc == ZF_PROC_DOWN) && !p->shutting_down)
        d = min_deadline(d, p->backoff_until ? p->backoff_until : 1);
    if (p->ps.state == PS_LOADING) {
        d = min_deadline(d, p->load_deadline);
        d = min_deadline(d, p->assume_at);
    }
    if (p->ps.state == PS_PLAYING || p->ps.state == PS_PAUSED) {
        if (p->ext)
            d = min_deadline(d, p->next_status);
        d = min_deadline(d, p->next_progress);
    }
    if (p->shutting_down)
        d = min_deadline(d, p->kill_stage_at);
    if (p->pid > 0 && p->pid_fd < 0 && p->out_eof)
        d = min_deadline(d, zf_mono_ms() + 100); /* poll waitpid without pidfd */
    return d;
}

static void begin_shutdown_locked(zf_player *p, int64_t now)
{
    if (ps_is_active(&p->ps))
        finish_active(p, now, PS_END_SHUTDOWN, NULL);
    if (p->pid > 0) {
        if (p->proc == ZF_PROC_READY || p->proc == ZF_PROC_PROBING) {
            send_line(p, "stop\n");
            send_line(p, "quit\n");
        }
        close_fd(&p->in_fd); /* EOF on stdin also makes ZeroPlay exit */
        p->kill_stage = 0;
        p->kill_stage_at = now + QUIT_GRACE_MS;
    } else {
        p->proc = ZF_PROC_DOWN;
    }
}

static void *thread_main(void *arg)
{
    zf_player *p = arg;
    bool shutdown_started = false;
    pthread_mutex_lock(&p->lock);
    p->backoff_until = zf_mono_ms();
    for (;;) {
        int64_t now = zf_mono_ms();
        if (p->shutting_down && !shutdown_started) {
            shutdown_started = true;
            begin_shutdown_locked(p, now);
        }
        if (shutdown_started && p->pid <= 0)
            break;

        struct pollfd fds[5];
        int nf = 0;
        fds[nf++] = (struct pollfd){.fd = p->wake_fd, .events = POLLIN};
        int i_out = -1, i_err = -1, i_pid = -1, i_in = -1;
        if (p->out_fd >= 0) {
            i_out = nf;
            fds[nf++] = (struct pollfd){.fd = p->out_fd, .events = POLLIN};
        }
        if (p->err_fd >= 0) {
            i_err = nf;
            fds[nf++] = (struct pollfd){.fd = p->err_fd, .events = POLLIN};
        }
        if (p->pid_fd >= 0) {
            i_pid = nf;
            fds[nf++] = (struct pollfd){.fd = p->pid_fd, .events = POLLIN};
        }
        if (p->in_fd >= 0 && p->outlen) {
            i_in = nf;
            fds[nf++] = (struct pollfd){.fd = p->in_fd, .events = POLLOUT};
        }
        int64_t dl = next_deadline(p);
        int timeout = -1;
        if (dl) {
            int64_t t = dl - now;
            timeout = t <= 0 ? 0 : (t > 60000 ? 60000 : (int)t);
        }
        pthread_mutex_unlock(&p->lock);
        int pr = poll(fds, (nfds_t)nf, timeout);
        pthread_mutex_lock(&p->lock);
        now = zf_mono_ms();
        if (pr < 0 && errno != EINTR) {
            LOGE("poll: %s", strerror(errno));
            break;
        }
        if (pr > 0 && (fds[0].revents & POLLIN)) {
            uint64_t v;
            ssize_t rr = read(p->wake_fd, &v, sizeof v);
            (void)rr;
        }
        /* Commands (even during shutdown we drain so media is freed). */
        while (p->qlen) {
            cmd c = p->q[p->qhead];
            p->qhead = (p->qhead + 1) % CMDQ_CAP;
            p->qlen--;
            if (shutdown_started) {
                free(c.media);
                continue;
            }
            exec_cmd(p, &c, now);
        }
        if (pr > 0) {
            if (i_out >= 0 && (fds[i_out].revents & (POLLIN | POLLHUP | POLLERR)))
                drain(&p->out_fd, &p->out_lb, on_out_line, p, &p->out_eof);
            if (i_err >= 0 && (fds[i_err].revents & (POLLIN | POLLHUP | POLLERR)))
                drain(&p->err_fd, &p->err_lb, on_err_line, p, NULL);
            if (i_in >= 0 && (fds[i_in].revents & (POLLOUT | POLLERR | POLLHUP)))
                flush_out(p);
            if (i_pid >= 0 && (fds[i_pid].revents & POLLIN)) {
                /* Drain remaining output first so a final `ended` is seen. */
                if (p->out_fd >= 0)
                    drain(&p->out_fd, &p->out_lb, on_out_line, p, &p->out_eof);
                if (p->err_fd >= 0)
                    drain(&p->err_fd, &p->err_lb, on_err_line, p, NULL);
                try_reap(p);
            }
        }
        if (p->pid > 0 && p->pid_fd < 0 && (p->out_eof || shutdown_started))
            try_reap(p);
        run_timers(p, zf_mono_ms());
    }
    p->proc = ZF_PROC_DOWN;
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

/* ---- public lifecycle ---------------------------------------------------------------- */

zf_player *zf_player_new(const zf_config *cfg, zf_player_hooks hooks)
{
    zf_player *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->cfg = *cfg;
    p->hooks = hooks;
    pthread_mutex_init(&p->lock, NULL);
    p->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    p->in_fd = p->out_fd = p->err_fd = p->pid_fd = -1;
    p->pid = -1;
    p->proc = ZF_PROC_DOWN;
    ps_init(&p->ps);
    if (p->wake_fd < 0) {
        free(p);
        return NULL;
    }
    return p;
}

int zf_player_start(zf_player *p)
{
    if (pthread_create(&p->thread, NULL, thread_main, p) != 0)
        return -1;
    p->thread_started = true;
    return 0;
}

void zf_player_shutdown(zf_player *p)
{
    if (!p || !p->thread_started)
        return;
    pthread_mutex_lock(&p->lock);
    p->shutting_down = true;
    pthread_mutex_unlock(&p->lock);
    uint64_t one = 1;
    ssize_t w = write(p->wake_fd, &one, sizeof one);
    (void)w;
    pthread_join(p->thread, NULL);
    p->thread_started = false;
}

void zf_player_free(zf_player *p)
{
    if (!p)
        return;
    for (int i = 0; i < p->qlen; i++)
        free(p->q[(p->qhead + i) % CMDQ_CAP].media);
    close_fd(&p->wake_fd);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

void zf_player_snapshot_get(zf_player *p, zf_player_snapshot *out)
{
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&p->lock);
    int64_t now = zf_mono_ms();
    out->state = p->ps.state;
    out->proc = p->proc;
    out->ext_events = p->ext;
    out->osd_supported = p->osd_supported;
    out->generation = p->ps.generation;
    out->position_ms = ps_position(&p->ps, now);
    out->duration_ms = p->ps.duration_ms;
    out->source = ps_source(&p->ps, now);
    out->has_media = p->has_media && p->ps.state != PS_IDLE;
    if (out->has_media) {
        out->media = p->media;
        out->media.url[0] = '\0';
        out->media.relay_id[0] = '\0';
    }
    zf_strcpy(out->error, sizeof out->error, p->ps.error);
    zf_strcpy(out->player_error, sizeof out->player_error, p->player_error);
    out->restarts = p->restarts;
    pthread_mutex_unlock(&p->lock);
}
