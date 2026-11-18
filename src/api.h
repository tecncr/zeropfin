/* api.h — /api/v1 routes (documented in docs/api.md). */
#ifndef ZF_API_H
#define ZF_API_H

#include "http.h"

/* Route a request under /api/. Produces a response synchronously or goes
 * asynchronous via zf_req_go_async. */
void zf_api_handle(zf_req *r);

/* Exposed for tests: build the player state JSON from a snapshot. */
cJSON *zf_api_player_json(const zf_player_snapshot *s);

#endif
