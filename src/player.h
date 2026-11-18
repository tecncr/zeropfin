/* player.h — ZeroPlay process supervisor and playback controller.
 *
 * One controller thread owns the single `zeroplay --control` child: it is the
 * only writer to the child's stdin, the only reader of stdout/stderr, and
 * the only mutator of the playback state machine. Other threads submit
 * commands through a bounded queue and read snapshots. */
#ifndef ZF_PLAYER_H
#define ZF_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "player_state.h"
#include "zp_proto.h"

typedef struct {
    char item_id[33];
    char media_source_id[64];
    char play_session_id[64];
    char title[256];
    char subtitle[256]; /* e.g. "Series · S1E2" */
    char image_item_id[33];
    char image_tag[64];
    char kind[16];      /* "Movie", "Episode", ... */
    int64_t runtime_ms; /* Jellyfin RunTimeTicks converted; 0 = unknown */
    int64_t start_ms;
    int audio_stream_index; /* -1 = none */
    int subtitle_stream_index;
    char url[ZP_MAX_URL]; /* loopback relay URL handed to ZeroPlay */
    char relay_id[65];
} zf_media;

typedef enum { ZF_REPORT_START, ZF_REPORT_PROGRESS, ZF_REPORT_STOP } zf_report_kind;

typedef struct {
    zf_report_kind kind;
    char item_id[33];
    char media_source_id[64];
    char play_session_id[64];
    int audio_stream_index;
    int subtitle_stream_index;
    bool has_position;
    int64_t position_ticks;
    bool paused;
    bool failed;            /* stop reports: abnormal end (crash/load failure) */
    const char *event_name; /* progress: "timeupdate", "pause", "unpause", ... */
} zf_report;

typedef struct {
    void *ctx;
    void (*report)(void *ctx, const zf_report *r);  /* must not block */
    void (*released)(void *ctx, const zf_media *m); /* revoke relay grant */
} zf_player_hooks;

typedef enum { ZF_PROC_DOWN, ZF_PROC_STARTING, ZF_PROC_PROBING, ZF_PROC_READY, ZF_PROC_BACKOFF } zf_proc_state;

typedef struct {
    ps_state state;
    zf_proc_state proc;
    bool ext_events;
    bool osd_supported;
    uint32_t generation;
    int64_t position_ms;
    int64_t duration_ms;
    ps_pos_source source;
    bool has_media;
    zf_media media; /* url/relay_id are cleared in snapshots */
    char error[160];
    char player_error[160];
    int restarts;
} zf_player_snapshot;

typedef struct zf_player zf_player;

zf_player *zf_player_new(const zf_config *cfg, zf_player_hooks hooks);
int zf_player_start(zf_player *p);
void zf_player_shutdown(zf_player *p); /* stops playback, quits the child, joins */
void zf_player_free(zf_player *p);

/* Commands return 0 when queued, -1 if invalid for the current state,
 * -2 if the command queue is full. */
int zf_player_load(zf_player *p, const zf_media *m);
int zf_player_pause(zf_player *p);
int zf_player_resume(zf_player *p);
int zf_player_seek(zf_player *p, int64_t pos_ms);
int zf_player_seek_relative(zf_player *p, int64_t delta_ms);
int zf_player_stop(zf_player *p);
int zf_player_osd(zf_player *p, const char *text, int duration_ms);

void zf_player_snapshot_get(zf_player *p, zf_player_snapshot *out);
const char *zf_proc_state_name(zf_proc_state s);

#endif
