#!/usr/bin/env bash
# Assert a release bundle contains exactly what the Pi needs: the setup script,
# checksums and an armhf .deb with ARMv6 binaries, static web assets, service
# files and docs, declaring its runtime dependencies. No Node tooling, source
# maps, sources, fixtures or credentials.
set -euo pipefail
ZIP="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail() { echo "RELEASE CHECK FAILED: $*" >&2; exit 1; }

unzip -q "$ZIP" -d "$tmp"
dir="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d)"
for f in setup.sh INSTALL.md SHA256SUMS; do [ -e "$dir/$f" ] || fail "missing $f in zip"; done
[ -x "$dir/setup.sh" ] || fail "setup.sh not executable"
(cd "$dir" && sha256sum -c --quiet SHA256SUMS) || fail "bundle checksum mismatch"
deb="$(ls "$dir"/zeropfin_*_armhf.deb)" || fail "no .deb in zip"

# Unpack the .deb with ar + tar (dpkg is not needed on the build host).
mkdir "$tmp/deb" "$tmp/root" "$tmp/ctl"
(cd "$tmp/deb" && ar x "$deb")
tar -xf "$tmp"/deb/control.tar.* -C "$tmp/ctl"
tar -xf "$tmp"/deb/data.tar.* -C "$tmp/root"
r="$tmp/root"
grep -q '^Architecture: armhf$' "$tmp/ctl/control" || fail "not armhf"
deps="$(sed -n 's/^Depends: //p' "$tmp/ctl/control")"
for d in libmicrohttpd libcurl libcjson libavformat libavcodec libdrm libasound2 adduser; do
  echo "$deps" | grep -q "$d" || fail "Depends lacks $d"
done
[ "$(cat "$tmp/ctl/conffiles")" = /etc/zeropfin/zeropfin.conf ] || fail "conffiles"
for f in usr/bin/zeropfin usr/lib/zeropfin/zeroplay usr/share/zeropfin/web/index.html \
         usr/share/zeropfin/web/licenses/Inter-OFL.txt usr/lib/systemd/system/zeropfin.service \
         etc/zeropfin/zeropfin.conf usr/share/doc/zeropfin/copyright usr/share/doc/zeropfin/BUILDINFO; do
  [ -e "$r/$f" ] || fail "missing $f in .deb"
done
for s in postinst prerm postrm; do [ -x "$tmp/ctl/$s" ] || fail "missing maintainer script $s"; done
bad="$(cd "$r" && find . \( -name node_modules -o -name '*.map' -o -name 'package*.json' -o -name 'pnpm-lock.yaml' \
        -o -name '*.ts' -o -name 'vite.config.*' -o -name '*.py' -o -name '*.c' -o -name '*.h' -o -name 'library.json' \
        -o -name 'credentials.json' -o -name 'sessions.json' -o -name '.env*' \) -print)"
[ -z "$bad" ] || fail "development or secret files present: $bad"
other="$(cd "$r/usr/share/zeropfin/web" && find . -type f ! -regex '.*\.\(html\|js\|css\|woff2\|svg\|png\|txt\|gz\)$' -print)"
[ -z "$other" ] || fail "unexpected web asset types: $other"
token="$(python3 -I -c "import json;print(json.load(open('$ROOT/tests/fixtures/library.json'))['token'])" 2>/dev/null || true)"
if [ -n "$token" ] && grep -rq "$token" "$r" "$dir/setup.sh"; then fail "fixture token found in package"; fi
if grep -rlE 'BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY|"accessToken" *: *"[0-9a-f]{32}"' "$r" >/dev/null; then
  fail "secret material found"
fi
"$ROOT/scripts/check-elf.sh" "$r/usr/bin/zeropfin" >/dev/null || fail "zeropfin ELF check"
"$ROOT/scripts/check-elf.sh" --any-deps "$r/usr/lib/zeropfin/zeroplay" >/dev/null || fail "zeroplay ELF check"
echo "release check passed: $(basename "$ZIP") ($(du -h "$ZIP" | cut -f1)); $(basename "$deb"): Depends: $deps"
