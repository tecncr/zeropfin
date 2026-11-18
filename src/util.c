#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- logging ------------------------------------------------------------ */

static int g_verbose = 0;
static bool g_journal = false;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void zf_log_init(int verbose)
{
    g_verbose = verbose;
    g_journal = getenv("JOURNAL_STREAM") != NULL;
}

void zf_log(enum zf_log_level lvl, const char *fmt, ...)
{
    if (lvl == ZF_LOG_DEBUG && !g_verbose)
        return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* Never let a newline/control byte from remote data forge log lines. */
    for (char *p = msg; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            *p = '?';

    pthread_mutex_lock(&g_log_lock);
    if (g_journal) {
        fprintf(stderr, "<%d>%s\n", (int)lvl, msg);
    } else {
        const char *tag = lvl == ZF_LOG_ERROR  ? "error"
                          : lvl == ZF_LOG_WARN ? "warn"
                          : lvl == ZF_LOG_INFO ? "info"
                                               : "debug";
        fprintf(stderr, "zeropfin: %s: %s\n", tag, msg);
    }
    pthread_mutex_unlock(&g_log_lock);
}

void zf_redact_url(const char *url, char *out, size_t cap)
{
    if (!cap)
        return;
    if (!url) {
        zf_strcpy(out, cap, "(null)");
        return;
    }
    /* Remove userinfo (scheme://user:pass@host) and the query/fragment. */
    char tmp[1024];
    size_t o = 0;
    const char *p = url;
    const char *scheme_end = strstr(url, "://");
    if (scheme_end) {
        size_t n = (size_t)(scheme_end - url) + 3;
        if (n >= sizeof tmp)
            n = sizeof tmp - 1;
        memcpy(tmp, url, n);
        o = n;
        p = scheme_end + 3;
        const char *auth_end = strpbrk(p, "/?#");
        const char *at = strchr(p, '@');
        if (at && (!auth_end || at < auth_end)) {
            const char *r = "[redacted]@";
            size_t rl = strlen(r);
            if (o + rl < sizeof tmp) {
                memcpy(tmp + o, r, rl);
                o += rl;
            }
            p = at + 1;
        }
    }
    for (; *p && o + 1 < sizeof tmp; p++) {
        if (*p == '?' || *p == '#') {
            const char *r = "?[redacted]";
            size_t rl = strlen(r);
            if (o + rl < sizeof tmp) {
                memcpy(tmp + o, r, rl);
                o += rl;
            }
            break;
        }
        tmp[o++] = *p;
    }
    tmp[o] = '\0';
    zf_strcpy(out, cap, tmp);
}

void zf_scrub_media_ids(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!cap)
        return;
    for (const char *s = in; *s && o + 1 < cap;) {
        if (!strncmp(s, "/media/", 7)) {
            const char *r = "/media/[id]";
            size_t rl = strlen(r);
            if (o + rl >= cap)
                break;
            memcpy(out + o, r, rl);
            o += rl;
            s += 7;
            while (*s && *s != ' ' && *s != '\'' && *s != '"' && *s != '`')
                s++;
            continue;
        }
        out[o++] = *s++;
    }
    out[o] = '\0';
}

void zf_mhd_logger(void *cls, const char *fmt, va_list ap)
{
    (void)cls;
    char raw[512], clean[512];
    vsnprintf(raw, sizeof raw, fmt, ap);
    size_t n = strlen(raw);
    while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r'))
        raw[--n] = '\0';
    zf_scrub_media_ids(raw, clean, sizeof clean);
    zf_log(ZF_LOG_DEBUG, "http: %s", clean);
}

/* ---- time --------------------------------------------------------------- */

int64_t zf_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t zf_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- randomness --------------------------------------------------------- */

int zf_random_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t r = getrandom(p, len, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

void zf_hex_encode(const uint8_t *in, size_t len, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 15];
    }
    out[2 * len] = '\0';
}

int zf_random_hex(char *out, size_t cap, size_t nbytes)
{
    uint8_t raw[64];
    if (nbytes > sizeof raw || cap < 2 * nbytes + 1)
        return -1;
    if (zf_random_bytes(raw, nbytes) < 0)
        return -1;
    zf_hex_encode(raw, nbytes, out);
    memset(raw, 0, sizeof raw);
    return 0;
}

bool zf_ct_equal(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

/* ---- strings ------------------------------------------------------------ */

int zf_strcpy(char *dst, size_t cap, const char *src)
{
    if (!cap)
        return -1;
    size_t n = strlen(src);
    if (n >= cap) {
        memcpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

int zf_fmt(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

bool zf_has_ctl(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f)
            return true;
    return false;
}

void zf_buf_init(zf_buf *b, size_t limit)
{
    memset(b, 0, sizeof *b);
    b->limit = limit;
}

int zf_buf_append(zf_buf *b, const void *p, size_t n)
{
    if (b->overflow || n > b->limit - b->len) {
        b->overflow = true;
        return -1;
    }
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1)
            nc *= 2;
        if (nc > b->limit + 1)
            nc = b->limit + 1;
        char *d = realloc(b->data, nc);
        if (!d) {
            b->overflow = true;
            return -1;
        }
        b->data = d;
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

void zf_buf_free(zf_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

char *zf_buf_cstr(zf_buf *b)
{
    if (!b->data) {
        b->data = malloc(1);
        if (!b->data)
            return NULL;
        b->data[0] = '\0';
        b->cap = 1;
    }
    return b->data;
}

/* ---- files -------------------------------------------------------------- */

char *zf_read_file(const char *path, size_t max, size_t *len_out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    if ((uint64_t)st.st_size > max) {
        close(fd);
        errno = EFBIG;
        return NULL;
    }
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    if (len_out)
        *len_out = got;
    return buf;
}

int zf_write_file_atomic(const char *path, const void *data, size_t len, unsigned mode)
{
    char tmp[PATH_MAX];
    if (zf_fmt(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()) < 0)
        return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, (mode_t)mode);
    if (fd < 0)
        return -1;
    /* umask may have widened/narrowed the mode; enforce it explicitly. */
    if (fchmod(fd, (mode_t)mode) < 0)
        goto fail;
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            goto fail;
        p += w;
        left -= (size_t)w;
    }
    if (fsync(fd) < 0)
        goto fail;
    if (close(fd) < 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (rename(tmp, path) < 0)
        goto fail;
    /* fsync the directory so the rename itself is durable. */
    char dir[PATH_MAX];
    zf_strcpy(dir, sizeof dir, path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir)
            slash[1] = '\0';
        else
            *slash = '\0';
    } else {
        zf_strcpy(dir, sizeof dir, ".");
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
fail:
    if (fd >= 0)
        close(fd);
    unlink(tmp);
    return -1;
}

/* ---- ticks -------------------------------------------------------------- */

int zf_ms_to_ticks(int64_t ms, int64_t *ticks)
{
    if (ms < 0 || ms > INT64_MAX / ZF_TICKS_PER_MS)
        return -1;
    *ticks = ms * ZF_TICKS_PER_MS;
    return 0;
}

int zf_ticks_to_ms(int64_t ticks, int64_t *ms)
{
    if (ticks < 0)
        return -1;
    *ms = ticks / ZF_TICKS_PER_MS;
    return 0;
}

int zf_parse_i64(const char *s, int64_t min, int64_t max, int64_t *out)
{
    if (!s || !*s)
        return -1;
    const char *p = s;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        p++;
    }
    if (!*p)
        return -1;
    /* Accumulate as negative to cover INT64_MIN without overflow. */
    int64_t v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        int d = *p - '0';
        if (v < (INT64_MIN + d) / 10)
            return -1;
        v = v * 10 - d;
    }
    if (!neg) {
        if (v == INT64_MIN)
            return -1;
        v = -v;
    }
    if (v < min || v > max)
        return -1;
    *out = v;
    return 0;
}
