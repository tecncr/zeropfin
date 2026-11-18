# ADR 0005 — Local pairing codes, hashed session cookies, CSRF/Origin/Host defenses

Status: accepted (2026-11-18)

## Decision
- A paired browser never sees Jellyfin credentials. Pairing uses a one-time 8-character code
  (31-symbol unambiguous alphabet, ~39.6 bits, rejection-sampled from getrandom) valid 10 min,
  burned after 5 wrong guesses, with a global limit of 10 failures/min. Codes come from
  `zeropfin pair` (control socket, SO_PEERCRED root/service-user only), and on request from the web
  UI are shown on the TV through ZeroPlay's `osd` command (configurable) and logged to the journal.
  Anyone who can see the TV can pair: that is the accepted trust model for a living-room device.
- Sessions: 256-bit random cookie `zf_session` (HttpOnly, SameSite=Strict, Path=/, 30 days,
  `Secure` when a local TLS proxy sets `X-Forwarded-Proto: https`). Only SHA-256 hashes persist in
  `/var/lib/zeropfin/sessions.json`, written only on pair/logout/revoke. Max 32 sessions.
- State-changing requests need `Content-Type: application/json`, a matching `Origin` (when sent),
  and `X-CSRF-Token` equal to the session's CSRF token (not needed for the two pairing endpoints).
- Host header allowlist (IP literals, localhost, `<hostname>`, `<hostname>.local`, configured
  names) defeats DNS rebinding; client addresses must be loopback/RFC1918/link-local/CGNAT/ULA
  unless configured otherwise.
