#include "setup.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "creds.h"
#include "ctl.h"
#include "jellyfin.h"
#include "urlutil.h"

static int read_line(const char *prompt, char *buf, size_t cap, bool echo)
{
    if (prompt) {
        fputs(prompt, stderr);
        fflush(stderr);
    }
    struct termios old, quiet;
    bool tty = isatty(STDIN_FILENO);
    if (!echo && tty) {
        tcgetattr(STDIN_FILENO, &old);
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    char *r = fgets(buf, (int)cap, stdin);
    if (!echo && tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fputc('\n', stderr);
    }
    if (!r)
        return -1;
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

static bool is_loopback_or_https(const char *url)
{
    zf_url u;
    if (zf_url_parse(url, &u) < 0)
        return false;
    return !strcmp(u.scheme, "https") || !strcmp(u.host, "localhost") || !strncmp(u.host, "127.", 4) ||
           !strcmp(u.host, "[::1]");
}

int zf_setup_run(const zf_config *cfg, const zf_setup_opts *o)
{
    if (geteuid() == 0 && !o->allow_root) {
        fprintf(stderr, "zeropfin setup must run as the service user so it can own the credentials file:\n"
                        "  sudo -u zeropfin zeropfin setup\n");
        return 2;
    }
    struct stat st;
    if (stat(cfg->state_dir, &st) < 0 || !S_ISDIR(st.st_mode) || access(cfg->state_dir, W_OK) < 0) {
        fprintf(stderr, "zeropfin: state directory %s is missing or not writable by this user.\n", cfg->state_dir);
        return 2;
    }

    zf_creds cur;
    int lrc = zf_creds_load(cfg->state_dir, &cur);
    if (lrc < 0) {
        fprintf(stderr, "zeropfin: the existing credentials file is unreadable or unsafe; fix or remove it.\n");
        return 2;
    }

    char input[512], url[300];
    if (o->server) {
        zf_strcpy(input, sizeof input, o->server);
    } else {
        char prompt[400];
        if (cur.server_url[0])
            zf_fmt(prompt, sizeof prompt, "Jellyfin server URL [%s]: ", cur.server_url);
        else
            zf_fmt(prompt, sizeof prompt, "Jellyfin server URL (e.g. http://192.168.1.20:8096): ");
        if (read_line(prompt, input, sizeof input, true) < 0)
            return 1;
        if (!input[0] && cur.server_url[0])
            zf_strcpy(input, sizeof input, cur.server_url);
    }
    if (zf_normalize_server_url(input, url, sizeof url) < 0) {
        fprintf(stderr, "zeropfin: '%s' is not a valid http(s) URL without credentials or query.\n", input);
        return 1;
    }

    zf_jf_conn *conn = zf_jf_conn_new();
    if (!conn)
        return 1;
    cJSON *info = NULL;
    zf_jf_err e = zf_jf_public_info(conn, url, cfg->jellyfin_ca_file, &info);
    if (e != ZF_JF_OK) {
        fprintf(stderr, "zeropfin: %s (%s/System/Info/Public)\n", zf_jf_err_message(e), url);
        zf_jf_conn_free(conn);
        return 1;
    }
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(info, "Version");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(info, "ServerName");
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(info, "Id");
    const char *vs = cJSON_IsString(ver) ? ver->valuestring : "?";
    fprintf(stderr, "Found Jellyfin server \"%s\" version %s\n", cJSON_IsString(name) ? name->valuestring : "?", vs);
    if (zf_jf_version_number(vs) < ZF_JF_MIN_VERSION) {
        fprintf(stderr, "zeropfin: Jellyfin 10.10 or newer is required.\n");
        cJSON_Delete(info);
        zf_jf_conn_free(conn);
        return 1;
    }
    if (!is_loopback_or_https(url))
        fprintf(stderr,
                "Note: %s is plain HTTP. Your password (once) and the access token travel unencrypted on "
                "this network. Prefer HTTPS when the network is not trusted.\n",
                url);

    zf_creds nc;
    memset(&nc, 0, sizeof nc);
    zf_strcpy(nc.device_id, sizeof nc.device_id, cur.device_id); /* keep a stable device id */
    if (zf_creds_ensure_device_id(&nc) < 0) {
        cJSON_Delete(info);
        zf_jf_conn_free(conn);
        return 1;
    }
    zf_strcpy(nc.server_url, sizeof nc.server_url, url);
    if (cJSON_IsString(name) && !zf_has_ctl(name->valuestring))
        zf_strcpy(nc.server_name, sizeof nc.server_name, name->valuestring);
    if (cJSON_IsString(ver))
        zf_strcpy(nc.server_version, sizeof nc.server_version, vs);
    if (cJSON_IsString(sid) && zf_token_syntax_ok(sid->valuestring))
        zf_strcpy(nc.server_id, sizeof nc.server_id, sid->valuestring);
    cJSON_Delete(info);

    char user[200], pw[512];
    if (o->user)
        zf_strcpy(user, sizeof user, o->user);
    else if (read_line("Jellyfin username: ", user, sizeof user, true) < 0) {
        zf_jf_conn_free(conn);
        return 1;
    }
    if (read_line(o->password_stdin ? NULL : "Password (not stored): ", pw, sizeof pw, false) < 0) {
        zf_jf_conn_free(conn);
        return 1;
    }
    cJSON *auth = NULL;
    e = zf_jf_authenticate(conn, url, cfg->jellyfin_ca_file, cfg->device_name, nc.device_id, user, pw, &auth);
    memset(pw, 0, sizeof pw);
    zf_jf_conn_free(conn);
    if (e != ZF_JF_OK) {
        fprintf(stderr, "zeropfin: sign-in failed: %s\n",
                e == ZF_JF_ERR_AUTH ? "wrong username or password" : zf_jf_err_message(e));
        cJSON_Delete(auth);
        return 1;
    }
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(auth, "AccessToken");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(auth, "User");
    const cJSON *uid = cJSON_GetObjectItemCaseSensitive(u, "Id");
    const cJSON *uname = cJSON_GetObjectItemCaseSensitive(u, "Name");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(auth, "ServerId");
    char nuid[33];
    if (!cJSON_IsString(tok) || !zf_token_syntax_ok(tok->valuestring) || !cJSON_IsString(uid) ||
        !zf_jf_id_normalize(uid->valuestring, nuid, sizeof nuid)) {
        fprintf(stderr, "zeropfin: unexpected sign-in response from the server.\n");
        if (cJSON_IsString(tok))
            memset(tok->valuestring, 0, strlen(tok->valuestring));
        cJSON_Delete(auth);
        return 1;
    }
    zf_strcpy(nc.access_token, sizeof nc.access_token, tok->valuestring);
    memset(tok->valuestring, 0, strlen(tok->valuestring));
    zf_strcpy(nc.user_id, sizeof nc.user_id, nuid);
    if (cJSON_IsString(uname) && !zf_has_ctl(uname->valuestring))
        zf_strcpy(nc.user_name, sizeof nc.user_name, uname->valuestring);
    if (cJSON_IsString(ssid) && zf_token_syntax_ok(ssid->valuestring))
        zf_strcpy(nc.server_id, sizeof nc.server_id, ssid->valuestring);
    cJSON_Delete(auth);

    int src = zf_creds_save(cfg->state_dir, &nc);
    memset(&nc, 0, sizeof nc);
    memset(&cur, 0, sizeof cur);
    if (src < 0) {
        fprintf(stderr, "zeropfin: could not write %s/credentials.json: %s\n", cfg->state_dir, strerror(errno));
        return 1;
    }
    fprintf(stderr, "Signed in as %s. Access token stored in %s/credentials.json (mode 0600).\n", user, cfg->state_dir);
    /* Tell a running daemon to pick it up; harmless if it is not running. */
    FILE *devnull = fopen("/dev/null", "w");
    int crc = zf_ctl_client(cfg->runtime_dir, "reload", devnull ? devnull : stderr, devnull ? devnull : stderr);
    if (devnull)
        fclose(devnull);
    if (crc == 0)
        fprintf(stderr, "The running service reloaded the new credentials.\n");
    else
        fprintf(stderr, "Restart the service to use them: sudo systemctl restart zeropfin\n");
    return 0;
}
