# Testing

Everything below runs on an x86_64 developer machine or CI — no Pi, no Jellyfin, no credentials.

## Native (C)

```sh
cmake -S . -B build -G Ninja -DZF_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug   # ASan + UBSan
cmake --build build
ctest --test-dir build --output-on-failure
```

| ctest name | What it covers |
|---|---|
| `unit.test_util` | ticks ↔ ms incl. overflow, strict integer parsing, URL redaction, bounded buffers, SHA-256 FIPS vectors, atomic writes |
| `unit.test_config` | defaults, parsing, unknown keys, ZeroPlay argument allowlist, path validation, the shipped sample config |
| `unit.test_urlutil` | URL parsing (userinfo/fragments refused), server URL normalization, redirect resolution, encoding, id syntax |
| `unit.test_netfilter` | private ranges incl. IPv4-mapped, CIDR, Host allowlist (DNS rebinding), Origin |
| `unit.test_auth` | pairing flow, one-time use, expiry, brute-force limits, hashed persistence, revoke |
| `unit.test_zp_proto` | event parsing, partial/CRLF/NUL/overlong lines, command injection attempts, OSD sanitizing |
| `unit.test_player_state` | state transitions, estimator, verified/estimated, stale `ended`, stock mode, crash ≠ completion |
| `unit.test_compat` | eligibility against fixtures: HEVC, Hi10P, first-audio DTS, 60 fps, bitrate, AVI, second source |
| `unit.test_jellyfin_map` | MediaBrowser header, version gating, item trimming, report bodies |
| `unit.test_relay_range` / `unit.test_http_static` | Range/Content-Range parsing; static path traversal and MIME |
| `component.player` | real supervisor + mock ZeroPlay: lifecycle, pause/seek/EOF reports, crash/restart, stock mode (load+seek), load failure, replacement, start failures |
| `integration.daemon` | real daemon + mock ZeroPlay + fake Jellyfin over HTTP (≈120 checks): pairing/CSRF/Origin/Host, browse/paging/search/images, eligibility, play/pause/seek/stop with state checks, Jellyfin reports (start/progress/stop, EOF marks played, crash `Failed: true`), relay Range/416/HEAD/redirects/resume/abort/memory/revocation, static assets/CSP/MIME/cache, logout, clean SIGTERM, secret hygiene in logs |

`tests/mock_zeroplay` speaks ZeroPlay's control protocol (stock or fork extensions) with fault
injection; `tests/mock_jellyfin/server.py` (stdlib only) serves `tests/fixtures/library.json`
(synthetic, regenerate with `tests/fixtures/generate_library.py`) with real Range semantics and
request recording.

Static analysis: `cppcheck --enable=warning,performance,portability --error-exitcode=1 src`
and `clang-format --dry-run --Werror src/*.[ch] tests/**/*.c`.

## Web

```sh
cd web
pnpm install --frozen-lockfile
pnpm test            # vitest: formatters, router, API client, poller/backoff
pnpm build           # production bundle + gzip + size report (budget 200 KiB JS+CSS gzip)
pnpm check:design    # static QA: Inter bundled + license, no remote loads, no gradients,
                     # palette = grays/black/white/blue, no tooltips/inline scripts/styles
ZEROPFIN_BUILD=../build CHROMIUM=/usr/bin/chromium-browser pnpm e2e
```
`pnpm e2e` starts `scripts/dev-stack.py` and drives Chromium at iPhone size (390×844, touch) and
desktop size (1440×900): pairing, home, infinite scroll, search, incompatible item, play/pause/+30 s,
tab hidden/visible polling, reload restores state, mini player, series/seasons; computed-style
audit (palette, gradients, `title` attributes, Inter loaded), focus visibility, CSP violations,
cross-origin requests. Screenshots land in `docs/screenshots/`.
**This is Chromium, not WebKit**: real iPhone Safari is part of the hardware checklist.

## ARMv6

```sh
scripts/build-armv6.sh --test --zeroplay ~/Projects/zeroplay
```
Builds inside a Raspbian Trixie armhf rootfs (signed archive, pinned key), runs **all** ctest
suites — unit, component and the end-to-end integration suite, as an unprivileged user — under
`QEMU_CPU=arm1176` (traps ARMv7-only instructions), compiles ZeroPlay from the given checkout, and
runs `scripts/check-elf.sh` (ELF32 ARM, `Tag_CPU_arch: v6`, VFP args,
no NEON, expected `NEEDED` libraries). Emulation is not hardware: see `hardware-test-report.md`.

## Release

`scripts/build-armv6.sh --zeroplay DIR --package --install-test` builds `zeropfin_<ver>_armhf.deb`
inside the Raspbian container (`scripts/make-deb.sh`; dependencies via `dpkg-shlibdeps`) and then
installs it with apt in a **fresh** container: package status and `Depends`, the `zeropfin` user
and `/var/lib/zeropfin` mode, no missing shared libraries for either binary, the daemon serving
`/healthz` and `/` as the service user with the packaged config, and `apt purge` cleanup.
`scripts/package.sh` builds the zip and runs `scripts/check-release.sh`: bundle checksums,
required files, maintainer scripts and conffiles, declared dependencies, no Node/pnpm/sources/
maps/fixtures/credentials, static asset types only, ELF targets of both binaries.
