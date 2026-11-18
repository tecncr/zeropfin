/* setup.h — interactive `zeropfin setup` (run over SSH as the service user).
 * Exchanges a Jellyfin username/password for an access token once; stores
 * only the token, server identity, user id and device id. */
#ifndef ZF_SETUP_H
#define ZF_SETUP_H

#include <stdbool.h>

#include "config.h"

typedef struct {
    const char *server; /* optional: skip the prompt */
    const char *user;   /* optional */
    bool password_stdin;
    bool allow_root; /* testing only */
} zf_setup_opts;

int zf_setup_run(const zf_config *cfg, const zf_setup_opts *o);

#endif
