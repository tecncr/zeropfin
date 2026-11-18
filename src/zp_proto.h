/* zp_proto.h — ZeroPlay `--control` line protocol (pure, no I/O).
 *
 * Documented upstream (stdin): load/loadloop/pause/resume/stop/seek <ms>/quit
 * Documented upstream (stdout): ready, ended
 * Undocumented but present upstream: osd <ms> <text>, osdclear -> "osd_ok"
 * Zeropfin fork extensions (opt-in, probed): status, events on, loadat,
 *   "status <state> <posMs> <durMs>", "events on", "loaded <durMs>", "error load"
 * Anything else on stdout is ignored (logged at debug level). */
#ifndef ZF_ZP_PROTO_H
#define ZF_ZP_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZP_MAX_LINE 4096
#define ZP_MAX_URL  1024

typedef enum {
    ZP_EV_UNKNOWN = 0,
    ZP_EV_READY,
    ZP_EV_ENDED,
    ZP_EV_LOADED,     /* ext: duration_ms (0 = unknown) */
    ZP_EV_LOAD_ERROR, /* ext */
    ZP_EV_STATUS,     /* ext: state, pos_ms, dur_ms */
    ZP_EV_EVENTS_ACK, /* ext: events_on */
    ZP_EV_OSD_OK,
} zp_event_type;

typedef enum { ZP_ST_IDLE, ZP_ST_PLAYING, ZP_ST_PAUSED, ZP_ST_IMAGE } zp_status_state;

typedef struct {
    zp_event_type type;
    int64_t pos_ms;
    int64_t dur_ms;
    zp_status_state state;
    bool events_on;
} zp_event;

/* Parse one stdout line (without newline). Never fails: unrecognized or
 * malformed lines yield ZP_EV_UNKNOWN. */
void zp_parse_event(const char *line, zp_event *ev);

/* Accumulates bytes into newline-terminated lines with a hard bound; an
 * overlong line is discarded up to its terminating newline. */
typedef struct {
    char buf[ZP_MAX_LINE];
    size_t len;
    bool discarding;
    unsigned long discarded_lines;
} zp_linebuf;
typedef void (*zp_line_cb)(void *ctx, const char *line);
void zp_linebuf_init(zp_linebuf *lb);
void zp_linebuf_feed(zp_linebuf *lb, const char *data, size_t n, zp_line_cb cb, void *ctx);

/* Media URL accepted for `load`: http://127.0.0.1:<port>/<path> with a
 * conservative character set (no spaces, quotes, parentheses, CR/LF/NUL). */
bool zp_url_ok(const char *url);

/* Command formatting. All return 0 or -1 (invalid parameter/overflow).
 * Output includes the trailing '\n'. */
int zp_fmt_load(char *out, size_t cap, const char *url, int64_t start_ms, bool ext_loadat);
int zp_fmt_seek(char *out, size_t cap, int64_t pos_ms);
/* OSD text is reduced to printable ASCII without backslashes, max 120 chars. */
int zp_fmt_osd(char *out, size_t cap, int duration_ms, const char *text);

/* stderr heuristic used only when extended events are unavailable. */
bool zp_stderr_is_load_failure(const char *line);

#endif
