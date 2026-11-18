# AGENTS.md — Zeropfin

> Instructions for AI coding agents working in this repository. This is an implementation specification, not a statement that the product already exists.

> Project: **Zeropfin**. Binary and CLI name: **`zeropfin`**. Canonical repository: **https://github.com/tecncr/zeropfin**.

## Project identity and agent discretion

- **Product name:** Zeropfin (case-sensitive branding in user-facing copy, documentation, and project headings).
- **CLI and executable:** `zeropfin` (lowercase); e.g., `zeropfin setup`, `zeropfin --help`. Name the systemd unit `zeropfin.service` unless established distribution policy gives a compelling reason to deviate. Use `zeropfin` for packaging, paths, and project-owned namespaces when practical.
- **Canonical GitHub repository:** https://github.com/tecncr/zeropfin. Use this URL in project metadata and docs; **do not assume that the remote exists, is accessible, or that pushing is authorized**.
- **Architecture and examples in this file are intentionally *proposed*.** The suggested directory tree, C module boundaries, file names (other than the product/binary identity), HTTP endpoints, JSON payloads, and implementation sequencing are **not binding**. An agent **may redesign them at its discretion** when it can achieve a simpler, safer, more maintainable, or more testable implementation. Document the final structure and actual API contracts, adjust tests and frontend consistently, and briefly explain any major departure.
- **Do not confuse flexibility with waived requirements.** Hardware/ARMv6 support, native low-memory C runtime, ZeroPlay as player, security boundaries, Jellyfin semantics, browser-only remote, and the mandated visual design below remain project requirements. Avoid asking permission merely to reorganize a proposed layout or route.

## 1. Mission

Create a genuinely small, reliable, single-device **Jellyfin playback receiver** for the original Raspberry Pi 1 Model B (including the 256 MiB revision) and Raspberry Pi Zero/Zero W. A separate phone/tablet/computer acts as the remote control through a pleasant, responsive web application. The Pi runs a native C daemon, manages Jellyfin sessions, and hands actual video and audio playback to the existing **ZeroPlay** executable. Target a 4:3 analog CRT connected by composite video and analog audio; do not assume an HDMI display.

The user has **personally confirmed ZeroPlay works on their Raspberry Pi 1B**. Do not spend this project trying to replace it, implement another hardware decoder, or prove that ARMv6 video decode exists. Verify its *control-mode and network playback behavior* on the installed version instead.

Success looks like: open `http://<pi-address>:<configured-port>` from iPhone Safari, sign in to the local remote, browse/search a Jellyfin library, choose a compatible movie/episode, and control the ZeroPlay session displayed on the CRT, with pause/resume/seek/stop, usable progress and honest limitations, and Jellyfin playback status/resume tracking. The iPhone never carries or transcodes the video stream.

## 2. Non-negotiable constraints

1. **Target baseline:** original single-core ARM1176JZF-S / ARMv6 / BCM2835, **256 MiB total RAM**. Raspberry Pi Zero W (ARMv6, usually 512 MiB) is the secondary target. Do not accidentally require ARMv7, ARMv8, NEON, 64-bit, or instructions unavailable on the Pi 1.
2. **OS:** Raspberry Pi OS Lite **32-bit**, with a DRM/KMS + V4L2 environment already known to run ZeroPlay. Do not require X11, Wayland, wlroots, KODI, a desktop session, or a video compositor. ZeroPlay owns the display pipeline.
3. **Runtime:** native C11/POSIX executable, system libraries, ZeroPlay, and prebuilt static frontend files only. **No Node.js/npm, Python, Go, Rust runtime, Vite dev server, backend JavaScript, database service, or browser running on the Pi.** Node/npm/Vite are **allowed and encouraged on developer machines and CI** exclusively.
4. **UI:** Tailwind CSS **v4** + **Alpine.js** (prefer `@alpinejs/csp`); use Vite for development/build. Output plain static assets; no React/Vue/Angular/SSR/hydration framework. **Mandatory visual direction: minimalist monochrome interface (black, white, shades of gray) with Tailwind blue as the sole UI accent, self-hosted Inter as the UI typeface, and absolutely no gradients.** No third-party CDN requests, remotely loaded fonts, or online icons required to render the UI.
5. **Video:** ZeroPlay is a **child process**, not a linked playback engine. Start it with `--control` and communicate through safely managed stdin/stdout pipes. No shell command construction; use `fork`/`exec*` (or an equivalent safe process spawning API). Do not require ZeroPlay's optional WebSocket build.
6. **Media:** initial playback target is H.264/AVC 8-bit in containers/streams verified to work with ZeroPlay, with supported audio; reject other media with clear actionable messaging. No silent software decode on this CPU. A server-side Jellyfin transcode/remux path is an optional later feature, not a hidden assumption.
7. **Network:** restrict control to a trusted LAN. Never expose an unauthenticated playback/control API. Never require Internet/cloud access beyond the user's Jellyfin server. Do not silently change firewall settings or publish an open port on WAN.
8. **Security:** do not send Jellyfin user credentials/access tokens to frontend JS or embed them in HTML, logs, public URLs, browser localStorage, process command-line arguments, or checked-in files. Use current, documented Jellyfin authentication headers, not an assumed legacy `api_key=` URL trick.
9. **Distribution:** everything is built off-device (developer host or CI) and shipped as prebuilt binaries: the BCM2835 must never compile Zeropfin, ZeroPlay or the web UI. Releases are a zip (`zeropfin-<version>-armhf.zip`) containing an armhf `.deb` that declares all runtime dependencies and bundles the patched ZeroPlay, plus a `setup.sh` that installs it with apt (see section 14). An ARMv6 build environment is essential; merely using the generic Debian `arm-linux-gnueabihf` toolchain does not by itself prove ARMv6 compatibility (we use a Raspbian armhf rootfs under qemu).
10. **Honesty:** no "fully supported" labels without tests. Report what was tested on x86 mocks, what was cross-compiled, and what was observed on actual Pi hardware. If a player feature does not exist, don't fabricate it.

## 3. Read and verify upstream interfaces first

- ZeroPlay: https://github.com/HorseyofCoursey/zeroplay
- Jellyfin API (prefer the server's OpenAPI/version-appropriate docs, inspect current Jellyfin source or SDK if necessary): https://github.com/jellyfin/jellyfin
- Tailwind 4 + Vite: https://tailwindcss.com/docs/installation/using-vite
- Alpine CSP build: https://alpinejs.dev/advanced/csp
- Vite production builds: https://vite.dev/guide/build.html

ZeroPlay's upstream README, as checked while drafting this file, documents:

```text
zeroplay --control [initial_path]
stdin commands, newline terminated:
  load <path>
  loadloop <path>
  pause
  resume
  stop
  seek <positionMs>
  quit
stdout events:
  ready
  ended
```

**Crucial:** stdout control mode only documents `ready` and `ended`, not periodic position/duration/paused telemetry. Do **not** infer accurate position merely from sending a `seek` command or elapsed wall time. Any monotonic-clock estimate must be explicitly labeled *estimated*, corrected on events, and tested against real playback. For frame-accurate or reliably authoritative progress, make a small independently reviewed upstream ZeroPlay status-event enhancement or a verified adapter; don't fake support. Upstream ZeroPlay has optional WebSocket control, but the project's default is the simpler stdin/stdout mode.

Verify the installed ZeroPlay version/commit and any downstream patches. Record it in release notes and hardware test reports. Zeropfin bundles a ZeroPlay **binary** built from the `tecncr/zeroplay` fork (upstream plus the minimal, backward-compatible control-mode extensions `status`, `events on`, `loadat`; see `docs/adr/0002-zeroplay-control-adapter.md` and `docs/zeroplay-control-extensions.md`) at a pinned commit (`ZEROPLAY_REF` in `.github/workflows/ci.yml`), installed at the private path `/usr/lib/zeropfin/zeroplay`. Do not copy ZeroPlay sources into this repository; change the fork (keeping it upstream-compatible and proposing the changes upstream) and bump the pinned commit. Zeropfin must keep working with stock upstream ZeroPlay via the configurable `zeroplay_path`.

## 4. Scope and milestones

### Phase 0 — evidence and integration spike

- Inspect ZeroPlay control-mode behavior, whether it accepts authenticated network media, how it handles HTTP range requests and redirects, and its stdout/stderr signaling.
- Capture the user's already-working Pi 1B ZeroPlay/CRT configuration in `docs/hardware.md`; never replace a working display configuration blindly.
- Build a tiny mock player which understands the ZeroPlay line protocol and emits deterministic `ready`/`ended` events. CI must not need a Pi.
- Identify the desired Jellyfin minimum supported versions from currently available APIs and produce compatibility fixtures. Do not assume a fixed server major version without validation.
- Write short ADRs in `docs/adr/` for important trade-offs (especially local authenticated-media relay vs. direct player URLs, and progress-state limitations).

### Phase 1 — receiver foundation

- Native C process starts and supervises `zeroplay --control`, handles backpressure, EOF, cleanup, SIGTERM/SIGINT, restart and child failure without leaking/zombifying subprocesses.
- Provide a protected local HTTP API for simple URL playback of an allowlisted test stream, pause, resume, seek, stop, volume if supported by the verified player interface (otherwise hide/disable volume), and status.
- Serve versioned static assets from a configured filesystem directory; do not require a special runtime builder. Provide sensible HTTP cache headers and MIME types.
- Implement safe configuration loading, local remote pairing/authentication and `systemd` service packaging.
- Allow a no-Jellyfin `--mock` or local fixture mode for integration testing without credentials.

### Phase 2 — Jellyfin client

- Implement account-specific authentication using a stable device ID; support a secure SSH/console setup command that obtains an access token without persisting the password.
- List/search libraries, resume items, series/seasons/episodes, item details, and poster/backdrop art. Paginate; never pull or hold entire huge libraries in Pi memory.
- Use version-appropriate `/Items/{itemId}/PlaybackInfo` or equivalent to negotiate the media source and playback session. Advertise only codecs, profiles, containers and subtitles actually supported by the installed ZeroPlay target.
- Prefer verified direct play; accurately describe DirectPlay vs DirectStream vs Transcode. Initial product may intentionally decline server transcoding rather than silently starting an incompatible stream.
- Make a secure HTTP **loopback-only authenticated media relay** available to ZeroPlay where needed, to avoid exposing Jellyfin credentials in a media URL. Implement bounded streaming and correct Range/206 handling; assess memory/CPU overhead on Pi 1B. See section 9.
- Report playback started/progress/stopped to Jellyfin using the correct server version's API. Handle abrupt player exit, early stop, media end, retries and network loss. Do not report an unsupported/estimated value as ground truth.

### Phase 3 — modern web remote and quality

- Responsive and accessible mobile-first UI using Vite + Tailwind v4 + Alpine's CSP-friendly package, built on the developer machine/CI only.
- Player overlay or separate Now Playing view: cover image, title, play/pause, skip/seek, duration, approximate/verified time, episode navigation, stop, loading and actionable error messages.
- Library browsing: search, filters, latest additions, continue watching, movie and series navigation, pagination, poster lazy-loading and empty states.
- Support reduced motion, visible focus states, dark appearance, 44px-ish touch targets, screen-reader labels, and iPhone Safari safe-area insets.
- Retry/resynchronization when iPhone Safari suspends a tab; the Pi's playback MUST continue independently of browser connection.
- Optional home-screen support; don't promise offline service workers/PWA installation on arbitrary plain-HTTP LAN origins. HTTPS/secure-context requirements must be respected.

### Phase 4 — hardening and release

- Native tests, HTTP integration tests, frontend tests, cross-build and artifact inspection, systemd packaging, reliable setup/update documentation, source licenses and a hardware checklist.
- Optional Jellyfin-native remote casting/session discovery only after the web remote is complete and tested. Jellyfin's remote WebSocket protocol is a **separate milestone**, not something automatically supplied by `/Sessions/Playing*` calls.

## 5. Proposed repository structure — illustrative, not prescriptive

**Every file, folder, module boundary, and build-system choice below is a suggestion, not a requirement.** Use the agent's engineering judgment; split, merge, rename, move, or omit modules to produce a smaller and better design. Preserve only the explicit Zeropfin product/binary identity and non-negotiable runtime/security/hardware/visual requirements. If the actual layout differs, document it accurately in the README and architecture notes; don't copy this tree merely to satisfy the diagram.

One *possible* repository structure:

```text
.
├── AGENTS.md
├── README.md
├── LICENSE                        # choose an explicit license; never guess ownership
├── CMakeLists.txt                  # or a straightforward Makefile; document choice
├── cmake/                          # toolchain configurations if CMake is used
├── src/
│   ├── main.c                     # startup, arg parsing, signal handling
│   ├── app.c/.h                   # main lifecycle and orchestrator
│   ├── config.c/.h                # validated configuration and storage
│   ├── http_server.c/.h           # libmicrohttpd adapter
│   ├── http_routes.c/.h           # URL routing, schemas, auth, errors
│   ├── local_auth.c/.h            # pairing, sessions, CSRF
│   ├── jellyfin_client.c/.h       # libcurl HTTP client and auth headers
│   ├── jellyfin_media.c/.h        # playback negotiation, capability checks
│   ├── jellyfin_session.c/.h      # playback lifecycle reporting
│   ├── media_relay.c/.h           # loopback HTTP media and Range relay
│   ├── zeroplay.c/.h              # child pipes, commands, event parsing
│   ├── player_state.c/.h          # state machine + timekeeping
│   ├── static_files.c/.h          # serve built UI with safe paths
│   ├── json.c/.h                  # cJSON helpers, bounded parsing
│   └── util.c/.h                  # bounded strings, logging, timers
├── web/
│   ├── package.json
│   ├── pnpm-lock.yaml
│   ├── index.html
│   ├── vite.config.js
│   ├── src/
│   │   ├── main.js               # register Alpine components and fetch wrapper
│   │   ├── styles.css            # @import 'tailwindcss' and theme
│   │   ├── components/
│   │   └── assets/               # local SVGs/icons only
│   └── dist/                      # generated; typically gitignored
├── tests/
│   ├── native/
│   ├── fixtures/
│   ├── mock_zeroplay/
│   ├── mock_jellyfin/
│   └── integration/
├── packaging/
│   ├── systemd/zeropfin.service
│   └── debian/                   # optional if packaging supports it
├── scripts/
│   ├── build-web.sh
│   ├── build-armv6.sh
│   ├── package.sh
│   └── check-release.sh
├── docs/
│   ├── architecture.md
│   ├── api.md
│   ├── hardware.md
│   ├── security.md
│   ├── deployment.md
│   ├── testing.md
│   └── adr/
└── .github/workflows/ci.yml
```

Start with functional vertical slices, not empty stubs for every file. Introduce a module when it has code or a concrete failing test.

## 6. Native C implementation standards

- C11 (or GNU11 only when needed and documented), POSIX APIs, narrow interfaces, consistent ownership, and explicit byte/length bounds. Compile with `-Wall -Wextra -Wpedantic` and treat significant warnings as failures; on host tests additionally enable ASan/UBSan where practical.
- Prefer established system libraries: `libmicrohttpd` (HTTP server), `libcurl` (Jellyfin HTTPS/client/relay), `cJSON` (or another small maintained packaged C JSON library). Record license and deployment package requirements. Avoid a large dependency tree.
- Use monotonic clock for scheduling/estimated playback progress and wall clock only for human timestamps/expiry.
- Never block an HTTP request dispatcher for the lifetime of media streaming or synchronous Jellyfin operations; use a bounded worker/queue approach. Do not spawn a thread for every client request or store whole JSON responses/remote images in RAM.
- Use bounded request size, pagination, timeouts, TLS verification, retry/backoff with jitter, cancellation, and cleanup. Define upper limits in code and tests; reject oversized payloads.
- No `system()`, `popen()`, `sh -c`, string-built shell commands, untrusted executable paths, unsafe `sprintf`, or uncontrolled deserialization. `exec` ZeroPlay via absolute configured path and an argument vector. Don't run as root.
- ZeroPlay command lines written to stdin must be normalized and safe: reject/control CR/LF and embedded NUL; bound URL/path lengths; prohibit command injection via a user-supplied title, URL or CRLF; serialize all writes on the player-controller thread.
- Treat child stdout as a machine protocol, stderr as diagnostic output; bound both buffers; if unrecognized future output appears, do not corrupt state. Handle broken pipes, HUP, startup timeout, `ready` deadline, signal forwarding, and resource release.
- One playback session at a time. A second `load` must transition the first item through an orderly stop/report cycle before starting a new one.
- Keep state machine transitions explicit and unit-tested (e.g. IDLE → STARTING → PLAYING ↔ PAUSED → ENDED / STOPPED; ERROR and RECOVERING paths). Distinguish command acceptance from actual playback commencement.
- Provide graceful shutdown and atomic credential/config persistence (fsync/rename where appropriate) without constantly writing progress to the SD card.
- Do not put the complete Jellyfin API schema or a full generic ORM into a 256 MiB device; parse only fields required by active screens, cap item counts, and free memory promptly.

## 7. ZeroPlay adapter contract

### Required behavior

1. Spawn exactly one `zeroplay --control` process while active, with a private stdin pipe and separately captured stdout/stderr, no shell.
2. Wait for the documented `ready` event before accepting a media `load` request; expose startup failure and bounded diagnostics to the admin interface.
3. Write `load <url-or-path>\n`, `pause\n`, `resume\n`, `seek <milliseconds>\n`, `stop\n`, `quit\n` only after validating and normalizing parameters. A `seek` parameter is a nonnegative, overflow-checked integer in **milliseconds**; Jellyfin uses **ticks**, commonly 10,000,000 ticks/second. Centralize and unit-test conversions.
4. `ended` applies only to the currently active non-looping item and must not accidentally complete a newer replacement item. Correlate generation IDs in our daemon even though the player line protocol lacks IDs; suppress stale events where feasible, and document the residual ambiguity.
5. Distinguish PAUSED from STOPPED and ERROR; avoid claiming precise actual position when only an estimator is available. Timekeeping may use last known/commanded seek plus monotonic running duration with a visible `estimated` label.
6. Maintain deterministic restart/crash behavior and avoid ever reporting that a file finished simply because the child crashed.

### Gaps, not assumptions

- The documented stdin protocol has no `status`, absolute position, volume, mute, playlist, selected track, or error-code event. Verify upstream before adding UI buttons or claiming these features.
- If accurate progress is necessary, propose an optional minimal backward-compatible ZeroPlay change such as `status`/`position` events with a stable documented schema; test a working version before changing source. Keep fallback usable without this feature.
- Direct media URLs and FFmpeg HTTP headers: ZeroPlay's README documents URLs but **not a generic per-request Authorization-header API**. Do not assume one. Our default integration uses a local relay instead of putting the user's Jellyfin access token in the URL.
- For composite CRT: reuse existing system KMS configuration, aspect ratio, audio device and ZeroPlay installation. The HTTP daemon never reconfigures the graphics stack.

## 8. Jellyfin integration

### Authentication and setup

- Offer an interactive `zeropfin setup` CLI intended to be run via SSH/terminal. It asks for server base URL and user account, obtains a long-lived session token through a version-appropriate Jellyfin login flow and stores **only the resulting token**, server identity, user ID and stable device ID, not the password. Support a supported admin-provisioned token path only if permissions and account scoping are understood.
- Prefer the modern `Authorization: MediaBrowser Client="...", Device="...", DeviceId="...", Version="...", Token="..."` header, verifying exact behavior against target Jellyfin versions. Do not depend on deprecated `X-Emby-Token`, `X-Emby-Authorization` or lowercase `api_key` query parameters. API and auth behavior has changed between Jellyfin major versions.
- Put config and token in a private directory (systemd `StateDirectory=` where applicable), restrict file permissions to the dedicated service user, use TLS with certificate verification for remote HTTPS server URLs, and redact all secrets in logs/error messages.
- UI should have a **separate** local authentication/pairing flow. A user who can load the Pi's web UI must not automatically obtain the Jellyfin token.

### API capabilities

- Version-check the server (public info endpoint), then use documented endpoints for library collections, item browsing, metadata, images, playback info, and `/Sessions/Playing`, `/Sessions/Playing/Progress`, `/Sessions/Playing/Stopped` (or their correct version-specific equivalents).
- Account for H.264 8-bit, profiles Baseline/Main/High within verified hardware constraints; known supported ZeroPlay containers MP4/MKV/MOV/HLS; known audio codecs per current upstream documentation; conservative subtitle support (SRT, if tested). Explicitly reject HEVC/AV1/VP9/H.264 Hi10P and unsupported subtitle/audio tracks rather than hanging.
- Do not assume every H.264 file is playable: assess bitrate, resolution/framerate, profile/level, audio, subtitles, track selection, server-side remux/transcode requirements, and transport capabilities. Provide user-facing failure reasons.
- Do not trust raw input item IDs, stream URLs or filenames; validate server origin, URL scheme, redirects, and allowed hosts to prevent SSRF. Do not use arbitrary externally supplied URL playback by default.
- Preserve/resume `MediaSourceId`, `PlaySessionId`, selected tracks, `StartTimeTicks`, play method, and final stop reasons as supported by the target API.
- Playback-progress updates should be rate-limited (e.g. an adjustable 10–30s interval plus pause/seek/stop events), and all timestamps must be truthfully derived. On `ended`, completion can be reported if the media actually reached EOF. On crash/unexpected stop, preserve the last trustworthy position.
- Limit retry queues; avoid infinite backlog or periodic SD-card writes; fail transparently when Jellyfin is unreachable. Make browsing error states actionable.

## 9. Authenticated media relay — design requirement

The server passes an authenticated source URL to a **loopback-only media endpoint**, such as `http://127.0.0.1:<relay-port>/media/<unpredictable-ephemeral-id>`, and ZeroPlay plays that URL. The daemon relays the single authorized Jellyfin media source using libcurl with current auth headers. No access token or username/password appears in the ZeroPlay URL, browser HTML, JavaScript, process argument list or standard logs.

Requirements:

- Bind media relay to `127.0.0.1` exclusively. The remote-control HTTP listener may bind separately to a configured LAN interface; never route relay endpoints through that listener.
- Restrict to the authenticated, internally selected Jellyfin origin; reject cross-origin redirects and all unsupported schemes. Follow only approved redirects, with strict host/protocol checks. TLS verify by default.
- Support `GET`, and `HEAD` if needed, including `Range` requests and correct upstream/downstream `200`, `206`, `416`, `Content-Range`, `Accept-Ranges`, `Content-Length`, `Content-Type`, EOF, cancellation, and seek behavior. Test FFmpeg/ZeroPlay seeking against the relay.
- **Stream in bounded chunks**; do not buffer the entire file in RAM. Use backpressure and cancellation, with a clearly defined one-playback/limited-reader concurrency policy.
- Do not implement HLS playlists/segment rewriting without tests. HLS can be deferred or supported using a carefully scoped manifest/segment relay with identical auth protections. An HLS URL that contains many referenced resources is not automatically supported by a single-file relay.
- Benchmark actual throughput, CPU use, memory, seek latency and audio sync on the 256 MiB Pi 1B. If this architecture is too costly, document the measured evidence and propose a narrow upstream ZeroPlay header-passing interface as an alternative; do not switch silently to credential-in-URL streaming.
- Account for ZeroPlay/FFmpeg probing and potentially parallel HTTP range requests. Review libmicrohttpd's streaming callback and libcurl interaction for deadlocks and memory growth.

## 10. Local web API — **proposed examples, not a fixed contract**

The following routes and payloads are **illustrative design ideas, not binding instructions** and not Jellyfin's upstream API. **Agents have full discretion to change URL paths, endpoint grouping, response shapes, method choices, resource naming, and versioning strategy**, provided the resulting browser/daemon contract is coherent, documented, secure, testable, and stable within the implementation. Keep library browsing, authenticated playback control, player state, and pairing capabilities, but don't contort the software to match these sample routes. Document the *actual* request/response contracts and errors in `docs/api.md` (or a clearly indicated equivalent) and test them end-to-end.

```text
GET    /healthz                    minimal liveness, no secrets
POST   /api/v1/auth/pair           exchange time-limited local pairing code
POST   /api/v1/auth/logout         invalidate local session
GET    /api/v1/status              player / Jellyfin / device status
GET    /api/v1/libraries           list authorized library views
GET    /api/v1/items               paginated library/search; bounded limit
GET    /api/v1/items/{id}          details, playback eligibility, resume
GET    /api/v1/items/{id}/children  seasons/episodes as appropriate
GET    /api/v1/images/{id}         bounded, access-controlled image relay/cache
POST   /api/v1/player/play         { "itemId": "...", "resume": true }
POST   /api/v1/player/pause
POST   /api/v1/player/resume
POST   /api/v1/player/seek         { "positionMs": 123456 }
POST   /api/v1/player/stop
GET    /api/v1/player/state        status + positionSource: verified|estimated|unknown
GET    /assets/*                   static prebuilt, fingerprinted Vite files
GET    /                           static index.html
```

Use predictable JSON error shapes (code, message, retryable) and proper HTTP status. State updates should initially use polling with modest frequency and exponential backoff when hidden/offline; don't add another always-on WebSocket server until needed. No credential/key material in `/status` or `/healthz`.

## 11. Web UI build and UX

### Mandatory visual design — Zeropfin

Treat these as **acceptance criteria**, not suggestions. Keep this visual design consistent across pairing, browsing, search, details, Now Playing, error/loading states, and desktop/tablet layouts:

- **Palette:** A restrained, minimalist monochrome design: black, white, and shades of gray for all interface chrome (surfaces, typography, borders, panels, icons, controls, skeletons). Use **Tailwind's blue palette** (such as `blue-500` / `blue-600` with accessible contrast) as the **only accent color** for selected/active/focused/primary interactive elements. Do not add secondary accent colors or colorful status badges. Actual Jellyfin posters and backdrops may retain their original artwork colors; they do not authorize a colorful interface palette.
- **Absolutely no gradients, anywhere in the Web UI.** Avoid CSS gradient functions (`linear-gradient`, `radial-gradient`, `conic-gradient`), SVG gradients, glow/fade effects built from gradients, gradient text, gradient borders/buttons, and gradient placeholder artwork. Prefer solid fills, hairline borders, spacing, and deliberate contrast. No exceptions for decorative hero areas or poster overlays.
- **Typography:** **Inter is required** as the interface font (with sane system sans-serif fallback). Include the appropriately licensed Inter font assets **locally in the Vite build**, preferably a small WOFF2 subset/variable font suited to the needed weights, with `font-display: swap`. No Google Fonts, font CDN, or runtime network fetch for typography. Keep license/attribution notices required by its distribution license.
- **Composition:** Minimal, deliberate, content-first, high-density-enough-but-breathable UI. Crisp alignment, excellent spacing, subtle grayscale borders, restrained radii, high legibility, compact and obvious player controls. The design should feel engineered rather than like a generic dashboard or tutorial.
- **No repetitive explanatory subheadings or redundant UI copy.** Don't place a description under every page title, section, card, or control to explain the obvious. Avoid repetitive subtitles such as “Browse your library”, “Control your playback”, or “Discover your content” when the labels already say so. Use concise functional labels and show helper text **only** for genuinely ambiguous actions, errors, onboarding, or configuration.
- **Avoid tooltips in the UI.** Make primary actions intelligible through labels and familiar icons; avoid tooltip-driven explanations and decorative hover hints. Icon-only controls must still have accessible names (`aria-label` or equivalent), visible keyboard focus, and appropriate semantics—accessibility text is not a decorative tooltip.
- **No gradients or extra accent colors in dark/light modes.** Dark-first is acceptable, as is a carefully composed light appearance, but both must follow the same monochrome + Tailwind blue rule. Prefer no theme toggle over a poorly executed second theme.
- Provide cohesive screens for phone, tablet, and desktop; use real Jellyfin artwork rather than brightly colored generic decorations, and verify the visual design through screenshots/review on at least one narrow iPhone viewport and one desktop viewport.

- `web/package.json`: pin exact versions, commit `pnpm-lock.yaml` (pnpm only; never npm), use `pnpm install --frozen-lockfile` in CI. E.g. install `tailwindcss`, `@tailwindcss/vite`, `vite` and `@alpinejs/csp` and configure Vite plugin according to Tailwind 4 docs.
- Put `@import "tailwindcss";` in CSS and use Tailwind 4's current CSS-first workflow, not old v3 `@tailwind base/components/utilities` boilerplate.
- Source structure can use HTML partials/build-time template processing where useful, but output must be HTML/CSS/JS/images only. Vanilla JavaScript modules and registered Alpine components are preferable to enormous inline `x-data` expressions.
- Alpine CSP build with a restrictive production CSP (`script-src 'self'`, no `'unsafe-eval'` and no inline JS if feasible). Test actual event handler syntax against the CSP version; avoid Alpine features unsupported by it. Avoid `x-html` on any untrusted text; use `x-text` or safe DOM APIs.
- Responsive narrow-screen design first, with **Inter**, monochrome surfaces, Tailwind blue accent, strong contrast, poster cards, grayscale skeleton/placeholder states, accessible focus handling, keyboard controls, touch-friendly transport controls, and a graceful no-artwork state. **Never** download fonts at runtime; bundle Inter locally.
- API requests go **only to the Pi's same-origin `/api/v1`**. The browser must never directly call Jellyfin with its secret token. Use `fetch` with same-origin cookies and CSRF protection.
- Preserve progress/Now Playing when browser reloads; get source of truth from daemon rather than treating browser-local state as authoritative. Detect `document.visibilityState` to reduce polling while backgrounded.
- Production Vite build goes to `web/dist`; its filenames must resolve correctly when served by the C daemon (e.g. root-base configuration), including hashed asset references. It may be installed at `/usr/share/zeropfin/web/` and should not need `npm` or `pnpm` on the Pi.
- Set an initial *target*, not a false guarantee, of approximately <200 KiB total gzip-compressed first-party frontend JS+CSS excluding posters. Measure and record actual build sizes in CI. Keep images responsive and lazy-load thumbnails.

## 12. Security baseline

- **Default no-WAN exposure.** Document use only on a trusted local network or through an authenticated TLS reverse proxy/VPN; no public HTTP access. LAN HTTP has confidentiality limits even if local sessions are authenticated.
- Pairing code: cryptographically random, short-lived, one-time-use, rate-limited, printed via local admin CLI or `journalctl`, not permanently embedded in the web UI. Authenticated local session: high-entropy opaque cookie, HttpOnly, SameSite=Strict, sensible expiry, `Secure` when served over HTTPS. Require a CSRF token and/or enforce strict Origin/Host checks for state-changing requests. Do not store access tokens in localStorage.
- Avoid information leaks on errors, generated logs, HTTP headers, traces, and crash reports. Redact token/query/user data. No debug `dump full config` command.
- Validate input lengths, integer overflows, item ID syntax, pagination limits, HTTP content lengths, JSON nesting, static file paths, origins, headers, redirects and URL schemes. Use explicit allowlists. Limit concurrency and per-client rate. Stop slow clients from starving playback.
- Protect image proxy against SSRF and giant image allocations. Cap image request count and size; prefer forwarding compressed image bytes with bounded memory.
- systemd dedicated unprivileged user with only the device groups required on the target (`video`, `render`, `audio`, as verified), restrained filesystem write access and restart policy. Do not enable root or make the daemon setuid. Document permissions if full sandbox hardening interferes with /dev/dri or ALSA.
- Secrets absent from git, release archives, examples and CI artifacts. Test this with automated checks.

## 13. Performance / resource budget

These are engineering **targets to measure**, not current measured results:

- Native daemon idle RSS aspirational target **≤16 MiB** and active browsing target **≤32 MiB** excluding ZeroPlay, kernel file cache, and other services; report actual observations and revisit based on benchmark data.
- Never hold an entire film, HLS segment playlist or full media library in memory. Pagination, streaming relay with a small bounded buffer, and a limited number of JPEG/PNG image responses in flight.
- No background scanning of the entire Jellyfin database, SQLite service, thumbnail transcoding, telemetry service, or periodic high-frequency SD-card writes.
- Battery/thermal concerns belong to iPhone and power supply; Pi-specific tests should cover memory pressure/OOM, network throughput/seek, CPU %, playback stalls, watchdog behavior and clean stopping.
- Measure `MemAvailable`, process RSS/PSS, total system memory under sustained playback, ZeroPlay RAM, relay CPU, seek time, and playback reliability on a **real 256 MiB Pi 1B**. A 512 MiB Pi Zero alone cannot prove 256 MiB feasibility.

## 14. Build, cross-compile, packaging

- Provide one documented developer workflow (e.g. `pnpm` commands, `cmake -S . -B build && cmake --build build`, and `ctest --test-dir build --output-on-failure`). Use whichever real commands the implemented project actually supports.
- If choosing CMake, use `pkg-config` discovery for libmicrohttpd/libcurl/cJSON and publish a proper `armv6` toolchain file. If choosing Make, provide equivalent documented architecture/sysroot flags. Cross-compile on Linux CI against a compatible **Raspberry Pi OS 32-bit ARMv6 sysroot**, not arbitrary generic Debian armhf system libraries that might require ARMv7.
- Verify the resulting ELF architecture, hard-float ABI and dynamic library dependencies using `file`, `readelf -A`, `readelf -d`, etc. Test library loading on the real Pi. Don't claim "ARMv6" based on filename alone.
- Build frontend in CI/developer environment; copy only `web/dist/` files into release archive. Do not install `node`, `pnpm`, `vite`, `tailwind` or development headers on production Pi for normal release deployment.
- Package as a single armhf `.deb` (`scripts/make-deb.sh`, built inside the Raspbian container): `/usr/bin/zeropfin`, the bundled ZeroPlay at `/usr/lib/zeropfin/zeroplay`, `/usr/share/zeropfin/web/*`, `/usr/lib/systemd/system/zeropfin.service`, the conffile `/etc/zeropfin/zeropfin.conf`, docs, `copyright` and a `BUILDINFO` recording the Zeropfin and ZeroPlay commits. `Depends` must come from `dpkg-shlibdeps` over both binaries (never hand-maintained). Maintainer scripts create the `zeropfin` system user (groups `video`, `render`, `audio`) and `/var/lib/zeropfin`, and enable/restart the service only when systemd is running; purge removes state only.
- Wrap the `.deb` with `packaging/setup.sh`, `packaging/INSTALL.md` and `SHA256SUMS` into `zeropfin-<version>-armhf.zip` (`scripts/package.sh`), and check it with `scripts/check-release.sh`. Every CI run must test-install the `.deb` with apt in a fresh Raspbian container (`scripts/build-armv6.sh --package --install-test`).
- If cross-build support is not immediately available, native build on Raspberry Pi OS is an acceptable development fallback, but avoid resource-exhausting frontend compilation on the Pi. Release must state build limitations.
- Do not hardcode `pi` username, `/home/pi`, an interface address, or a fixed ALSA device. Configuration options and error messages should explain what is required.

### Release process

1. Add `docs/releases/<version>.md` (notes: what works, what was not verified on hardware, ZeroPlay commit).
2. Make sure CI is green on `master`, then push a tag named exactly `<version>` (e.g. `0.0.1`; a `v` prefix is also accepted) on `master`.
3. The `release` job publishes a regular GitHub release (not a draft or pre-release, unless the owner says otherwise) with `zeropfin-<version>-armhf.zip`, `zeropfin_<version>_armhf.deb` and `SHA256SUMS`, built by CI only. Never upload locally built binaries to a release.
4. Versions are semantic (`x.y.z`); untagged builds get `0.0.0~dev<timestamp>+<commit>` package versions.

## 15. Tests and CI requirements

Implement in this order:

1. **Pure unit tests:** JSON parsing and limits, URL validation, token redaction, ticks ↔ milliseconds (including overflow), player state transitions, progress estimates, config validation, static path traversal and CSRF/origin checks.
2. **Mock ZeroPlay:** `ready`, `ended`, startup failure, late event, crash, EOF, failed writes, seek, stop, rapid replacement, and buffering of partial stdout lines. No live video needed.
3. **Fake Jellyfin HTTP server:** auth response, pagination, images, playback-info, direct play eligibility, incompatible tracks, playback start/progress/stop, network timeout, 401 refresh/re-auth flow, 5xx retry, and invalid JSON. Maintain versioned fixtures.
4. **Relay tests:** GET, HEAD, Range 206, 416, aborted clients, redirects/SSRF rejection, timeouts, bounded memory, multiple FFmpeg probes, no leaking token in URLs/logs. A small local H.264 sample test fixture should be generated or checked in only with appropriate license.
5. **Frontend:** unit tests for API wrapper/formatters/state handling; browser smoke tests for responsive iPhone-sized viewport, search, navigation and Now Playing. Include a visual/design QA check that verifies **Inter assets are bundled locally, UI chrome stays monochrome with Tailwind blue as the only accent, no gradients are present, and no repetitive explanatory subheadings or gratuitous tooltips are introduced**. Browser-based testing runs on CI/developer host, never on Pi.
6. **Build checks:** `pnpm install --frozen-lockfile`, production web build, native build/test with sanitizer on x86, clang-format/check or equivalent, `clang-tidy`/`cppcheck` if practical, ARMv6 build and ELF checks of both bundled binaries, `.deb` install/purge test in a fresh container, bundle-content assertions (no node/npm/source-map secrets).
7. **Hardware validation:** composite picture and audio, actual URL playback, seek, pause, two-hour endurance, crash/restart, 256 MiB memory, SD-card writes, iPhone Safari tab sleep/resume and Wi-Fi failures. Maintain `docs/hardware-test-report.md` with date, Pi revision/RAM, ZeroPlay commit, Raspberry Pi OS release, server version, display configuration and raw measurements.

CI must run without a live Jellyfin server, the user's credentials, or actual Raspberry Pi hardware. Never skip failing tests silently; distinguish skipped hardware-only tests from passing automated tests.

## 16. Documentation and release quality

`README.md` should explain purpose, demo/screenshot, supported targets, exact dependencies, quickstart, build/install commands, setup over SSH, web pairing, secrets/TLS limitations, how to access from Safari, media constraints, and troubleshooting.

`docs/architecture.md` should diagram traffic clearly:

```text
Safari (HTML/JS remote)
    -> Pi daemon HTTP API (auth/session, browsing/control)
    -> Jellyfin REST API (library, PlaybackInfo, playback reports)
    -> ZeroPlay adapter (--control stdin/stdout)
ZeroPlay
    -> http://127.0.0.1:<relay>/media/<nonce>
    -> bounded Pi relay (authenticated, Range-aware)
    -> Jellyfin video stream
ZeroPlay
    -> V4L2 decode + DRM/KMS composite video + ALSA audio -> CRT
```

Explain that browser API control is distinct from Jellyfin native casting. Include an optional later milestone for the latter rather than claiming it exists by default.

## 17. Workflow rules

1. At the beginning of work, inspect the repo and this file; do not overwrite user changes. If this is an empty directory, initialize a sane project rather than only producing a design narrative.
2. For multi-component work, maintain a short checked implementation plan (`docs/implementation-plan.md` or equivalent), with phases and status. Plans must not substitute for code and tests.
3. Implement the smallest end-to-end vertical slice first, including tests. Keep functionality runnable on the host with mocks; don't make every feature depend on Pi hardware.
4. Before changing external protocols, verify against current upstream source or reproducible test fixtures. Record version-specific deviations; don't invent API fields or ZeroPlay commands.
5. Prefer small, reviewable commits (following the Conventional Commits standard, `feat:`, `chore:`, `build:`, `ci:` prefixes, etc.) if authorized; never force-push or rewrite existing history. Never add real credentials to fixtures. Do not auto-install arbitrary binaries with `curl | sudo bash` as part of tests.
6. Run the relevant tests after each significant change; fix failures, not just suppress them. Collect build and bundle-size evidence. If a dependency/toolchain/network is missing, state the blocker precisely and implement all verifiable portions first.
7. Never say the Pi 1B hardware test passed unless it actually ran on that board. Mark device-only tests clearly as **NEEDS HARDWARE VALIDATION**.
8. When completing a task, report what files were changed, what works, exact commands/tests run and their results, known gaps/security concerns, and the next concrete milestone. No vague claims of production readiness.

## 18. MVP definition of done

A minimum useful release requires:

- [ ] ARMv6-compatible native executable plus static frontend files; no Node.js runtime on Pi.
- [ ] Secure first-run Jellyfin configuration + locally authenticated web control.
- [ ] Real Jellyfin browse/search/item details, paginated and lightweight.
- [ ] One verified H.264 movie or episode direct-plays using ZeroPlay via secure authenticated relay, with CRT output controlled independently of the browser.
- [ ] Pause, resume, seek and stop work; errors and incompatible tracks are visible.
- [ ] Actual `ready`/`ended` parsing, child crash recovery, correct stop/end behavior, honest position-source reporting.
- [ ] Appropriate Jellyfin playback started/progress/stopped reporting and resume behavior, with documented position-accuracy limitations if telemetry is unavailable.
- [ ] Tailwind v4 + Alpine CSP mobile UI, self-hosted Inter, **strict monochrome (black/white/grays) + Tailwind blue accent, absolutely no gradients**, and no repetitive explanatory UI subtitles or tooltips; no remote CDN or heavyweight framework.
- [ ] CI tests on host, ARMv6 build validation, documented deployment/service, and real 256 MiB Pi 1B hardware test results before declaring compatibility verified for the new receiver.

## 19. Out of scope unless separately requested

Casting from the official Jellyfin iPhone app (native session/control protocol), multi-user concurrency, audio-only library features, Spotify/YouTube/Chromecast/AirPlay receivers, DLNA, local torrent handling, H.265 software decoding, DRM streaming, live TV transcoding, subtitle burn-in, HDR tone mapping, video streaming *through* the iPhone, an on-CRT browsing interface, a full Wayland compositor, and nonessential animations.

**Priority:** a small working receiver on the user's 256 MiB Pi 1B, not a feature-heavy generic media center.
