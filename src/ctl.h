/* ctl.h — local admin control socket (<runtime_dir>/control.sock, mode 0600).
 *
 * Used by `zeropfin pair`, `zeropfin sessions`, `zeropfin reload` and
 * `zeropfin status` from an SSH shell. Peers are authenticated with
 * SO_PEERCRED: only root or the daemon's own user may connect. */
#ifndef ZF_CTL_H
#define ZF_CTL_H

#include <stdio.h>

#include "app.h"

typedef struct zf_ctl zf_ctl;

zf_ctl *zf_ctl_start(zf_app *app);
void zf_ctl_stop(zf_ctl *c);

/* Client side: send one command line and print the reply. Returns an exit code. */
int zf_ctl_client(const char *runtime_dir, const char *command, FILE *out, FILE *err);

#endif
