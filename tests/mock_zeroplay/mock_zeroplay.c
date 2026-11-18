/* mock_zeroplay — deterministic stand-in for `zeroplay --control`.
 *
 * Speaks the documented line protocol (ready/ended; load/loadloop/pause/
 * resume/stop/seek/quit, plus osd/osdclear -> osd_ok) and, unless --stock is
 * given, the Zeropfin fork extensions (status, events on, loadat, loaded,
 * error load). Time advances with the monotonic clock scaled by --speed.
 *
 * Failure injection:
 *   --fail-start        exit(1) before printing ready
 *   --no-ready          never print ready (startup timeout)
 *   --crash-after-ms N  abort() N ms (scaled) after a successful load
 *   URLs containing "fail" cannot be opened
 *   --fetch             actually GET the URL (Range bytes=0-65535) with
 *                       libcurl, as FFmpeg would, and fail the load on error
 *   --log FILE          append every received command line to FILE
 */
#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool stock = false, fetch = false, ext = false;
static double speed = 1.0;
static int64_t duration = 5000, crash_after = -1, bytes_per_ms = 0;
static FILE *cmdlog;

enum { IDLE, PLAYING, PAUSED } st = IDLE;
static int64_t pos_anchor, mono_anchor, crash_at = -1;

static int64_t position(void)
{
    int64_t p = pos_anchor;
    if (st == PLAYING)
        p += (int64_t)((double)(now_ms() - mono_anchor) * speed);
    return p > duration ? duration : p;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* With bytes-per-ms=N the clip duration is derived from the media size
 * (Content-Range total), like a real player learning it from the file. */
static size_t on_hdr(char *p, size_t s, size_t n, void *u)
{
    (void)u;
    size_t len = s * n;
    if (bytes_per_ms > 0 && len > 14 && !strncasecmp(p, "Content-Range:", 14)) {
        const char *slash = memchr(p, '/', len);
        if (slash) {
            long long total = strtoll(slash + 1, NULL, 10);
            if (total > 0)
                duration = total / bytes_per_ms;
        }
    }
    return len;
}

static size_t sink(char *p, size_t s, size_t n, void *u)
{
    (void)p;
    size_t *got = u;
    *got += s * n;
    return s * n;
}

static bool try_fetch(const char *url)
{
    CURL *c = curl_easy_init();
    if (!c)
        return false;
    size_t got = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_RANGE, "0-65535");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &got);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    fprintf(stderr, "mock: fetched %zu bytes, HTTP %ld\n", got, code);
    return rc == CURLE_OK && (code == 200 || code == 206) && got > 0;
}

static void do_load(const char *path, int64_t start)
{
    st = IDLE;
    bool ok = !strstr(path, "fail");
    if (ok && fetch)
        ok = try_fetch(path);
    if (!ok) {
        fprintf(stderr, "zeroplay: failed to open '%s'\n", path);
        if (ext)
            out("error load\n");
        return;
    }
    fprintf(stderr, "zeroplay: load %s\n", path);
    st = PLAYING;
    pos_anchor = start;
    mono_anchor = now_ms();
    crash_at = crash_after >= 0 ? now_ms() + (int64_t)((double)crash_after / speed) : -1;
    if (ext)
        out("loaded %lld\n", (long long)duration);
}

static void handle(char *line)
{
    if (cmdlog) {
        fprintf(cmdlog, "%s\n", line);
        fflush(cmdlog);
    }
    char *arg = strchr(line, ' ');
    if (arg)
        *arg++ = '\0';
    if (!strcmp(line, "load") && arg) {
        do_load(arg, 0);
    } else if (!stock && !strcmp(line, "loadat") && arg) {
        char *sp = strchr(arg, ' ');
        if (!sp) {
            if (ext)
                out("error load\n");
            return;
        }
        *sp = '\0';
        do_load(sp + 1, strtoll(arg, NULL, 10));
    } else if (!strcmp(line, "pause")) {
        if (st == PLAYING) {
            pos_anchor = position();
            st = PAUSED;
        }
    } else if (!strcmp(line, "resume")) {
        if (st == PAUSED) {
            mono_anchor = now_ms();
            st = PLAYING;
        }
    } else if (!strcmp(line, "stop")) {
        st = IDLE;
        crash_at = -1;
    } else if (!strcmp(line, "seek") && arg) {
        if (st != IDLE) {
            int64_t t = strtoll(arg, NULL, 10);
            pos_anchor = t < 0 ? 0 : (t > duration ? duration : t);
            mono_anchor = now_ms();
        }
    } else if (!strcmp(line, "osd") || !strcmp(line, "osdclear")) {
        out("osd_ok\n");
    } else if (!stock && !strcmp(line, "status")) {
        const char *s = st == PLAYING ? "playing" : st == PAUSED ? "paused" : "idle";
        out("status %s %lld %lld\n", s, (long long)(st == IDLE ? 0 : position()),
            (long long)(st == IDLE ? 0 : duration));
    } else if (!stock && !strcmp(line, "events") && arg) {
        ext = !strcmp(arg, "on");
        out("events %s\n", ext ? "on" : "off");
    } else if (!strcmp(line, "quit")) {
        exit(0);
    }
    /* anything else: ignored, like ZeroPlay */
}

int main(int argc, char **argv)
{
    bool control = false, fail_start = false, no_ready = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--control"))
            control = true;
        else if (!strcmp(argv[i], "--stock"))
            stock = true;
        else if (!strcmp(argv[i], "--fetch"))
            fetch = true;
        else if (!strcmp(argv[i], "--fail-start"))
            fail_start = true;
        else if (!strcmp(argv[i], "--no-ready"))
            no_ready = true;
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--duration-ms") && i + 1 < argc)
            duration = strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--crash-after-ms") && i + 1 < argc)
            crash_after = strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            cmdlog = fopen(argv[++i], "a");
        else if (!strcmp(argv[i], "--audio-device") || !strcmp(argv[i], "--vol"))
            i++; /* accepted real flags with values */
    }
    /* Flags can also come from the environment, so a single mock binary can be
     * configured from tests that only control zeroplay_path. */
    const char *env = getenv("MOCK_ZEROPLAY_FLAGS");
    if (env) {
        if (strstr(env, "stock"))
            stock = true;
        if (strstr(env, "fetch"))
            fetch = true;
        if (strstr(env, "fail-start"))
            fail_start = true;
        if (strstr(env, "no-ready"))
            no_ready = true;
        const char *c = strstr(env, "crash-after-ms=");
        if (c)
            crash_after = strtoll(c + 15, NULL, 10);
        const char *d = strstr(env, "duration-ms=");
        if (d)
            duration = strtoll(d + 12, NULL, 10);
        const char *b = strstr(env, "bytes-per-ms=");
        if (b)
            bytes_per_ms = strtoll(b + 13, NULL, 10);
        const char *s = strstr(env, "speed=");
        if (s)
            speed = atof(s + 6);
        const char *l = strstr(env, "log=");
        if (l) {
            char path[512];
            size_t n = strcspn(l + 4, " ");
            if (n < sizeof path) {
                memcpy(path, l + 4, n);
                path[n] = '\0';
                cmdlog = fopen(path, "a");
            }
        }
    }
    if (!control) {
        fprintf(stderr, "mock_zeroplay: only --control mode is implemented\n");
        return 2;
    }
    if (fail_start) {
        fprintf(stderr, "drm: failed to open /dev/dri/card0\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!no_ready)
        out("ready\n");
    fprintf(stderr, "zeroplay: control mode — ready (mock%s)\n", stock ? ", stock" : "");

    char buf[8192];
    size_t len = 0;
    for (;;) {
        struct pollfd p = {.fd = 0, .events = POLLIN};
        int pr = poll(&p, 1, 20);
        if (pr > 0) {
            ssize_t r = read(0, buf + len, sizeof buf - 1 - len);
            if (r <= 0)
                return 0; /* controller closed stdin */
            len += (size_t)r;
            char *nl;
            while ((nl = memchr(buf, '\n', len))) {
                *nl = '\0';
                handle(buf);
                size_t rest = len - (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, rest);
                len = rest;
            }
            if (len >= sizeof buf - 1)
                len = 0;
        }
        if (crash_at >= 0 && now_ms() >= crash_at) {
            fprintf(stderr, "mock: simulated crash\n");
            abort();
        }
        if (st == PLAYING && position() >= duration) {
            st = IDLE;
            out("ended\n");
        }
    }
}
