#include "ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "api.h"

struct zf_ctl {
    zf_app *app;
    int fd;
    int wake[2];
    pthread_t thread;
    char path[108];
};

static void reply(int fd, bool ok, const char *text)
{
    char head[8];
    zf_fmt(head, sizeof head, "%s\n", ok ? "ok" : "error");
    ssize_t w = send(fd, head, strlen(head), MSG_NOSIGNAL);
    if (w > 0 && text)
        w = send(fd, text, strlen(text), MSG_NOSIGNAL);
    (void)w;
}

static void fmt_time(int64_t wall_ms, char *out, size_t cap)
{
    time_t t = (time_t)(wall_ms / 1000);
    struct tm tm;
    if (!localtime_r(&t, &tm) || !strftime(out, cap, "%Y-%m-%d %H:%M", &tm))
        zf_strcpy(out, cap, "?");
}

static void handle(zf_ctl *c, int fd, char *line)
{
    zf_app *app = c->app;
    char out[4096];
    if (!strcmp(line, "pair")) {
        char code[16];
        if (zf_auth_new_code(app->auth, zf_mono_ms(), true, code) != ZF_AUTH_OK) {
            reply(fd, false, "could not create a pairing code\n");
            return;
        }
        LOGI("pairing code issued via the control socket");
        zf_fmt(out, sizeof out, "Pairing code: %s (valid for 10 minutes, single use)\n", code);
        reply(fd, true, out);
    } else if (!strcmp(line, "sessions")) {
        cJSON *arr = zf_auth_list(app->auth);
        size_t n = 0;
        out[0] = '\0';
        const cJSON *s;
        if (cJSON_GetArraySize(arr) == 0)
            zf_strcpy(out, sizeof out, "No paired remotes.\n");
        cJSON_ArrayForEach(s, arr)
        {
            char created[32], expires[32];
            fmt_time((int64_t)cJSON_GetObjectItem(s, "created")->valuedouble, created, sizeof created);
            fmt_time((int64_t)cJSON_GetObjectItem(s, "expires")->valuedouble, expires, sizeof expires);
            int w = snprintf(out + n, sizeof out - n, "%s  %-24.24s  paired %s  expires %s\n",
                             cJSON_GetObjectItem(s, "id")->valuestring, cJSON_GetObjectItem(s, "name")->valuestring,
                             created, expires);
            if (w < 0 || (size_t)w >= sizeof out - n)
                break;
            n += (size_t)w;
        }
        cJSON_Delete(arr);
        reply(fd, true, out);
    } else if (!strncmp(line, "revoke ", 7)) {
        int n = zf_auth_revoke(app->auth, line + 7);
        zf_fmt(out, sizeof out, "Revoked %d session(s).\n", n);
        reply(fd, n > 0, out);
    } else if (!strcmp(line, "reload")) {
        if (zf_app_reload_creds(app) == 0)
            reply(fd, true, "Reloaded Jellyfin credentials.\n");
        else
            reply(fd, false, "Could not load credentials; see the service log.\n");
    } else if (!strcmp(line, "status")) {
        zf_player_snapshot s;
        zf_player_snapshot_get(app->player, &s);
        cJSON *o = zf_api_player_json(&s);
        bool reachable;
        char err[160];
        zf_jf_get_health(app->jf, &reachable, err, sizeof err);
        cJSON *jf = cJSON_AddObjectToObject(o, "jellyfin");
        cJSON_AddBoolToObject(jf, "configured", zf_jf_configured(app->jf));
        cJSON_AddBoolToObject(jf, "reachable", reachable);
        if (err[0])
            cJSON_AddStringToObject(jf, "error", err);
        unsigned readers, resumes;
        unsigned long long bytes;
        zf_relay_stats(app->relay, &readers, &bytes, &resumes);
        cJSON *rl = cJSON_AddObjectToObject(o, "relay");
        cJSON_AddNumberToObject(rl, "port", zf_relay_port(app->relay));
        cJSON_AddNumberToObject(rl, "activeReaders", readers);
        cJSON_AddNumberToObject(rl, "bytesOut", (double)bytes);
        cJSON_AddNumberToObject(rl, "resumes", resumes);
        unsigned sent, failed, dropped;
        zf_reporter_stats(app->reporter, &sent, &failed, &dropped);
        cJSON *rp = cJSON_AddObjectToObject(o, "reports");
        cJSON_AddNumberToObject(rp, "sent", sent);
        cJSON_AddNumberToObject(rp, "failed", failed);
        cJSON_AddNumberToObject(rp, "dropped", dropped);
        char *text = cJSON_Print(o);
        cJSON_Delete(o);
        if (text) {
            zf_fmt(out, sizeof out, "%s\n", text);
            free(text);
        }
        reply(fd, true, out);
    } else {
        reply(fd, false, "unknown command (pair | sessions | revoke <id|all> | reload | status)\n");
    }
}

static void *thread_main(void *arg)
{
    zf_ctl *c = arg;
    for (;;) {
        struct pollfd fds[2] = {{.fd = c->fd, .events = POLLIN}, {.fd = c->wake[0], .events = POLLIN}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents)
            break;
        if (!(fds[0].revents & POLLIN))
            continue;
        int fd = accept4(c->fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        struct ucred cred;
        socklen_t len = sizeof cred;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || (cred.uid != 0 && cred.uid != geteuid())) {
            reply(fd, false, "permission denied\n");
            close(fd);
            continue;
        }
        struct timeval tv = {.tv_sec = 2};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char line[256];
        size_t n = 0;
        while (n < sizeof line - 1) {
            ssize_t r = recv(fd, line + n, sizeof line - 1 - n, 0);
            if (r <= 0)
                break;
            n += (size_t)r;
            if (memchr(line, '\n', n))
                break;
        }
        line[n] = '\0';
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
            if (nl > line && nl[-1] == '\r')
                nl[-1] = '\0';
            if (!zf_has_ctl(line))
                handle(c, fd, line);
        }
        close(fd);
    }
    return NULL;
}

zf_ctl *zf_ctl_start(zf_app *app)
{
    zf_ctl *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->app = app;
    c->fd = -1;
    c->wake[0] = c->wake[1] = -1;
    if (zf_fmt(c->path, sizeof c->path, "%s/control.sock", app->cfg.runtime_dir) < 0) {
        LOGE("runtime_dir path is too long for a unix socket");
        free(c);
        return NULL;
    }
    struct stat st;
    if (stat(app->cfg.runtime_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        LOGW("runtime directory %s missing; admin commands (zeropfin pair) are unavailable", app->cfg.runtime_dir);
        free(c);
        return NULL;
    }
    c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    zf_strcpy(sa.sun_path, sizeof sa.sun_path, c->path);
    unlink(c->path);
    mode_t old = umask(0177);
    int brc = c->fd >= 0 ? bind(c->fd, (struct sockaddr *)&sa, sizeof sa) : -1;
    umask(old);
    if (brc < 0 || chmod(c->path, 0600) < 0 || listen(c->fd, 4) < 0 || pipe2(c->wake, O_CLOEXEC) < 0) {
        LOGE("control socket %s: %s", c->path, strerror(errno));
        if (c->fd >= 0)
            close(c->fd);
        free(c);
        return NULL;
    }
    if (pthread_create(&c->thread, NULL, thread_main, c) != 0) {
        close(c->fd);
        close(c->wake[0]);
        close(c->wake[1]);
        free(c);
        return NULL;
    }
    return c;
}

void zf_ctl_stop(zf_ctl *c)
{
    if (!c)
        return;
    ssize_t w = write(c->wake[1], "x", 1);
    (void)w;
    pthread_join(c->thread, NULL);
    close(c->fd);
    close(c->wake[0]);
    close(c->wake[1]);
    unlink(c->path);
    free(c);
}

int zf_ctl_client(const char *runtime_dir, const char *command, FILE *out, FILE *err)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (zf_fmt(sa.sun_path, sizeof sa.sun_path, "%s/control.sock", runtime_dir) < 0) {
        fprintf(err, "zeropfin: runtime directory path too long\n");
        return 2;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(err,
                "zeropfin: cannot reach the running service at %s: %s\n"
                "Is zeropfin.service running, and are you the zeropfin user (sudo -u zeropfin) or root?\n",
                sa.sun_path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return 2;
    }
    char line[300];
    if (zf_fmt(line, sizeof line, "%s\n", command) < 0 || send(fd, line, strlen(line), MSG_NOSIGNAL) < 0) {
        close(fd);
        return 2;
    }
    char buf[4096];
    size_t n = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + n, sizeof buf - 1 - n, 0);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n >= sizeof buf - 1)
            break;
    }
    close(fd);
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (!nl) {
        fprintf(err, "zeropfin: no reply from the service\n");
        return 2;
    }
    *nl = '\0';
    bool ok = !strcmp(buf, "ok");
    fputs(nl + 1, ok ? out : err);
    return ok ? 0 : 1;
}
