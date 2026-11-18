#!/usr/bin/env bash
# Build zeropfin_<version>_armhf.deb. Runs INSIDE the Raspbian armhf build
# container (scripts/build-armv6.sh --package) so dpkg-shlibdeps resolves the
# exact Raspberry Pi OS runtime packages both binaries link against.
#
# Inputs:  /out/zeropfin, /out/zeroplay (patched fork build), /src (this repo,
#          with web/dist already built), env DEB_VERSION, ZF_COMMIT, ZEROPLAY_COMMIT
# Output:  /out/zeropfin_${DEB_VERSION}_armhf.deb
set -euo pipefail
: "${DEB_VERSION:?}"
SRC=/src
OUT=/out
[ -x "$OUT/zeropfin" ] || { echo "missing $OUT/zeropfin" >&2; exit 1; }
[ -x "$OUT/zeroplay" ] || { echo "missing $OUT/zeroplay (pass --zeroplay DIR)" >&2; exit 1; }
[ -f "$SRC/web/dist/index.html" ] || { echo "missing web/dist (run pnpm build first)" >&2; exit 1; }

pkg="$(mktemp -d)/zeropfin"
install -Dm0755 "$OUT/zeropfin" "$pkg/usr/bin/zeropfin"
install -Dm0755 "$OUT/zeroplay" "$pkg/usr/lib/zeropfin/zeroplay"
(cd "$SRC/web/dist" && find . -type f ! -name build-report.json ! -name '*.map' -print0 |
  xargs -0 -I{} install -Dm0644 {} "$pkg/usr/share/zeropfin/web/{}")
install -Dm0644 "$SRC/packaging/systemd/zeropfin.service" "$pkg/usr/lib/systemd/system/zeropfin.service"
install -Dm0644 "$SRC/packaging/zeropfin.conf" "$pkg/etc/zeropfin/zeropfin.conf"
install -Dm0755 "$SRC/scripts/capture-hw-info.sh" "$pkg/usr/share/zeropfin/capture-hw-info.sh"
doc="$pkg/usr/share/doc/zeropfin"
install -Dm0644 "$SRC/README.md" "$doc/README.md"
install -Dm0644 "$SRC/THIRD_PARTY_LICENSES.md" "$doc/THIRD_PARTY_LICENSES.md"
for d in deployment security hardware hardware-test-report api zeroplay-control-extensions; do
  install -Dm0644 "$SRC/docs/$d.md" "$doc/$d.md"
done
{
  echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
  echo "Upstream-Name: zeropfin"
  echo "Source: https://github.com/tecncr/zeropfin"
  echo
  echo "Files: usr/bin/zeropfin usr/share/zeropfin/*"
  echo "License: MIT"
  echo
  echo "Files: usr/lib/zeropfin/zeroplay"
  echo "Comment: ZeroPlay (https://github.com/HorseyofCoursey/zeroplay) built from the"
  echo " tecncr/zeroplay fork with control-mode status extensions, commit ${ZEROPLAY_COMMIT:-unknown}."
  echo "License: MIT"
  echo
  echo "Files: usr/share/zeropfin/web/assets/inter-*"
  echo "License: OFL-1.1"
  echo "Comment: full text in usr/share/zeropfin/web/licenses/Inter-OFL.txt"
  echo
  echo "License: MIT"
  sed 's/^$/./; s/^/ /' "$SRC/LICENSE"
} > "$doc/copyright"
if [ -f /zeroplay/LICENSE ]; then install -Dm0644 /zeroplay/LICENSE "$doc/zeroplay-LICENSE"; fi
. /etc/os-release
cat > "$doc/BUILDINFO" <<EOF
zeropfin ${DEB_VERSION} (commit ${ZF_COMMIT:-n/a})
zeroplay fork commit ${ZEROPLAY_COMMIT:-unknown}
built $(date -u +%Y-%m-%dT%H:%M:%SZ) on ${PRETTY_NAME}, $(gcc -dumpfullversion 2>/dev/null || true)
EOF

# Runtime dependencies from the real ELF NEEDED entries of both binaries.
work="$(mktemp -d)"
mkdir -p "$work/debian"
printf 'Source: zeropfin\n\nPackage: zeropfin\nArchitecture: armhf\n' > "$work/debian/control"
shlibs="$(cd "$work" && dpkg-shlibdeps -O "$pkg/usr/bin/zeropfin" "$pkg/usr/lib/zeropfin/zeroplay" 2>/dev/null |
  sed -n 's/^shlibs:Depends=//p')"
[ -n "$shlibs" ] || { echo "dpkg-shlibdeps produced no dependencies" >&2; exit 1; }

mkdir -p "$pkg/DEBIAN"
cat > "$pkg/DEBIAN/control" <<EOF
Package: zeropfin
Version: ${DEB_VERSION}
Architecture: armhf
Maintainer: Zeropfin maintainers <zeropfin@users.noreply.github.com>
Depends: ${shlibs}, adduser
Recommends: systemd
Section: video
Priority: optional
Homepage: https://github.com/tecncr/zeropfin
Installed-Size: $(du -sk "$pkg" | cut -f1)
Description: Jellyfin playback receiver for Raspberry Pi 1 / Zero with a web remote
 Zeropfin turns an ARMv6 Raspberry Pi with a composite CRT into a Jellyfin
 receiver controlled from a phone browser. It bundles a ZeroPlay build with
 control-mode status extensions for hardware H.264 playback (V4L2 + DRM/KMS).
EOF
echo /etc/zeropfin/zeropfin.conf > "$pkg/DEBIAN/conffiles"
cat > "$pkg/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    if ! getent passwd zeropfin >/dev/null; then
        adduser --system --group --home /var/lib/zeropfin --no-create-home \
            --shell /usr/sbin/nologin --comment "Zeropfin receiver" zeropfin >/dev/null
    fi
    # DRM/KMS, the V4L2 decoder and ALSA for the ZeroPlay child process.
    for g in video render audio; do
        if getent group "$g" >/dev/null; then adduser zeropfin "$g" >/dev/null 2>&1 || true; fi
    done
    install -d -o zeropfin -g zeropfin -m 0700 /var/lib/zeropfin
    if [ -d /run/systemd/system ]; then
        systemctl daemon-reload >/dev/null 2>&1 || true
        systemctl enable zeropfin.service >/dev/null 2>&1 || true
        if [ -n "$2" ]; then
            systemctl try-restart zeropfin.service >/dev/null 2>&1 || true
        else
            systemctl start zeropfin.service >/dev/null 2>&1 || true
        fi
    fi
fi
exit 0
EOF
cat > "$pkg/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ] && { [ "$1" = remove ] || [ "$1" = deconfigure ]; }; then
    systemctl stop zeropfin.service >/dev/null 2>&1 || true
    systemctl disable zeropfin.service >/dev/null 2>&1 || true
fi
exit 0
EOF
cat > "$pkg/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = purge ]; then
    # Credentials and paired sessions. The system user is kept (Debian policy).
    rm -rf /var/lib/zeropfin
fi
if [ -d /run/systemd/system ]; then systemctl daemon-reload >/dev/null 2>&1 || true; fi
exit 0
EOF
chmod 0755 "$pkg/DEBIAN/postinst" "$pkg/DEBIAN/prerm" "$pkg/DEBIAN/postrm"
find "$pkg" -type d -exec chmod 0755 {} +
deb="$OUT/zeropfin_${DEB_VERSION}_armhf.deb"
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$deb" >/dev/null
echo "built $deb"
dpkg-deb -f "$deb" Package Version Architecture Depends
