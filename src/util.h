/* util.h — logging, time, randomness, bounded strings, files, tick conversion. */
#ifndef ZF_UTIL_H
#define ZF_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- logging ------------------------------------------------------------
 * Messages go to stderr. When running under systemd (JOURNAL_STREAM set) a
 * "<N>" syslog priority prefix is emitted so journald records the level.
 * Never pass secrets to these functions; use zf_redact_url() for URLs. */
enum zf_log_level { ZF_LOG_ERROR = 3, ZF_LOG_WARN = 4, ZF_LOG_INFO = 6, ZF_LOG_DEBUG = 7 };

void zf_log_init(int verbose);
void zf_log(enum zf_log_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) zf_log(ZF_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) zf_log(ZF_LOG_WARN, __VA_ARGS__)
#define LOGI(...) zf_log(ZF_LOG_INFO, __VA_ARGS__)
#define LOGD(...) zf_log(ZF_LOG_DEBUG, __VA_ARGS__)

/* Copy `url` into `out` with the query string and any userinfo replaced by
 * "[redacted]". Always NUL-terminates (truncating if needed). */
void zf_redact_url(const char *url, char *out, size_t cap);

/* Replace "/media/<id>" relay capability ids with "/media/[id]". */
void zf_scrub_media_ids(const char *in, char *out, size_t cap);
/* libmicrohttpd external logger (MHD_OPTION_EXTERNAL_LOGGER): scrubs relay
 * ids and logs at debug level (client aborts are routine for media). */
void zf_mhd_logger(void *cls, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));

/* ---- time --------------------------------------------------------------- */
int64_t zf_mono_ms(void); /* CLOCK_MONOTONIC, milliseconds */
int64_t zf_wall_ms(void); /* CLOCK_REALTIME, milliseconds (display/expiry only) */

/* ---- randomness --------------------------------------------------------- */
int zf_random_bytes(void *buf, size_t len);                   /* getrandom(); 0 ok, -1 error */
int zf_random_hex(char *out, size_t cap, size_t nbytes);      /* out gets 2*nbytes hex + NUL */
void zf_hex_encode(const uint8_t *in, size_t len, char *out); /* out: 2*len+1 */

/* Constant-time comparison of equal-length buffers. */
bool zf_ct_equal(const void *a, const void *b, size_t len);

/* ---- bounded strings ---------------------------------------------------- */
/* Copy src into dst (capacity cap). Returns 0, or -1 if truncated (dst is
 * still NUL-terminated). */
int zf_strcpy(char *dst, size_t cap, const char *src);
/* snprintf that returns -1 on error or truncation. */
int zf_fmt(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool zf_has_ctl(const char *s); /* any byte < 0x20 or 0x7f */

/* Growable byte buffer with a hard cap. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
    bool overflow;
} zf_buf;
void zf_buf_init(zf_buf *b, size_t limit);
int zf_buf_append(zf_buf *b, const void *p, size_t n); /* -1 when limit exceeded */
void zf_buf_free(zf_buf *b);
char *zf_buf_cstr(zf_buf *b); /* ensures NUL terminator; NULL on OOM */

/* ---- files -------------------------------------------------------------- */
/* Read a whole file up to `max` bytes (NUL-terminated). Returns malloc'd data
 * or NULL (errno set; EFBIG when larger than max). */
char *zf_read_file(const char *path, size_t max, size_t *len_out);
/* Atomically replace `path` with data: write tmp, fsync, rename, fsync dir. */
int zf_write_file_atomic(const char *path, const void *data, size_t len, unsigned mode);

/* ---- Jellyfin ticks ------------------------------------------------------
 * Jellyfin positions are 100 ns "ticks": 10,000,000 per second, 10,000 per ms.
 * Return 0 on success, -1 on negative input or overflow. */
#define ZF_TICKS_PER_MS 10000LL
int zf_ms_to_ticks(int64_t ms, int64_t *ticks);
int zf_ticks_to_ms(int64_t ticks, int64_t *ms);

/* Parse a non-negative decimal integer occupying the whole string. */
int zf_parse_i64(const char *s, int64_t min, int64_t max, int64_t *out);

#endif
