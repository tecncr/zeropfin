#include "app.h"

#include <curl/curl.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ctl.h"
#include "http.h"
#include "version.h"

/* Minimal sd_notify(3) without linking libsystemd. */
static void sd_notify_msg(const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || (path[0] != '/' && path[0] != '@') || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path));
    if (sa.sun_path[0] == '@')
        sa.sun_path[0] = '\0';
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path));
    ssize_t w = sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa, len);
    (void)w;
    close(fd);
}

static void hook_report(void *ctx, const zf_report *r)
{
    zf_app *app = ctx;
    zf_reporter_submit(app->reporter, r);
}

static void hook_released(void *ctx, const zf_media *m)
{
    zf_app *app = ctx;
    zf_relay_revoke(app->relay, m->relay_id);
}

int zf_app_reload_creds(zf_app *app)
{
    zf_creds c;
    int rc = zf_creds_load(app->cfg.state_dir, &c);
    if (rc < 0)
        return -1;
    zf_jf_set_creds(app->jf, &c);
    bool ok = zf_creds_configured(&c);
    memset(&c, 0, sizeof c);
    LOGI("Jellyfin credentials %s", ok ? "loaded" : "not configured");
    return 0;
}

static void teardown(zf_app *app)
{
    sd_notify_msg("STOPPING=1");
    if (app->ctl)
        zf_ctl_stop(app->ctl);
    if (app->http)
        zf_http_quiesce(app->http);
    if (app->player)
        zf_player_shutdown(app->player); /* emits the final stop report */
    if (app->workers)
        zf_workers_shutdown(app->workers); /* cancelled jobs resume their connections */
    if (app->http)
        zf_http_stop(app->http);
    if (app->reporter)
        zf_reporter_shutdown(app->reporter, 4000);
    if (app->relay)
        zf_relay_stop(app->relay);
    zf_player_free(app->player);
    zf_workers_free(app->workers);
    zf_reporter_free(app->reporter);
    zf_relay_free(app->relay);
    zf_jf_free(app->jf);
    zf_auth_free(app->auth);
    curl_global_cleanup();
}

int zf_serve(const zf_config *cfg)
{
    if (geteuid() == 0) {
        LOGE("refusing to run as root; use the dedicated zeropfin service user");
        return 2;
    }
    zf_app app;
    memset(&app, 0, sizeof app);
    app.cfg = *cfg;
    app.started_mono = zf_mono_ms();
    if (gethostname(app.hostname, sizeof app.hostname - 1) < 0)
        app.hostname[0] = '\0';

    /* All threads inherit this mask; signals are taken synchronously below. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        LOGE("libcurl initialisation failed");
        return 1;
    }
    struct stat st;
    if (stat(cfg->state_dir, &st) < 0 || !S_ISDIR(st.st_mode))
        LOGW("state directory %s does not exist; pairing and setup will not persist", cfg->state_dir);

    zf_creds creds;
    int crc = zf_creds_load(cfg->state_dir, &creds);
    if (crc < 0)
        LOGE("could not load Jellyfin credentials; browsing is unavailable until fixed");

    app.auth = zf_auth_new(cfg->state_dir, cfg->session_days);
    app.jf = zf_jf_new(cfg, crc == 0 ? &creds : NULL);
    bool configured = crc == 0 && zf_creds_configured(&creds);
    memset(&creds, 0, sizeof creds);
    app.relay = zf_relay_new(app.jf, cfg);
    app.reporter = zf_reporter_new(app.jf);
    app.workers = zf_workers_new(2, 32);
    zf_player_hooks hooks = {.ctx = &app, .report = hook_report, .released = hook_released};
    app.player = zf_player_new(cfg, hooks);
    if (!app.auth || !app.jf || !app.relay || !app.reporter || !app.workers || !app.player ||
        zf_relay_start(app.relay, cfg->relay_port) < 0 || zf_reporter_start(app.reporter) < 0 ||
        zf_player_start(app.player) < 0) {
        LOGE("startup failed");
        teardown(&app);
        return 1;
    }
    app.http = zf_http_start(&app);
    if (!app.http) {
        teardown(&app);
        return 1;
    }
    app.ctl = zf_ctl_start(&app);

    LOGI("Zeropfin %s ready", ZF_VERSION);
    if (!configured)
        LOGW("Jellyfin is not set up yet: run `sudo -u zeropfin zeropfin setup`");
    sd_notify_msg("READY=1");

    int rc = 0;
    for (;;) {
        int sig = 0;
        if (sigwait(&set, &sig) != 0)
            continue;
        if (sig == SIGHUP) {
            zf_app_reload_creds(&app);
            continue;
        }
        LOGI("received signal %d, shutting down", sig);
        break;
    }
    teardown(&app);
    return rc;
}
