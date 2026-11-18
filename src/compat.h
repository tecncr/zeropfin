/* compat.h — direct-play eligibility of a Jellyfin MediaSource for ZeroPlay.
 *
 * Mirrors ZeroPlay's real stream selection (demux.c): the FIRST H.264 video
 * stream, the FIRST audio stream and the FIRST embedded subtitle stream are
 * used; there is no track selection. Anything outside the configured
 * conservative envelope is rejected with a user-facing reason. Pure: only
 * reads cJSON, so it is exercised directly by unit tests. */
#ifndef ZF_COMPAT_H
#define ZF_COMPAT_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define ZF_COMPAT_MAX_MSG 6

typedef struct {
    bool playable;
    int n_reasons, n_warnings;
    char reasons[ZF_COMPAT_MAX_MSG][192];
    char warnings[ZF_COMPAT_MAX_MSG][192];

    char media_source_id[64];
    char container[32];
    char video_codec[24];
    char video_profile[40];
    int width, height, level, bit_depth;
    double fps;
    int64_t bitrate;
    int audio_index; /* -1 = no audio */
    char audio_codec[24];
    int audio_channels;
    char audio_label[96];
    int subtitle_index; /* first embedded subtitle shown by ZeroPlay, -1 = none */
    char subtitle_label[96];
} zf_compat;

void zf_compat_evaluate(const cJSON *media_source, const zf_config *cfg, zf_compat *out);
/* Evaluate every MediaSource in an array; returns the index of the first
 * playable one (filling out), or -1 with `out` describing the first source. */
int zf_compat_choose(const cJSON *media_sources, const zf_config *cfg, zf_compat *out);
cJSON *zf_compat_to_json(const zf_compat *c);

#endif
