/* app.h — daemon-wide context and lifecycle. */
#ifndef ZF_APP_H
#define ZF_APP_H

#include <stdint.h>

#include "auth.h"
#include "config.h"
#include "jellyfin.h"
#include "player.h"
#include "relay.h"
#include "reporter.h"
#include "workers.h"

struct zf_http;
struct zf_ctl;

typedef struct zf_app {
    zf_config cfg;
    char hostname[64];
    zf_auth *auth;
    zf_jf *jf;
    zf_relay *relay;
    zf_player *player;
    zf_reporter *reporter;
    zf_workers *workers;
    struct zf_http *http;
    struct zf_ctl *ctl;
    int64_t started_mono;
} zf_app;

/* Run the daemon until SIGINT/SIGTERM. Returns a process exit code. */
int zf_serve(const zf_config *cfg);
/* Re-read <state_dir>/credentials.json (after `zeropfin setup`). */
int zf_app_reload_creds(zf_app *app);

#endif
