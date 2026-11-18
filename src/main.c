/* main.c — `zeropfin` command-line entry point. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "ctl.h"
#include "setup.h"
#include "util.h"
#include "version.h"

#define DEFAULT_CONFIG "/etc/zeropfin/zeropfin.conf"

static const char *build_arch(void)
{
#if defined(__aarch64__)
    return "arm64";
#elif defined(__arm__) && defined(__ARM_ARCH)
#if __ARM_ARCH == 6
    return "armv6";
#elif __ARM_ARCH == 7
    return "armv7";
#else
    return "arm";
#endif
#elif defined(__x86_64__)
    return "amd64";
#else
    return "unknown";
#endif
}

static void print_version(void)
{
    if (!strcmp(ZF_VERSION, "development build"))
        printf("zeropfin development build\n");
    else
        printf("zeropfin v%s\n", ZF_VERSION);
    printf("commit %s, built %s, linux/%s\n", ZF_COMMIT, ZF_BUILD_DATE, build_arch());
}

static void usage(FILE *f)
{
    fprintf(f, "Usage: zeropfin [options] [command]\n"
               "\n"
               "Commands:\n"
               "  serve              run the receiver daemon (default)\n"
               "  setup              sign in to Jellyfin and store an access token (interactive)\n"
               "  pair               print a one-time pairing code for a new remote\n"
               "  sessions           list paired remotes\n"
               "  revoke ID|all      revoke paired remote(s)\n"
               "  reload             make the running service re-read its Jellyfin credentials\n"
               "  status             show player, relay and Jellyfin status from the running service\n"
               "\n"
               "Options:\n"
               "  -c, --config PATH  configuration file (default " DEFAULT_CONFIG ")\n"
               "  -v, --verbose      debug logging\n"
               "      --version      print version and exit\n"
               "  -h, --help         this help\n"
               "\n"
               "Development overrides (serve):\n"
               "      --listen ADDR  --port N  --relay-port N  --web-root DIR\n"
               "      --state-dir DIR  --runtime-dir DIR  --zeroplay PATH\n"
               "\n"
               "Setup options:\n"
               "      --server URL  --user NAME  --password-stdin\n");
}

enum {
    OPT_VERSION = 1000,
    OPT_LISTEN,
    OPT_PORT,
    OPT_RELAY_PORT,
    OPT_WEB_ROOT,
    OPT_STATE_DIR,
    OPT_RUNTIME_DIR,
    OPT_ZEROPLAY,
    OPT_SERVER,
    OPT_USER,
    OPT_PASSWORD_STDIN,
};

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, OPT_VERSION},
        {"listen", required_argument, NULL, OPT_LISTEN},
        {"port", required_argument, NULL, OPT_PORT},
        {"relay-port", required_argument, NULL, OPT_RELAY_PORT},
        {"web-root", required_argument, NULL, OPT_WEB_ROOT},
        {"state-dir", required_argument, NULL, OPT_STATE_DIR},
        {"runtime-dir", required_argument, NULL, OPT_RUNTIME_DIR},
        {"zeroplay", required_argument, NULL, OPT_ZEROPLAY},
        {"server", required_argument, NULL, OPT_SERVER},
        {"user", required_argument, NULL, OPT_USER},
        {"password-stdin", no_argument, NULL, OPT_PASSWORD_STDIN},
        {NULL, 0, NULL, 0},
    };
    const char *config_path = DEFAULT_CONFIG;
    bool verbose = false;
    /* Overrides are collected as "key = value" lines and parsed with the
     * same validator as the config file. */
    char overrides[2048] = "";
    zf_setup_opts sopts = {0};
    int opt;
    while ((opt = getopt_long(argc, argv, "c:vh", longopts, NULL)) != -1) {
        const char *key = NULL;
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'v': verbose = true; break;
        case 'h': usage(stdout); return 0;
        case OPT_VERSION: print_version(); return 0;
        case OPT_LISTEN: key = "listen_address"; break;
        case OPT_PORT: key = "listen_port"; break;
        case OPT_RELAY_PORT: key = "relay_port"; break;
        case OPT_WEB_ROOT: key = "web_root"; break;
        case OPT_STATE_DIR: key = "state_dir"; break;
        case OPT_RUNTIME_DIR: key = "runtime_dir"; break;
        case OPT_ZEROPLAY: key = "zeroplay_path"; break;
        case OPT_SERVER: sopts.server = optarg; break;
        case OPT_USER: sopts.user = optarg; break;
        case OPT_PASSWORD_STDIN: sopts.password_stdin = true; break;
        default: usage(stderr); return 2;
        }
        if (key) {
            size_t n = strlen(overrides);
            if (strchr(optarg, '\n') || zf_fmt(overrides + n, sizeof overrides - n, "%s = %s\n", key, optarg) < 0) {
                fprintf(stderr, "zeropfin: option value too long\n");
                return 2;
            }
        }
    }
    const char *cmd = optind < argc ? argv[optind] : "serve";
    const char *arg = optind + 1 < argc ? argv[optind + 1] : NULL;

    zf_log_init(verbose);
    zf_config cfg;
    zf_config_defaults(&cfg);
    char err[256] = "";
    if (zf_config_load(&cfg, config_path, err, sizeof err) < 0) {
        fprintf(stderr, "zeropfin: %s: %s\n", config_path, err);
        return 2;
    }
    if (overrides[0] && zf_config_parse(&cfg, overrides, err, sizeof err) < 0) {
        fprintf(stderr, "zeropfin: invalid command-line override: %s\n", err);
        return 2;
    }
    if (cfg.verbose)
        zf_log_init(1);

    if (!strcmp(cmd, "serve"))
        return zf_serve(&cfg);
    if (!strcmp(cmd, "setup"))
        return zf_setup_run(&cfg, &sopts);
    if (!strcmp(cmd, "pair") || !strcmp(cmd, "sessions") || !strcmp(cmd, "reload") || !strcmp(cmd, "status"))
        return zf_ctl_client(cfg.runtime_dir, cmd, stdout, stderr);
    if (!strcmp(cmd, "revoke")) {
        char line[128];
        if (!arg || zf_fmt(line, sizeof line, "revoke %s", arg) < 0 || zf_has_ctl(arg)) {
            fprintf(stderr, "zeropfin: revoke needs a session id (see `zeropfin sessions`) or 'all'\n");
            return 2;
        }
        return zf_ctl_client(cfg.runtime_dir, line, stdout, stderr);
    }
    fprintf(stderr, "zeropfin: unknown command '%s'\n", cmd);
    usage(stderr);
    return 2;
}
