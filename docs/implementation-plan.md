# Implementation plan

Vertical slices, each runnable on an x86 host with mocks. Status as of 2026-11-18.
Legend: ✅ done and tested on host · 🟡 done, verified only in emulation/mocks · ⬜ not started.

## Slice 1 — receiver foundation ✅
- ✅ C11 daemon, config (strict), CLI, systemd notify, ordered shutdown
- ✅ ZeroPlay supervisor (`posix_spawn`, pipes, pidfd, ready/probe/load timeouts, backoff restart)
- ✅ Line protocol with injection-safe formatting; mock ZeroPlay with fault injection
- ✅ Playback state machine + estimator (verified/estimated/unknown)
- ✅ Pairing codes, hashed sessions, CSRF/Origin/Host/LAN filters, admin control socket
- ✅ Static file server (safe paths, gzip siblings, CSP, caching)

## Slice 2 — Jellyfin client ✅
- ✅ `zeropfin setup` (public info + version ≥ 10.10 + AuthenticateByName; token only)
- ✅ Views, items (paged, search, sort, unplayed), resume, next up, latest, details, seasons, episodes, images
- ✅ PlaybackInfo (direct play only) + conservative eligibility with user-facing reasons
- ✅ Fake Jellyfin + synthetic fixtures; verified paths against 10.10.0 and 12.4 sources/OpenAPI
- ⬜ Validation against the owner's real Jellyfin server (`zeropfin setup` by the owner)

## Slice 3 — relay and lifecycle reporting ✅ (host) / 🟡 (Pi)
- ✅ Loopback relay: Range/206/416/HEAD, same-origin redirects, resume after drop, backpressure, revocation
- ✅ Playing/Progress/Stopped reporting; EOF marks played; crash reports `Failed`
- ✅ ZeroPlay fork patch (`status`, `events on`, `loadat`) — compiles in the Raspbian container 🟡
- ⬜ Relay CPU/throughput/seek latency on a Pi 1B (hardware checklist H7, H17)

## Slice 4 — web remote, packaging, CI ✅ / 🟡
- ✅ Vite 8 + Tailwind 4 + Alpine CSP UI; Inter self-hosted; palette enforced; 33.5 KiB gzip JS+CSS
- ✅ vitest units; Chromium e2e at iPhone and desktop sizes with screenshots and runtime design audit
- ✅ ARMv6 build in a Raspbian Trixie armhf rootfs (qemu, `QEMU_CPU=arm1176` tests, ELF checks) 🟡
- ✅ Release bundle: armhf `.deb` (bundled patched ZeroPlay, `dpkg-shlibdeps` dependencies, maintainer scripts) + zip with `setup.sh`; install/purge tested in a fresh Raspbian container; content checks
- ✅ Tag-driven GitHub releases (0.0.1)
- ✅ GitHub Actions workflow (host tests, web, ARMv6 build, package) — not yet run on GitHub
- ⬜ Real iPhone Safari test (WebKit) — hardware checklist H3, H14, H15

## Next milestones
1. Run the hardware checklist (`docs/hardware-test-report.md`) on the 256 MiB Pi 1B; fill `docs/hardware.md`.
2. Upstream the ZeroPlay control-mode extensions (or keep the fork) after Pi validation.
3. Optional: audio-track selection via a ZeroPlay extension; server-side remux/transcode path (new ADR).
4. Optional, separate milestone: Jellyfin native remote-control/casting session protocol.
