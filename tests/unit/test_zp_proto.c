#include "util.h"
#include "zp_proto.h"
#include "zt.h"

static void events(void)
{
    zp_event e;
    zp_parse_event("ready", &e);
    ZT_EQ_INT(e.type, ZP_EV_READY);
    zp_parse_event("ended", &e);
    ZT_EQ_INT(e.type, ZP_EV_ENDED);
    zp_parse_event("loaded 5400000", &e);
    ZT_EQ_INT(e.type, ZP_EV_LOADED);
    ZT_EQ_INT(e.dur_ms, 5400000);
    zp_parse_event("loaded -1", &e);
    ZT_EQ_INT(e.type, ZP_EV_UNKNOWN);
    zp_parse_event("error load", &e);
    ZT_EQ_INT(e.type, ZP_EV_LOAD_ERROR);
    zp_parse_event("events on", &e);
    ZT_EQ_INT(e.type, ZP_EV_EVENTS_ACK);
    ZT_CHECK(e.events_on);
    zp_parse_event("status paused 61000 120000", &e);
    ZT_EQ_INT(e.type, ZP_EV_STATUS);
    ZT_EQ_INT(e.state, ZP_ST_PAUSED);
    ZT_EQ_INT(e.pos_ms, 61000);
    ZT_EQ_INT(e.dur_ms, 120000);
    const char *junk[] = {"",
                          "READY",
                          "ready ",
                          "status playing 1",
                          "status bogus 1 2",
                          "status playing 1 2 3",
                          "status playing x 2",
                          "osd_ok extra",
                          "ended\r",
                          "loaded 99999999999999999999"};
    for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
        zp_parse_event(junk[i], &e);
        ZT_EQ_INT(e.type, ZP_EV_UNKNOWN);
    }
}

typedef struct {
    int n;
    char lines[8][64];
} collector;

static void collect(void *ctx, const char *line)
{
    collector *c = ctx;
    if (c->n < 8)
        zf_strcpy(c->lines[c->n++], 64, line);
}

static void linebuf(void)
{
    zp_linebuf lb;
    collector c = {0};
    zp_linebuf_init(&lb);
    zp_linebuf_feed(&lb, "rea", 3, collect, &c);
    ZT_EQ_INT(c.n, 0);
    zp_linebuf_feed(&lb, "dy\nend", 6, collect, &c);
    ZT_EQ_INT(c.n, 1);
    ZT_EQ_STR(c.lines[0], "ready");
    zp_linebuf_feed(&lb, "ed\r\nx\0y\nok\n", 11, collect, &c);
    ZT_EQ_INT(c.n, 3);
    ZT_EQ_STR(c.lines[1], "ended"); /* CRLF tolerated */
    ZT_EQ_STR(c.lines[2], "ok");    /* NUL-poisoned line dropped */
    /* overlong line is discarded, following line still parsed */
    char big[ZP_MAX_LINE + 100];
    memset(big, 'A', sizeof big);
    zp_linebuf_feed(&lb, big, sizeof big, collect, &c);
    zp_linebuf_feed(&lb, "\nended\n", 7, collect, &c);
    ZT_EQ_INT(c.n, 4);
    ZT_EQ_STR(c.lines[3], "ended");
    ZT_EQ_INT(lb.discarded_lines, 2);
}

static void commands(void)
{
    char out[1200];
    const char *url = "http://127.0.0.1:34567/media/0123456789abcdef0123456789abcdef";
    ZT_EQ_INT(zp_fmt_load(out, sizeof out, url, 0, true), 0);
    ZT_EQ_STR(out, "load http://127.0.0.1:34567/media/0123456789abcdef0123456789abcdef\n");
    ZT_EQ_INT(zp_fmt_load(out, sizeof out, url, 61000, true), 0);
    ZT_EQ_STR(out, "loadat 61000 http://127.0.0.1:34567/media/0123456789abcdef0123456789abcdef\n");
    ZT_EQ_INT(zp_fmt_load(out, sizeof out, url, 61000, false), 0); /* stock: seek is sent separately */
    ZT_CHECK(!strncmp(out, "load ", 5));
    const char *bad[] = {
        "http://127.0.0.1:1/media/x\nquit", /* command injection via newline */
        "http://127.0.0.1:1/media/x\rquit",
        "http://127.0.0.1:1/media/x y", /* spaces */
        "http://example.com/media/x",   /* non-loopback */
        "https://127.0.0.1:1/x",
        "(http://127.0.0.1:1/a)(http://x/b)", /* zeroplay dual-URL syntax */
        "http://127.0.0.1:/x",
        "http://127.0.0.1:123456/x",
        "/etc/passwd",
        "http://127.0.0.1:1/x?api_key=1",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        ZT_CHECK(!zp_url_ok(bad[i]));
        ZT_EQ_INT(zp_fmt_load(out, sizeof out, bad[i], 0, true), -1);
    }
    ZT_EQ_INT(zp_fmt_load(out, sizeof out, url, -1, true), -1);
    ZT_EQ_INT(zp_fmt_seek(out, sizeof out, 90000), 0);
    ZT_EQ_STR(out, "seek 90000\n");
    ZT_EQ_INT(zp_fmt_seek(out, sizeof out, -1), -1);
    ZT_EQ_INT(zp_fmt_osd(out, sizeof out, 5000, "Code: AB\nquit\\n"), 0);
    ZT_EQ_STR(out, "osd 5000 Code: AB quit n\n");
    ZT_EQ_INT(zp_fmt_osd(out, sizeof out, -1, "x"), -1);
    ZT_EQ_INT(zp_fmt_osd(out, sizeof out, 10, ""), -1);
    ZT_CHECK(zp_stderr_is_load_failure("zeroplay: failed to open 'x'"));
    ZT_CHECK(!zp_stderr_is_load_failure("zeroplay: load x"));
}

int main(void)
{
    ZT_RUN(events);
    ZT_RUN(linebuf);
    ZT_RUN(commands);
    ZT_DONE();
}
