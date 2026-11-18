# ADR 0007 — Prebuilt armhf .deb bundling the patched ZeroPlay, shipped in a zip with setup.sh

Status: accepted (2026-11-18)

## Context
A BCM2835 (Pi 1 / Zero) takes a very long time to compile Zeropfin and ZeroPlay, and building the
web UI there is out of the question. AGENTS.md already required off-device builds. Users also need
the patched ZeroPlay (ADR 0002) for verified positions and load-error events.

## Decision
- CI and developers build everything in the Raspbian Trixie armhf rootfs under qemu (ADR 0001,
  `scripts/build-armv6.sh`), including ZeroPlay from `tecncr/zeroplay` at a pinned commit
  (`ZEROPLAY_REF` in `.github/workflows/ci.yml`).
- `scripts/make-deb.sh` produces a single `zeropfin_<version>_armhf.deb`:
  - **Contents:** `/usr/bin/zeropfin`, the bundled ZeroPlay at the private path
    `/usr/lib/zeropfin/zeroplay` (never clobbering a user's `/usr/local/bin/zeroplay`), the web
    bundle, the systemd unit, the conffile `/etc/zeropfin/zeropfin.conf`, docs, a machine-readable
    `copyright` and a `BUILDINFO` file recording both commits.
  - **Depends:** computed by `dpkg-shlibdeps` over both binaries, so apt installs FFmpeg, libdrm,
    ALSA, FreeType, libmicrohttpd, libcurl and cJSON from Raspberry Pi OS. `adduser` is listed
    too.
  - **Maintainer scripts:** they create the system user, its groups and the state directory, and
    enable/start/restart the service when systemd is running. Purging removes the state.
- `scripts/package.sh` wraps the `.deb` with `setup.sh` (architecture/OS checks, checksum
  verification, then `apt-get install ./zeropfin_*.deb`), `INSTALL.md` and `SHA256SUMS` into
  `zeropfin-<version>-armhf.zip`. The `.deb` is also published on its own.
- Every CI run test-installs the `.deb` with apt in a fresh container, checking the package state,
  the user, shared libraries, that the daemon serves HTTP, and that purge cleans up.
- Pushing a tag `x.y.z` (or `vx.y.z`) publishes a regular, non-draft GitHub release with the zip,
  the `.deb` and `SHA256SUMS`, using `docs/releases/x.y.z.md` as the notes.

## Consequences
- The Pi only downloads and installs; nothing is compiled there.
- One package version pins one ZeroPlay build. Updating ZeroPlay means bumping `ZEROPLAY_REF` and
  releasing again.
- Without an apt repository, upgrades are manual: download the new zip and rerun `setup.sh`.
- Replaces the earlier tarball and `install.sh`.
