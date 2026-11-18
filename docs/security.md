# Security model

Zeropfin is a **LAN appliance**. Do not port-forward it or expose it to the Internet. If you need
remote access, use a VPN or an authenticated TLS reverse proxy you control.

## Assets and boundaries

| Asset | Where it lives | Who can read it |
|---|---|---|
| Jellyfin access token | `/var/lib/zeropfin/credentials.json` (0600, service user; refused if group/other readable) and daemon memory | daemon only; sent only in the `Authorization` header to the configured Jellyfin origin |
| Jellyfin password | nowhere — typed once into `zeropfin setup`, exchanged for a token, wiped from memory | – |
| Remote sessions | browser cookie (HttpOnly); SHA-256 hash in `/var/lib/zeropfin/sessions.json` (0600) | browser / daemon |
| Pairing code | daemon memory, journal line, TV OSD when requested | people with SSH/journal access or line of sight to the TV |
| Media relay grants | daemon memory; random 128-bit id in the loopback URL given to ZeroPlay | local processes on the Pi (loopback only) until revoked |

The browser never receives Jellyfin credentials, URLs with credentials, or Jellyfin item paths.
All browser traffic is same-origin to the Pi; the UI makes no third-party requests (enforced by CSP
and tested).

## Controls

- **No unauthenticated control:** every endpoint except `/healthz`, `/api/v1/session` and the two
  pairing endpoints requires a paired session. See `docs/adr/0005-local-pairing-and-sessions.md`.
- **CSRF / cross-site:** SameSite=Strict cookie, `X-CSRF-Token` on all authenticated POSTs,
  Origin must equal Host, JSON content type required (forces CORS preflight; no CORS is granted).
- **DNS rebinding:** Host header allowlist (`421` otherwise).
- **LAN only:** client source address must be private unless `allowed_networks` says otherwise.
- **Brute force:** codes expire in 10 min, single use, 5 attempts per code, 10 failures per minute
  globally, code requests rate-limited.
- **Input bounds:** 16 KiB bodies, strict integer parsing with range checks, id syntax checks,
  pagination caps, item-type/sort allowlists, Jellyfin responses capped (4 MiB JSON, 2 MiB images),
  image content types restricted to JPEG/PNG/WebP/GIF.
- **SSRF:** the browser can only name item ids; the daemon builds all Jellyfin paths. The relay
  serves only daemon-registered paths, refuses redirects to other origins, http/https only.
- **Player command injection:** ZeroPlay commands are formatted by `zp_proto.c`; URLs must be
  `http://127.0.0.1:<port>/…` with `[A-Za-z0-9/._-]` only; OSD text is reduced to printable ASCII
  without backslashes. ZeroPlay is started with `posix_spawn`, an absolute path and an allowlisted
  argument vector — never a shell.
- **TLS:** libcurl verifies certificates and host names; `jellyfin_ca_file` for private CAs.
  Plain `http://` Jellyfin URLs are allowed on a LAN, with a warning during setup.
- **Logs:** URLs are logged without query strings; relay ids are scrubbed (including
  libmicrohttpd's internal messages); tokens are never formatted into log lines; control bytes
  in log arguments are neutralized. The integration test greps all daemon logs for the token.
- **Privileges:** dedicated `zeropfin` user (groups `video`, `render`, `audio`), refuses to run as
  root, systemd hardening (`ProtectSystem=strict`, `ProtectHome`, `NoNewPrivileges`, namespace,
  address-family and personality restrictions, `UMask=0077`). `PrivateDevices` is not used because
  ZeroPlay needs `/dev/dri`, `/dev/video*` and `/dev/snd`.
- **Admin socket:** `/run/zeropfin/control.sock` mode 0600 and SO_PEERCRED (root or service user).

## Known limitations

- Plain HTTP on the LAN: anyone who can sniff the Wi-Fi can steal a session cookie. Use WPA2/3,
  or front Zeropfin with a TLS proxy (cookies get `Secure` when the proxy on 127.0.0.1 sends
  `X-Forwarded-Proto: https`).
- `pairing_show_on_display = true` lets anyone in the room pair. Set it to `false` to require
  `zeropfin pair` over SSH.
- The pairing code appears in the journal (by design, for headless setups).
- Local users on the Pi could fetch an active relay URL if they learn its id (it is not logged).
- Jellyfin itself may log the device name and id; the token is never placed in URLs.
