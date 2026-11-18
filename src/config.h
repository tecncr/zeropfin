/* config.h — validated daemon configuration (/etc/zeropfin/zeropfin.conf).
 *
 * Format: one "key = value" per line, '#' comments, no quoting or escapes.
 * Unknown keys and invalid values are errors: a typo must never silently
 * weaken security. Secrets never live in this file (see creds.h). */
#ifndef ZF_CONFIG_H
#define ZF_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZF_MAX_PLAYER_ARGS 16
#define ZF_MAX_LIST        16

typedef struct {
    /* LAN remote-control listener */
    char listen_address[64]; /* "::" (dual stack), "0.0.0.0", or a literal address */
    uint16_t listen_port;
    char allowed_hosts[ZF_MAX_LIST][128]; /* extra Host header names (DNS rebinding guard) */
    int n_allowed_hosts;
    char allowed_networks[ZF_MAX_LIST][64]; /* extra client CIDRs beyond private ranges */
    int n_allowed_networks;
    bool allow_any_client_address; /* dangerous: disables the private-address filter */

    /* Loopback media relay (never bound to anything but 127.0.0.1) */
    uint16_t relay_port; /* 0 = kernel-assigned ephemeral port */

    /* Paths */
    char web_root[256];
    char state_dir[256];
    char runtime_dir[256];

    /* ZeroPlay */
    char zeroplay_path[256];
    char zeroplay_args[ZF_MAX_PLAYER_ARGS][128];
    int n_zeroplay_args;
    int player_ready_timeout_s;
    int player_load_timeout_s;

    /* Local authentication */
    bool pairing_show_on_display;
    int session_days;

    /* Media eligibility (conservative defaults; see docs/adr/0004) */
    int max_width, max_height, max_fps, max_level;
    int64_t max_bitrate;
    char audio_codecs[ZF_MAX_LIST][16];
    int n_audio_codecs;
    char containers[ZF_MAX_LIST][16];
    int n_containers;

    /* Jellyfin */
    char device_name[64];
    char jellyfin_ca_file[256];
    int progress_interval_s;
    bool report_estimated_position;

    bool verbose;
} zf_config;

void zf_config_defaults(zf_config *c);
/* Parse text into c (on top of existing values). Returns 0, or -1 and fills err. */
int zf_config_parse(zf_config *c, const char *text, char *err, size_t errcap);
/* Load file if it exists (missing file = defaults). Applies systemd
 * STATE_DIRECTORY / RUNTIME_DIRECTORY environment overrides afterwards. */
int zf_config_load(zf_config *c, const char *path, char *err, size_t errcap);
/* Case-insensitive membership in a ZF_MAX_LIST x 16 token table (pass &table[0][0]). */
bool zf_config_list_has(const char *list, int n, const char *v);

#endif
