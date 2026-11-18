#include "player_state.h"

#include <string.h>

#include "util.h"
#include "zp_proto.h"

void ps_init(ps_machine *m)
{
    memset(m, 0, sizeof *m);
    m->state = PS_IDLE;
}

const char *ps_state_name(ps_state s)
{
    switch (s) {
    case PS_IDLE: return "idle";
    case PS_LOADING: return "loading";
    case PS_PLAYING: return "playing";
    case PS_PAUSED: return "paused";
    case PS_ENDED: return "ended";
    case PS_STOPPED: return "stopped";
    case PS_ERROR: return "error";
    }
    return "unknown";
}

const char *ps_source_name(ps_pos_source s)
{
    switch (s) {
    case POS_VERIFIED: return "verified";
    case POS_ESTIMATED: return "estimated";
    default: return "unknown";
    }
}

bool ps_is_active(const ps_machine *m)
{
    return m->state == PS_LOADING || m->state == PS_PLAYING || m->state == PS_PAUSED;
}

static int64_t clamp_pos(const ps_machine *m, int64_t pos)
{
    if (pos < 0)
        pos = 0;
    if (m->duration_ms > 0 && pos > m->duration_ms)
        pos = m->duration_ms;
    return pos;
}

int64_t ps_position(const ps_machine *m, int64_t now)
{
    if (m->state == PS_IDLE)
        return 0;
    if (m->state == PS_ENDED && m->duration_ms > 0)
        return m->duration_ms;
    int64_t pos = m->anchor_pos_ms;
    if (m->clock_running && now > m->anchor_mono_ms)
        pos += now - m->anchor_mono_ms;
    return clamp_pos(m, pos);
}

ps_pos_source ps_source(const ps_machine *m, int64_t now)
{
    switch (m->state) {
    case PS_IDLE:
    case PS_LOADING: return POS_UNKNOWN;
    case PS_ENDED: return m->duration_ms > 0 ? POS_VERIFIED : POS_UNKNOWN;
    case PS_PLAYING:
    case PS_PAUSED:
        if (m->ext && m->ever_verified && now - m->last_verified_mono <= PS_VERIFIED_FRESH_MS)
            return POS_VERIFIED;
        return POS_ESTIMATED;
    case PS_STOPPED:
    case PS_ERROR: return m->started ? POS_ESTIMATED : POS_UNKNOWN;
    }
    return POS_UNKNOWN;
}

/* Re-anchor the estimator at the current position. */
static void freeze(ps_machine *m, int64_t now)
{
    m->anchor_pos_ms = ps_position(m, now);
    m->anchor_mono_ms = now;
    m->clock_running = false;
}

uint32_t ps_load(ps_machine *m, int64_t now, int64_t start_ms, int64_t duration_hint_ms, bool ext)
{
    m->generation++;
    m->state = PS_LOADING;
    m->ext = ext;
    m->awaiting_loaded = ext;
    m->duration_ms = duration_hint_ms > 0 ? duration_hint_ms : 0;
    m->duration_from_player = false;
    m->anchor_pos_ms = start_ms > 0 ? start_ms : 0;
    m->anchor_mono_ms = now;
    m->clock_running = false;
    m->ever_verified = false;
    m->last_verified_mono = 0;
    m->load_mono = now;
    m->quarantine_until = ext ? 0 : now + PS_STOCK_QUARANTINE_MS;
    m->started = false;
    m->end_reason = PS_END_NONE;
    m->error[0] = '\0';
    return m->generation;
}

static void start_clock(ps_machine *m, int64_t now)
{
    m->state = PS_PLAYING;
    m->anchor_mono_ms = now;
    m->clock_running = true;
    m->started = true;
}

bool ps_on_loaded(ps_machine *m, int64_t now, int64_t dur_ms)
{
    if (m->state != PS_LOADING || !m->awaiting_loaded)
        return false;
    m->awaiting_loaded = false;
    if (dur_ms > 0) {
        m->duration_ms = dur_ms;
        m->duration_from_player = true;
    }
    m->anchor_pos_ms = clamp_pos(m, m->anchor_pos_ms);
    start_clock(m, now);
    return true;
}

bool ps_assume_started(ps_machine *m, int64_t now)
{
    if (m->state != PS_LOADING || m->ext)
        return false;
    start_clock(m, now);
    return true;
}

bool ps_on_load_error(ps_machine *m, int64_t now, ps_end_reason why, const char *msg)
{
    /* Stock players only reveal failures on stderr, possibly after we
     * assumed the item started; accept that within the load window. */
    if (m->state != PS_LOADING && !(m->state == PS_PLAYING && !m->ext))
        return false;
    if (m->ext && !m->awaiting_loaded)
        return false;
    freeze(m, now);
    m->awaiting_loaded = false;
    m->state = PS_ERROR;
    m->end_reason = why;
    zf_strcpy(m->error, sizeof m->error, msg ? msg : "Playback failed to start");
    return true;
}

bool ps_on_ended(ps_machine *m, int64_t now)
{
    if (m->ext && m->awaiting_loaded)
        return false; /* belongs to the previous item */
    if (!m->ext && now < m->quarantine_until)
        return false; /* stock: probably the previous item's EOF */
    if (m->state != PS_PLAYING && m->state != PS_PAUSED)
        return false;
    freeze(m, now);
    if (m->duration_ms > 0)
        m->anchor_pos_ms = m->duration_ms;
    m->state = PS_ENDED;
    m->end_reason = PS_END_EOF;
    return true;
}

bool ps_on_status(ps_machine *m, int64_t now, int zp_state, int64_t pos_ms, int64_t dur_ms)
{
    if (!m->ext || (m->state != PS_PLAYING && m->state != PS_PAUSED))
        return false;
    if (zp_state != ZP_ST_PLAYING && zp_state != ZP_ST_PAUSED)
        return false; /* idle/image: an `ended` or error event will follow */
    if (dur_ms > 0) {
        m->duration_ms = dur_ms;
        m->duration_from_player = true;
    }
    m->anchor_pos_ms = clamp_pos(m, pos_ms);
    m->anchor_mono_ms = now;
    m->ever_verified = true;
    m->last_verified_mono = now;
    if (zp_state == ZP_ST_PAUSED) {
        m->state = PS_PAUSED;
        m->clock_running = false;
    } else {
        m->state = PS_PLAYING;
        m->clock_running = true;
    }
    return true;
}

int ps_pause(ps_machine *m, int64_t now)
{
    if (m->state == PS_PAUSED)
        return 0;
    if (m->state != PS_PLAYING)
        return -1;
    freeze(m, now);
    m->state = PS_PAUSED;
    return 0;
}

int ps_resume(ps_machine *m, int64_t now)
{
    if (m->state == PS_PLAYING)
        return 0;
    if (m->state != PS_PAUSED)
        return -1;
    m->anchor_mono_ms = now;
    m->clock_running = true;
    m->state = PS_PLAYING;
    return 0;
}

int ps_seek(ps_machine *m, int64_t now, int64_t pos_ms)
{
    if (m->state != PS_PLAYING && m->state != PS_PAUSED)
        return -1;
    if (pos_ms < 0)
        return -1;
    m->anchor_pos_ms = clamp_pos(m, pos_ms);
    m->anchor_mono_ms = now;
    /* A commanded seek is not a measurement: drop to estimated until the
     * player reports again. */
    m->ever_verified = false;
    return 0;
}

bool ps_finish(ps_machine *m, int64_t now, ps_end_reason why, const char *msg)
{
    if (!ps_is_active(m))
        return false;
    freeze(m, now);
    m->awaiting_loaded = false;
    m->end_reason = why;
    if (why == PS_END_CRASH || why == PS_END_LOAD_FAILED || why == PS_END_LOAD_TIMEOUT) {
        m->state = PS_ERROR;
        zf_strcpy(m->error, sizeof m->error, msg ? msg : "Playback failed");
    } else {
        m->state = PS_STOPPED;
    }
    return true;
}
