#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util.h"

void zf_config_defaults(zf_config *c)
{
    memset(c, 0, sizeof *c);
    zf_strcpy(c->listen_address, sizeof c->listen_address, "::");
    c->listen_port = 8080;
    c->relay_port = 0;
    zf_strcpy(c->web_root, sizeof c->web_root, "/usr/share/zeropfin/web");
    zf_strcpy(c->state_dir, sizeof c->state_dir, "/var/lib/zeropfin");
    zf_strcpy(c->runtime_dir, sizeof c->runtime_dir, "/run/zeropfin");
    zf_strcpy(c->zeroplay_path, sizeof c->zeroplay_path, "/usr/lib/zeropfin/zeroplay");
    c->player_ready_timeout_s = 20;
    c->player_load_timeout_s = 45;
    c->pairing_show_on_display = true;
    c->session_days = 30;
    c->max_width = 1920;
    c->max_height = 1080;
    c->max_fps = 30;
    c->max_level = 41;
    c->max_bitrate = 12000000;
    const char *ac[] = {"aac", "mp3", "ac3", "flac", "opus"};
    for (size_t i = 0; i < sizeof ac / sizeof *ac; i++)
        zf_strcpy(c->audio_codecs[c->n_audio_codecs++], 16, ac[i]);
    const char *ct[] = {"mp4", "m4v", "mov", "mkv"};
    for (size_t i = 0; i < sizeof ct / sizeof *ct; i++)
        zf_strcpy(c->containers[c->n_containers++], 16, ct[i]);
    c->progress_interval_s = 10;
    c->report_estimated_position = true;
}

bool zf_config_list_has(const char *list, int n, const char *v)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(list + (size_t)i * 16, v) == 0)
            return true;
    return false;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static int parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1") || !strcasecmp(v, "on"))
        *out = true;
    else if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcmp(v, "0") || !strcasecmp(v, "off"))
        *out = false;
    else
        return -1;
    return 0;
}

static int parse_int(const char *v, int64_t min, int64_t max, int64_t *out)
{
    return zf_parse_i64(v, min, max, out);
}

static int set_path(char *dst, size_t cap, const char *v)
{
    if (v[0] != '/' || zf_has_ctl(v) || strstr(v, "/../") || strlen(v) >= cap)
        return -1;
    size_t n = strlen(v);
    if (n >= 3 && strcmp(v + n - 3, "/..") == 0)
        return -1;
    return zf_strcpy(dst, cap, v);
}

/* Split a comma-separated list into a fixed table of short tokens. */
static int set_list(char *base, size_t elem, int max, int *n, const char *v, bool lower)
{
    *n = 0;
    char tmp[1024];
    if (zf_strcpy(tmp, sizeof tmp, v) < 0)
        return -1;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok)
            continue;
        if (*n >= max || strlen(tok) >= elem)
            return -1;
        for (char *p = tok; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            if (!(isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == ':' || ch == '/'))
                return -1;
            if (lower)
                *p = (char)tolower(ch);
        }
        zf_strcpy(base + (size_t)(*n) * elem, elem, tok);
        (*n)++;
    }
    return 0;
}

/* Player arguments: whitespace separated, only an allowlisted subset of
 * ZeroPlay flags. Control mode is always added by us; network/remote
 * features (--ws-url, --device-token, ...) are refused. */
static int set_player_args(zf_config *c, const char *v, char *err, size_t errcap)
{
    static const struct {
        const char *flag;
        bool takes_value;
    } allowed[] = {
        {"--audio-device", true}, {"--no-audio", false},   {"--no-downmixing", false},
        {"--vol", true},          {"--hls-bitrate", true}, {"--verbose", false},
    };
    char tmp[2048];
    if (zf_strcpy(tmp, sizeof tmp, v) < 0)
        return -1;
    c->n_zeroplay_args = 0;
    char *save = NULL;
    bool need_value = false;
    for (char *tok = strtok_r(tmp, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (c->n_zeroplay_args >= ZF_MAX_PLAYER_ARGS || strlen(tok) >= sizeof c->zeroplay_args[0])
            return -1;
        if (need_value) {
            if (tok[0] == '-') {
                zf_fmt(err, errcap, "zeroplay_args: flag needs a value");
                return -1;
            }
            need_value = false;
        } else {
            bool ok = false;
            for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++) {
                if (strcmp(tok, allowed[i].flag) == 0) {
                    ok = true;
                    need_value = allowed[i].takes_value;
                }
            }
            if (!ok) {
                zf_fmt(err, errcap, "zeroplay_args: flag '%.40s' is not allowed", tok);
                return -1;
            }
        }
        zf_strcpy(c->zeroplay_args[c->n_zeroplay_args++], sizeof c->zeroplay_args[0], tok);
    }
    if (need_value) {
        zf_fmt(err, errcap, "zeroplay_args: missing value for last flag");
        return -1;
    }
    return 0;
}

int zf_config_parse(zf_config *c, const char *text, char *err, size_t errcap)
{
    char *copy = strdup(text);
    if (!copy) {
        zf_fmt(err, errcap, "out of memory");
        return -1;
    }
    int rc = 0, lineno = 0;
    char *save = NULL;
    char *line = copy;
    for (;;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        lineno++;
        char *s = line;
        char *hash = strchr(s, '#');
        if (hash)
            *hash = '\0';
        s = trim(s);
        if (*s) {
            char *eq = strchr(s, '=');
            if (!eq) {
                zf_fmt(err, errcap, "line %d: expected key = value", lineno);
                rc = -1;
                break;
            }
            *eq = '\0';
            char *k = trim(s), *v = trim(eq + 1);
            int64_t n = 0;
            bool b = false;
            int bad = 0;
            char sub[160] = "";
            if (!strcmp(k, "listen_address")) {
                bad = zf_has_ctl(v) || zf_strcpy(c->listen_address, sizeof c->listen_address, v) < 0;
            } else if (!strcmp(k, "listen_port")) {
                bad = parse_int(v, 1, 65535, &n) < 0;
                c->listen_port = (uint16_t)n;
            } else if (!strcmp(k, "relay_port")) {
                bad = parse_int(v, 0, 65535, &n) < 0;
                c->relay_port = (uint16_t)n;
            } else if (!strcmp(k, "allowed_hosts")) {
                bad = set_list(&c->allowed_hosts[0][0], sizeof c->allowed_hosts[0], ZF_MAX_LIST, &c->n_allowed_hosts, v,
                               true) < 0;
            } else if (!strcmp(k, "allowed_networks")) {
                bad = set_list(&c->allowed_networks[0][0], sizeof c->allowed_networks[0], ZF_MAX_LIST,
                               &c->n_allowed_networks, v, true) < 0;
            } else if (!strcmp(k, "allow_any_client_address")) {
                bad = parse_bool(v, &b) < 0;
                c->allow_any_client_address = b;
            } else if (!strcmp(k, "web_root")) {
                bad = set_path(c->web_root, sizeof c->web_root, v) < 0;
            } else if (!strcmp(k, "state_dir")) {
                bad = set_path(c->state_dir, sizeof c->state_dir, v) < 0;
            } else if (!strcmp(k, "runtime_dir")) {
                bad = set_path(c->runtime_dir, sizeof c->runtime_dir, v) < 0;
            } else if (!strcmp(k, "zeroplay_path")) {
                bad = set_path(c->zeroplay_path, sizeof c->zeroplay_path, v) < 0;
            } else if (!strcmp(k, "zeroplay_args")) {
                bad = set_player_args(c, v, sub, sizeof sub) < 0;
            } else if (!strcmp(k, "player_ready_timeout_s")) {
                bad = parse_int(v, 1, 600, &n) < 0;
                c->player_ready_timeout_s = (int)n;
            } else if (!strcmp(k, "player_load_timeout_s")) {
                bad = parse_int(v, 1, 600, &n) < 0;
                c->player_load_timeout_s = (int)n;
            } else if (!strcmp(k, "pairing_show_on_display")) {
                bad = parse_bool(v, &b) < 0;
                c->pairing_show_on_display = b;
            } else if (!strcmp(k, "session_days")) {
                bad = parse_int(v, 1, 365, &n) < 0;
                c->session_days = (int)n;
            } else if (!strcmp(k, "max_width")) {
                bad = parse_int(v, 16, 8192, &n) < 0;
                c->max_width = (int)n;
            } else if (!strcmp(k, "max_height")) {
                bad = parse_int(v, 16, 8192, &n) < 0;
                c->max_height = (int)n;
            } else if (!strcmp(k, "max_fps")) {
                bad = parse_int(v, 1, 240, &n) < 0;
                c->max_fps = (int)n;
            } else if (!strcmp(k, "max_level")) {
                bad = parse_int(v, 10, 62, &n) < 0;
                c->max_level = (int)n;
            } else if (!strcmp(k, "max_bitrate")) {
                bad = parse_int(v, 100000, 1000000000, &n) < 0;
                c->max_bitrate = n;
            } else if (!strcmp(k, "audio_codecs")) {
                bad = set_list(&c->audio_codecs[0][0], 16, ZF_MAX_LIST, &c->n_audio_codecs, v, true) < 0;
            } else if (!strcmp(k, "containers")) {
                bad = set_list(&c->containers[0][0], 16, ZF_MAX_LIST, &c->n_containers, v, true) < 0;
            } else if (!strcmp(k, "device_name")) {
                bad = !*v || zf_has_ctl(v) || strchr(v, '"') || strchr(v, '\\') ||
                      zf_strcpy(c->device_name, sizeof c->device_name, v) < 0;
            } else if (!strcmp(k, "jellyfin_ca_file")) {
                bad = *v && set_path(c->jellyfin_ca_file, sizeof c->jellyfin_ca_file, v) < 0;
            } else if (!strcmp(k, "progress_interval_s")) {
                bad = parse_int(v, 5, 300, &n) < 0;
                c->progress_interval_s = (int)n;
            } else if (!strcmp(k, "report_estimated_position")) {
                bad = parse_bool(v, &b) < 0;
                c->report_estimated_position = b;
            } else if (!strcmp(k, "verbose")) {
                bad = parse_bool(v, &b) < 0;
                c->verbose = b;
            } else {
                zf_fmt(err, errcap, "line %d: unknown key '%.60s'", lineno, k);
                rc = -1;
                break;
            }
            if (bad) {
                if (sub[0])
                    zf_fmt(err, errcap, "line %d: %s", lineno, sub);
                else
                    zf_fmt(err, errcap, "line %d: invalid value for '%.60s'", lineno, k);
                rc = -1;
                break;
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    (void)save;
    free(copy);
    return rc;
}

int zf_config_load(zf_config *c, const char *path, char *err, size_t errcap)
{
    if (path) {
        size_t len = 0;
        char *text = zf_read_file(path, 64 * 1024, &len);
        if (!text) {
            if (errno != ENOENT) {
                zf_fmt(err, errcap, "cannot read %s: %s", path, strerror(errno));
                return -1;
            }
        } else {
            int rc = zf_config_parse(c, text, err, errcap);
            free(text);
            if (rc < 0)
                return -1;
        }
    }
    /* systemd StateDirectory=/RuntimeDirectory= provide authoritative paths. */
    const char *sd = getenv("STATE_DIRECTORY");
    if (sd && *sd && !strchr(sd, ':'))
        set_path(c->state_dir, sizeof c->state_dir, sd);
    const char *rd = getenv("RUNTIME_DIRECTORY");
    if (rd && *rd && !strchr(rd, ':'))
        set_path(c->runtime_dir, sizeof c->runtime_dir, rd);
    if (!c->device_name[0]) {
        char host[64] = "";
        if (gethostname(host, sizeof host - 1) < 0 || !host[0] || zf_has_ctl(host) || strchr(host, '"'))
            zf_strcpy(host, sizeof host, "Zeropfin");
        zf_strcpy(c->device_name, sizeof c->device_name, host);
    }
    return 0;
}
