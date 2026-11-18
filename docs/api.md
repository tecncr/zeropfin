# Zeropfin local HTTP API (v1)

Served by the daemon on the LAN listener (default `:8080`). Same-origin only; this is **not** the
Jellyfin API and never exposes Jellyfin credentials. Implemented in `src/api.c` and `src/http.c`;
exercised end to end by `tests/integration/run.py`.

## Conventions

- JSON in and out (`application/json; charset=utf-8`), `Cache-Control: no-store`.
- Errors: `{"error": {"code": "...", "message": "...", "retryable": bool}}` with a matching HTTP
  status. `422 incompatible` additionally carries `error.playback` (see *Playback eligibility*).
- Every request: the client address must be private (loopback, RFC 1918, link-local, 100.64/10,
  IPv6 ULA/link-local) or in `allowed_networks` → else `403` (plain text). The `Host` header must be
  an IP literal, `localhost`, the machine hostname (`.local` too) or in `allowed_hosts` → else `421`.
- Authenticated endpoints need the `zf_session` cookie → else `401 unauthenticated`.
- `POST` requests need `Content-Type: application/json` (`415`), an `Origin` equal to
  `http(s)://<Host>` when present (`403 bad_origin`), and—except the pairing endpoints—
  `X-CSRF-Token: <csrfToken>` (`403 csrf`). Bodies are capped at 16 KiB (`413 too_large`).
- Jellyfin-backed endpoints run on a bounded worker pool; when it is full: `503 busy` (retryable).
  Jellyfin failures map to `502 jellyfin_unreachable` (retryable), `502 jellyfin_auth`,
  `403 forbidden`, `404 not_found`, `503 not_configured`.

## Public

| Method | Path | Response |
|---|---|---|
| GET | `/healthz` | `{"ok":true}` (no auth, no details) |
| GET | `/api/v1/session` | `{"authenticated": bool, "csrfToken"?: string, "pairingOnDisplay": bool}` |
| POST | `/api/v1/pairing/request` `{}` | `{"displayRequested": bool, "expiresInSec": 600}`; new one-time code shown on the TV (if enabled) and logged; `429 rate_limited` within 5 s |
| POST | `/api/v1/pairing/complete` `{"code": "ABCD-EFGH", "deviceName"?: string}` | `200 {"csrfToken"}` + `Set-Cookie: zf_session=…; HttpOnly; SameSite=Strict`; `401 invalid_code`; `429 rate_limited` |

## Session

| POST | `/api/v1/session/logout` `{}` | `{}`, cookie cleared, session deleted |
|---|---|---|

## Status and player

`GET /api/v1/status`
```json
{"version": "development build",
 "jellyfin": {"configured": true, "serverName": "Home", "serverVersion": "10.10.7", "userName": "demo",
              "reachable": true, "error": "optional"},
 "player": { …player state… }}
```

`GET /api/v1/player` → player state:
```json
{"state": "idle|loading|playing|paused|ended|stopped|error",
 "positionMs": 612345, "durationMs": 4800000,
 "positionSource": "verified|estimated|unknown",
 "generation": 3, "error": null,
 "player": {"status": "down|starting|ready|restarting", "statusEvents": true, "restarts": 0, "error"?: "…"},
 "item": {"id": "…", "title": "…", "subtitle": "Series · S1E2 | 2019", "type": "Movie|Episode|…",
          "runtimeMs": 4800000, "poster"?: {"id", "type", "tag"}} | null}
```
- `verified`: reported by ZeroPlay (`status` extension) within the last 3 s.
- `estimated`: monotonic-clock estimate anchored at the last load/seek/pause/status.
- `ended` only follows a real end-of-file event for the current item; a player crash yields
  `error` with a message.

| Method | Path / body | Result |
|---|---|---|
| POST | `/api/v1/player/play` `{"itemId", "resume"?: bool = true, "startMs"?: int}` | `202 {"playback": {…eligibility…}, "startMs"}`; `422 incompatible` (with reasons) / `not_playable`; `403 not_allowed` |
| POST | `/api/v1/player/pause` `{}` | `202` player state; `409 invalid_state` unless playing/paused |
| POST | `/api/v1/player/resume` `{}` | as above |
| POST | `/api/v1/player/seek` `{"positionMs": int}` or `{"deltaMs": int}` | exactly one field; integers; `409` unless playing/paused |
| POST | `/api/v1/player/stop` `{}` | `202` |

Commands are accepted asynchronously; poll `/api/v1/player` (the UI does: 1 s while active).

## Library (Jellyfin proxy, trimmed responses)

Item summary: `{"id", "name", "type", "collectionType"?, "year"?, "seriesName"?, "seriesId"?,
"seasonId"?, "index"?, "parentIndex"?, "runtimeMs"?, "isFolder", "played"?, "playedPct"?,
"resumeMs"?, "unplayed"?, "childCount"?, "poster"?, "thumb"?, "backdrop"?, "overview"?}`, images as
`{"id", "type": "Primary|Thumb|Backdrop", "tag"}`. Lists: `{"items": [...], "total"?, "start"}`.

| GET | Query | Jellyfin call |
|---|---|---|
| `/api/v1/views` | – | `/UserViews` (video libraries only) |
| `/api/v1/items` | `parentId`, `search` (≤100 chars), `types` (Movie,Series,Episode,Video,MusicVideo,BoxSet), `sort` (name, added, released, rating), `filter=unplayed`, `start`, `limit` ≤ 60 | `/Items?recursive=true…` |
| `/api/v1/resume` | – | `/UserItems/Resume` (video) |
| `/api/v1/nextup` | – | `/Shows/NextUp` |
| `/api/v1/latest` | `parentId` | `/Items/Latest` |
| `/api/v1/items/{id}` | – | `/Items/{id}`; adds `overview`, `genres`, `officialRating`, `communityRating`, `playback` |
| `/api/v1/items/{id}/seasons` | – | `/Shows/{id}/Seasons` |
| `/api/v1/items/{id}/episodes` | `seasonId`, `start`, `limit` ≤ 100 | `/Shows/{id}/Episodes` (with overview) |
| `/api/v1/images/{id}/{Primary\|Thumb\|Backdrop}` | `w` 40–1920, `tag` | image bytes (JPEG/PNG/WebP/GIF only, ≤ 2 MiB), `Cache-Control: private, max-age=604800, immutable` |

Ids are 32 hex digits (dashed GUIDs accepted and normalized); anything else → `404`/`400`.

## Playback eligibility object
```json
{"playable": false, "method": "DirectPlay|None",
 "reasons": ["Video codec HEVC is not supported; ZeroPlay decodes H.264 only."],
 "warnings": ["Only the first audio track (English - AAC - Stereo) can be played."],
 "media": {"container": "mkv", "videoCodec": "h264", "videoProfile": "High", "width": 1920, "height": 1080,
           "level": 41, "fps": 23.98, "bitrate": 8000000, "audioCodec": "aac", "audioChannels": 2,
           "audio": "…", "subtitles": "…"}}
```

## Static files
`GET /` and `/<path>` from `web_root`: strict path allowlist (no dot-segments or hidden files),
`*.gz` siblings served when the client accepts gzip, `/assets/*` immutable for a year, everything
else `no-cache`, strict CSP, `nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`.

## Not on the LAN listener
The media relay (`/media/<id>`) exists only on `127.0.0.1:<ephemeral>` for ZeroPlay
(see `docs/architecture.md`). The admin control socket is a Unix socket (`zeropfin pair|sessions|
revoke|reload|status`).
