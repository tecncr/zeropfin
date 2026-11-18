#include "player_state.h"
#include "zp_proto.h"
#include "zt.h"

static void ext_happy_path(void)
{
    ps_machine m;
    ps_init(&m);
    ZT_EQ_STR(ps_state_name(m.state), "idle");
    ZT_EQ_INT(ps_source(&m, 0), POS_UNKNOWN);
    uint32_t g = ps_load(&m, 1000, 0, 120000, true);
    ZT_EQ_INT(g, 1);
    ZT_EQ_INT(m.state, PS_LOADING);
    ZT_EQ_INT(ps_position(&m, 5000), 0); /* clock not running until loaded */
    ZT_CHECK(ps_on_loaded(&m, 2000, 119500));
    ZT_EQ_INT(m.state, PS_PLAYING);
    ZT_EQ_INT(m.duration_ms, 119500);
    ZT_EQ_INT(ps_position(&m, 12000), 10000);
    ZT_EQ_INT(ps_source(&m, 12000), POS_ESTIMATED); /* no status yet */
    ZT_CHECK(ps_on_status(&m, 12500, ZP_ST_PLAYING, 10300, 119500));
    ZT_EQ_INT(ps_source(&m, 13000), POS_VERIFIED);
    ZT_EQ_INT(ps_position(&m, 13000), 10800);
    ZT_EQ_INT(ps_source(&m, 12500 + PS_VERIFIED_FRESH_MS + 1), POS_ESTIMATED); /* stale telemetry degrades */
    ZT_EQ_INT(ps_pause(&m, 14000), 0);
    ZT_EQ_INT(m.state, PS_PAUSED);
    ZT_EQ_INT(ps_position(&m, 60000), 11800); /* frozen while paused */
    ZT_EQ_INT(ps_resume(&m, 60000), 0);
    ZT_EQ_INT(ps_position(&m, 61000), 12800);
    ZT_EQ_INT(ps_seek(&m, 61000, 999999), 0);
    ZT_EQ_INT(ps_position(&m, 61000), 119500); /* clamped to duration */
    ZT_EQ_INT(ps_seek(&m, 61000, 30000), 0);
    ZT_EQ_INT(ps_source(&m, 61000), POS_ESTIMATED); /* commanded seek is not a measurement */
    ZT_CHECK(ps_on_ended(&m, 70000));
    ZT_EQ_INT(m.state, PS_ENDED);
    ZT_EQ_INT(m.end_reason, PS_END_EOF);
    ZT_EQ_INT(ps_position(&m, 80000), 119500);
    ZT_EQ_INT(ps_source(&m, 80000), POS_VERIFIED);
}

static void stale_ended_ext(void)
{
    ps_machine m;
    ps_init(&m);
    ps_load(&m, 0, 0, 10000, true);
    ps_on_loaded(&m, 100, 10000);
    /* replace: first item finished as REPLACED, second loading */
    ZT_CHECK(ps_finish(&m, 5000, PS_END_REPLACED, NULL));
    ZT_EQ_INT(m.state, PS_STOPPED);
    ps_load(&m, 5000, 0, 20000, true);
    /* an `ended` belonging to item 1 arrives before `loaded` of item 2 */
    ZT_CHECK(!ps_on_ended(&m, 5001));
    ZT_EQ_INT(m.state, PS_LOADING);
    ZT_CHECK(ps_on_loaded(&m, 5100, 20000));
    ZT_CHECK(ps_on_ended(&m, 25100));
    ZT_EQ_INT(m.generation, 2);
}

static void stock_mode(void)
{
    ps_machine m;
    ps_init(&m);
    ps_load(&m, 1000, 30000, 100000, false);
    ZT_CHECK(!ps_on_loaded(&m, 1100, 0)); /* stock never confirms */
    ZT_CHECK(!ps_on_ended(&m, 1200));     /* quarantine: previous item's EOF */
    ZT_CHECK(ps_assume_started(&m, 3000));
    ZT_EQ_INT(m.state, PS_PLAYING);
    ZT_EQ_INT(ps_position(&m, 4000), 31000);
    ZT_EQ_INT(ps_source(&m, 4000), POS_ESTIMATED);
    ZT_CHECK(!ps_on_status(&m, 4000, ZP_ST_PLAYING, 1, 2)); /* ignored without extensions */
    /* stderr failure after assumption is still accepted */
    ZT_CHECK(ps_on_load_error(&m, 5000, PS_END_LOAD_FAILED, "nope"));
    ZT_EQ_INT(m.state, PS_ERROR);
    ZT_EQ_STR(m.error, "nope");
}

static void errors_and_crash(void)
{
    ps_machine m;
    ps_init(&m);
    ps_load(&m, 0, 0, 0, true);
    ZT_CHECK(ps_on_load_error(&m, 10, PS_END_LOAD_FAILED, "bad codec"));
    ZT_EQ_INT(m.state, PS_ERROR);
    ZT_CHECK(!m.started);
    ZT_EQ_INT(ps_source(&m, 20), POS_UNKNOWN);
    ZT_EQ_INT(ps_pause(&m, 20), -1); /* invalid in ERROR */
    ZT_EQ_INT(ps_seek(&m, 20, 5), -1);

    ps_load(&m, 100, 0, 50000, true);
    ps_on_loaded(&m, 200, 50000);
    ZT_CHECK(ps_finish(&m, 10200, PS_END_CRASH, "crashed"));
    ZT_EQ_INT(m.state, PS_ERROR);
    ZT_EQ_INT(m.end_reason, PS_END_CRASH);
    ZT_EQ_INT(ps_position(&m, 20000), 10000); /* frozen at detection */
    ZT_CHECK(!ps_on_ended(&m, 20000));        /* a crash is never completion */
    ZT_CHECK(!ps_finish(&m, 20000, PS_END_USER_STOP, NULL));

    ps_load(&m, 0, 0, 0, true);
    ps_on_loaded(&m, 0, 0); /* unknown duration: no clamping */
    ZT_EQ_INT(ps_position(&m, 3600000), 3600000);
    ZT_CHECK(ps_finish(&m, 3600000, PS_END_USER_STOP, NULL));
    ZT_EQ_INT(m.state, PS_STOPPED);
    ZT_EQ_INT(ps_pause(&m, 0), -1);
    ZT_EQ_INT(ps_resume(&m, 0), -1);
}

static void external_pause_status(void)
{
    ps_machine m;
    ps_init(&m);
    ps_load(&m, 0, 0, 100000, true);
    ps_on_loaded(&m, 0, 100000);
    ZT_CHECK(ps_on_status(&m, 1000, ZP_ST_PAUSED, 900, 100000));
    ZT_EQ_INT(m.state, PS_PAUSED);
    ZT_EQ_INT(ps_position(&m, 9000), 900);
    ZT_CHECK(!ps_on_status(&m, 9000, ZP_ST_IDLE, 0, 0)); /* idle ignored; `ended` will follow */
    ZT_EQ_INT(m.state, PS_PAUSED);
}

int main(void)
{
    ZT_RUN(ext_happy_path);
    ZT_RUN(stale_ended_ext);
    ZT_RUN(stock_mode);
    ZT_RUN(errors_and_crash);
    ZT_RUN(external_pause_status);
    ZT_DONE();
}
