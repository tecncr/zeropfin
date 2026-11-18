# Zeropfin architecture

Zeropfin turns a Raspberry Pi 1 / Zero with a composite CRT into a Jellyfin playback receiver.
A phone browser is the remote; the Pi fetches and plays the media itself through ZeroPlay.

```text
Safari (static HTML/CSS/JS, same origin)
   │  HTTP :8080 — pairing cookie + CSRF; JSON; polling
   ▼
zeropfin daemon (C11) ─────────────── Jellyfin REST (libcurl, MediaBrowser Authorization header)
   │  http.c  LAN listener (1 MHD event thread, suspend/resume)          ▲
   │  api.c   routes → workers.c (2 threads, queue 32) ─────────────────┤ browse, images, PlaybackInfo
   │  reporter.c (1 thread, bounded queue) ─────────────────────────────┤ /Sessions/Playing{,/Progress,/Stopped}
   │  player.c  controller thread: state machine + child supervision     │
   │     │ stdin/stdout/stderr pipes (zp_proto.c)                        │
   │     ▼                                                               │
   │  zeroplay --control  (child, own process group)                     │
   │     │ GET/Range http://127.0.0.1:<eph>/media/<128-bit id>           │
   │     ▼                                                               │
   └─ relay.c  loopback-only MHD, ≤4 readers, 128 KiB ring each ─────────┘ /Videos/{id}/stream?static=true (Range)
ZeroPlay → V4L2 M2M H.264 decode → DRM/KMS composite → ALSA analog audio → CRT
```

Browser control is **not** Jellyfin native casting: the Jellyfin apps cannot see or control
Zeropfin as a cast target. That (Jellyfin's session WebSocket protocol) is a separate later milestone.

## Source layout

| File | Responsibility |
|---|---|
| `src/main.c` | CLI: `serve` (default), `setup`, `pair`, `sessions`, `revoke`, `reload`, `status`, `--version` |
| `src/app.c` | lifecycle, signals (`sigwait`), `sd_notify` without libsystemd, ordered shutdown |
| `src/config.c` | strict `key = value` config; unknown keys are errors; ZeroPlay arg allowlist |
| `src/creds.c` | `credentials.json` (0600, atomic write): server URL/id, user id, token, device id |
| `src/auth.c` + `src/sha256.c` | pairing codes, session cookies (hashed at rest), rate limits |
| `src/netfilter.c` | private-address filter, CIDRs, Host allowlist, Origin check |
| `src/http.c` | MHD LAN server, static files, async plumbing; `src/api.c` routes |
| `src/jellyfin.c` | Jellyfin client, error mapping, item → slim JSON |
| `src/compat.c` | direct-play eligibility (pure) |
| `src/player_state.c` | playback state machine + position estimator (pure) |
| `src/zp_proto.c` | ZeroPlay line protocol: parsing, bounded line buffer, safe command formatting (pure) |
| `src/player.c` | spawning/supervising ZeroPlay, timers, reports |
| `src/relay.c` | loopback relay with Range, redirects, resume, backpressure |
| `src/reporter.c` | Jellyfin playback reports with coalescing and retry |
| `src/ctl.c` | Unix control socket for admin commands |
| `src/setup.c` | interactive sign-in storing only the token |
| `web/` | Vite + Tailwind v4 + Alpine CSP remote (built off-device) |
| `tests/` | unit tests, mock ZeroPlay, fake Jellyfin, end-to-end runner |

The layout follows AGENTS.md's suggestion loosely; `jellyfin_media`/`jellyfin_session` became
`compat.c`/`reporter.c`, `local_auth` became `auth.c`, `http_routes` became `api.c`, and a
`workers.c`, `ctl.c` and `netfilter.c` were added.

## Playback flow

1. `POST /player/play {itemId}` → worker: `GET /Items/{id}`, `POST /Items/{id}/PlaybackInfo`
   (direct play only), `compat.c` picks the first eligible MediaSource or returns `422` with reasons.
2. The relay registers `/Videos/{id}/stream?static=true&mediaSourceId&deviceId&playSessionId`
   under a random id; the controller gets the loopback URL.
3. Controller: if something is active, report `Stopped` for it, revoke its relay grant, send `stop`;
   then `loadat <ms> <url>` (fork) or `load <url>` + `seek <ms>` (stock).
4. `loaded` (fork) or the 2 s stock assumption → state `playing`, `Sessions/Playing` reported.
   Progress every `progress_interval_s` (default 10 s) and on pause/unpause/seek.
5. `ended` for the current generation → `ended`, `Stopped` with the full runtime (Jellyfin marks it
   played). Stop/replace → `Stopped` at the current position. Crash → `error`, `Stopped` with
   `Failed: true` and the last trustworthy position, player restarted with backoff.

## Position accuracy

| ZeroPlay | Source | Accuracy |
|---|---|---|
| fork with `status` | `verified` | presented-frame PTS reported by the player, polled at 1 Hz; degrades to `estimated` if older than 3 s or right after a seek |
| stock upstream | `estimated` | monotonic clock from the commanded start/seek, frozen on pause; start time is ~2 s heuristic; network stalls/buffering are invisible |

Residual ambiguities (stock only): a load failure is learned from stderr text; an `ended` that
arrives more than 1.5 s after a replacing `load` is treated as the new item's EOF; buffering time is
counted as playback. With the fork, generation ordering is exact because ZeroPlay writes events in
command order and every load is acknowledged with `loaded` or `error load`.

## Memory and resource bounds (code-level limits)

HTTP bodies 16 KiB; per-connection MHD pool 32 KiB; 64 LAN connections (16/IP); Jellyfin JSON
responses 4 MiB; images 2 MiB; worker queue 32; report queue 16 (progress coalesced); relay: 4
upstream readers × 128 KiB ring; ZeroPlay protocol lines 4 KiB; command buffer 8 KiB; 32 sessions;
4 relay grants. Only pairing/logout/revoke and `zeropfin setup` write to the SD card.
