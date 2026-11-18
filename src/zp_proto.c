#include "zp_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

static bool parse_ms(const char *s, int64_t *out)
{
    return zf_parse_i64(s, 0, INT64_MAX / 1000, out) == 0;
}

void zp_parse_event(const char *line, zp_event *ev)
{
    memset(ev, 0, sizeof *ev);
    ev->type = ZP_EV_UNKNOWN;
    if (!line)
        return;
    if (!strcmp(line, "ready")) {
        ev->type = ZP_EV_READY;
    } else if (!strcmp(line, "ended")) {
        ev->type = ZP_EV_ENDED;
    } else if (!strcmp(line, "osd_ok")) {
        ev->type = ZP_EV_OSD_OK;
    } else if (!strcmp(line, "error load")) {
        ev->type = ZP_EV_LOAD_ERROR;
    } else if (!strcmp(line, "events on") || !strcmp(line, "events off")) {
        ev->type = ZP_EV_EVENTS_ACK;
        ev->events_on = line[8] == 'n';
    } else if (!strncmp(line, "loaded ", 7)) {
        if (parse_ms(line + 7, &ev->dur_ms))
            ev->type = ZP_EV_LOADED;
    } else if (!strncmp(line, "status ", 7)) {
        char st[16], pos[24], dur[24], extra[2];
        if (sscanf(line + 7, "%15s %23s %23s %1s", st, pos, dur, extra) != 3)
            return;
        if (!strcmp(st, "idle"))
            ev->state = ZP_ST_IDLE;
        else if (!strcmp(st, "playing"))
            ev->state = ZP_ST_PLAYING;
        else if (!strcmp(st, "paused"))
            ev->state = ZP_ST_PAUSED;
        else if (!strcmp(st, "image"))
            ev->state = ZP_ST_IMAGE;
        else
            return;
        if (!parse_ms(pos, &ev->pos_ms) || !parse_ms(dur, &ev->dur_ms))
            return;
        ev->type = ZP_EV_STATUS;
    }
}

void zp_linebuf_init(zp_linebuf *lb)
{
    memset(lb, 0, sizeof *lb);
}

void zp_linebuf_feed(zp_linebuf *lb, const char *data, size_t n, zp_line_cb cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        char ch = data[i];
        if (ch == '\n') {
            if (lb->discarding) {
                lb->discarding = false;
                lb->discarded_lines++;
            } else {
                if (lb->len && lb->buf[lb->len - 1] == '\r')
                    lb->len--;
                lb->buf[lb->len] = '\0';
                cb(ctx, lb->buf);
            }
            lb->len = 0;
            continue;
        }
        if (lb->discarding)
            continue;
        if (ch == '\0') {
            /* NUL in a text protocol: poison the line. */
            lb->discarding = true;
            lb->len = 0;
            continue;
        }
        if (lb->len + 1 >= sizeof lb->buf) {
            lb->discarding = true;
            lb->len = 0;
            continue;
        }
        lb->buf[lb->len++] = ch;
    }
}

bool zp_url_ok(const char *url)
{
    static const char prefix[] = "http://127.0.0.1:";
    if (!url || strncmp(url, prefix, sizeof prefix - 1) != 0)
        return false;
    size_t n = strlen(url);
    if (n >= ZP_MAX_URL)
        return false;
    const char *p = url + sizeof prefix - 1;
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
        p++;
        digits++;
    }
    if (digits < 1 || digits > 5 || *p != '/')
        return false;
    for (; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (!(isalnum(ch) || ch == '/' || ch == '.' || ch == '-' || ch == '_'))
            return false;
    }
    return true;
}

int zp_fmt_load(char *out, size_t cap, const char *url, int64_t start_ms, bool ext_loadat)
{
    if (!zp_url_ok(url) || start_ms < 0)
        return -1;
    if (ext_loadat && start_ms > 0)
        return zf_fmt(out, cap, "loadat %lld %s\n", (long long)start_ms, url);
    return zf_fmt(out, cap, "load %s\n", url);
}

int zp_fmt_seek(char *out, size_t cap, int64_t pos_ms)
{
    if (pos_ms < 0 || pos_ms > INT64_MAX / 1000)
        return -1;
    return zf_fmt(out, cap, "seek %lld\n", (long long)pos_ms);
}

int zp_fmt_osd(char *out, size_t cap, int duration_ms, const char *text)
{
    if (duration_ms < 0 || duration_ms > 3600000 || !text)
        return -1;
    char clean[121];
    size_t o = 0;
    for (const char *p = text; *p && o < sizeof clean - 1; p++) {
        unsigned char ch = (unsigned char)*p;
        clean[o++] = (ch >= 0x20 && ch < 0x7f && ch != '\\') ? (char)ch : ' ';
    }
    clean[o] = '\0';
    if (!o)
        return -1;
    return zf_fmt(out, cap, "osd %d %s\n", duration_ms, clean);
}

bool zp_stderr_is_load_failure(const char *line)
{
    return line && (!strncmp(line, "zeroplay: failed to open", 24) || !strncmp(line, "demux: could not open", 21));
}
