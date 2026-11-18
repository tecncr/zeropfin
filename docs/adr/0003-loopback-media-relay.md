# ADR 0003 — Loopback authenticated media relay instead of tokens in player URLs

Status: accepted (2026-11-18); performance NEEDS HARDWARE VALIDATION

## Context
ZeroPlay opens URLs through FFmpeg and has no per-request header API in control mode. Putting the
Jellyfin token in the URL (`api_key=`) would expose it in argv-like command logs, ZeroPlay's stderr
and Jellyfin access logs, and Jellyfin 12 disables query-string tokens by default.

## Decision
ZeroPlay receives `http://127.0.0.1:<ephemeral>/media/<128-bit random id>`. The relay (`relay.c`):
binds 127.0.0.1 only and rechecks the peer; maps the id to one server-relative path chosen by the
daemon (`/Videos/{id}/stream?static=true&mediaSourceId=…&deviceId=…&playSessionId=…`); adds the
MediaBrowser `Authorization` header itself; passes single byte ranges through (200/206/416,
`Content-Range`, `Content-Length`, `Accept-Ranges`; multi-range requests get a full 200); follows
redirects manually and only to the configured Jellyfin origin; streams through a 128 KiB ring per
reader with curl pause/unpause backpressure; resumes transparently with a Range request if the
upstream drops mid-transfer (e.g. Kestrel's minimum data-rate timeout while the player is paused);
and revokes the grant when the item stops, ends, fails or is replaced. Relay ids are scrubbed from
logs (including libmicrohttpd's own messages).

## Evidence (x86_64 host, mock upstream)
`tests/integration/run.py`: exact bytes for ranges and suffix ranges, 416 passthrough, HEAD,
multi-range fallback, 51 MiB full transfer, resume after an injected drop at 5 MiB with byte-exact
output, aborted client releases its reader, same-origin redirect followed, cross-origin refused,
revocation after stop. Release build: 55 MB/s on the dev host; daemon RSS 14 MiB with +60 KiB growth
while streaming 51 MiB.

## Open question
CPU cost on the Pi 1B at 8–12 Mbps (one extra copy per byte through loopback). If measurements
show it is too expensive, propose a narrow upstream ZeroPlay option to pass an
`Authorization` header to FFmpeg (`headers` AVOption) via stdin — never fall back to tokens in URLs.
