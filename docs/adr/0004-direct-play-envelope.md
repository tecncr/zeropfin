# ADR 0004 — Direct play only, conservative H.264 envelope, refuse everything else

Status: accepted (2026-11-18)

## Decision
`PlaybackInfo` is requested with `EnableDirectPlay=true`, `EnableDirectStream=false`,
`EnableTranscoding=false` and a device profile advertising only H.264 in MP4/M4V/MOV/MKV with
AAC/MP3/AC3/FLAC/Opus. Zeropfin then evaluates each MediaSource itself (`compat.c`) against what
ZeroPlay will actually decode: first H.264 stream (cover-art mjpeg/png skipped), profile in
Baseline/Constrained Baseline/Main/Extended/High, 8-bit 4:2:0 only, level ≤ 4.1, ≤ 1920×1080,
≤ 30 fps (+0.5), source bitrate ≤ 12 Mbps, `File` protocol, `VideoFile` type, and the **first**
audio track's codec in the allowlist. The first playable source wins. Otherwise the user sees every
reason (e.g. "The first audio track is DTS…"). All limits are configurable.

The first embedded *text* subtitle stream is reported (and shown automatically by ZeroPlay);
bitmap subtitles are ignored. External subtitles are not supported.

## Not done (later milestone)
Server transcoding/remuxing, HLS, audio-track selection.
