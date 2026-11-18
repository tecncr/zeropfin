/* player_state.h — pure playback state machine + position estimator.
 *
 *   IDLE ─load→ LOADING ─loaded/assumed→ PLAYING ⇄ PAUSED
 *                  │                        │  │
 *                  └─load error/timeout→ ERROR ←─ child crash
 *   PLAYING/PAUSED ─ended(EOF)→ ENDED     any active ─stop→ STOPPED
 *   any state ─load→ LOADING (generation+1)
 *
 * Positions are milliseconds. Source is VERIFIED only when the player
 * itself reported it recently (ZeroPlay fork `status` extension); otherwise
 * the monotonic estimate is labelled ESTIMATED. No function here performs
 * I/O, so every transition is unit-testable. */
#ifndef ZF_PLAYER_STATE_H
#define ZF_PLAYER_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { PS_IDLE, PS_LOADING, PS_PLAYING, PS_PAUSED, PS_ENDED, PS_STOPPED, PS_ERROR } ps_state;
typedef enum { POS_UNKNOWN, POS_ESTIMATED, POS_VERIFIED } ps_pos_source;
typedef enum {
    PS_END_NONE,
    PS_END_EOF, /* player reported `ended` for the current item */
    PS_END_USER_STOP,
    PS_END_REPLACED, /* a new load superseded the item */
    PS_END_LOAD_FAILED,
    PS_END_LOAD_TIMEOUT,
    PS_END_CRASH, /* player process died: never counts as completion */
    PS_END_SHUTDOWN,
} ps_end_reason;

#define PS_VERIFIED_FRESH_MS   3000 /* status older than this degrades to estimated */
#define PS_STOCK_QUARANTINE_MS 1500 /* stock ZeroPlay: `ended` this soon after a load is stale */

typedef struct {
    ps_state state;
    uint32_t generation;
    bool ext;             /* player supports fork extensions */
    bool awaiting_loaded; /* ext: `ended` before `loaded` belongs to the previous item */
    int64_t duration_ms;  /* 0 = unknown */
    bool duration_from_player;
    int64_t anchor_pos_ms;
    int64_t anchor_mono_ms;
    bool clock_running;
    bool ever_verified;
    int64_t last_verified_mono;
    int64_t load_mono;
    int64_t quarantine_until;
    bool started; /* playback actually commenced (start reported) */
    ps_end_reason end_reason;
    char error[160];
} ps_machine;

void ps_init(ps_machine *m);
const char *ps_state_name(ps_state s);
const char *ps_source_name(ps_pos_source s);
bool ps_is_active(const ps_machine *m); /* LOADING, PLAYING or PAUSED */

/* Begin a new item. Returns the new generation. Callers must first finish
 * any active item (ps_finish with PS_END_REPLACED). */
uint32_t ps_load(ps_machine *m, int64_t now, int64_t start_ms, int64_t duration_hint_ms, bool ext);
/* ext: player confirmed the load. Returns true if it applied. */
bool ps_on_loaded(ps_machine *m, int64_t now, int64_t dur_ms);
/* stock: no confirmation exists; assume playback started (estimated). */
bool ps_assume_started(ps_machine *m, int64_t now);
bool ps_on_load_error(ps_machine *m, int64_t now, ps_end_reason why, const char *msg);
/* Returns true if the `ended` event was attributed to the current item. */
bool ps_on_ended(ps_machine *m, int64_t now);
bool ps_on_status(ps_machine *m, int64_t now, int zp_state, int64_t pos_ms, int64_t dur_ms);
int ps_pause(ps_machine *m, int64_t now);
int ps_resume(ps_machine *m, int64_t now);
int ps_seek(ps_machine *m, int64_t now, int64_t pos_ms);
/* Stop/replace/shutdown/crash an active item. Returns true if it was active. */
bool ps_finish(ps_machine *m, int64_t now, ps_end_reason why, const char *msg);

int64_t ps_position(const ps_machine *m, int64_t now);
ps_pos_source ps_source(const ps_machine *m, int64_t now);

#endif
