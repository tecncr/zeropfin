#!/usr/bin/env bash
# Assemble the release bundle from the ARMv6 .deb (scripts/build-armv6.sh
# --zeroplay DIR --package):
#   dist/zeropfin-<version>-armhf.zip  = zeropfin-<version>/{zeropfin_<version>_armhf.deb, setup.sh,
#                                        INSTALL.md, SHA256SUMS}
#   dist/zeropfin_<version>_armhf.deb  (also published on its own)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEB="$(ls "$ROOT"/dist/armv6/zeropfin_*_armhf.deb 2>/dev/null | head -1)"
[ -n "$DEB" ] || { echo "no .deb in dist/armv6 (run scripts/build-armv6.sh --zeroplay DIR --package)" >&2; exit 1; }
VERSION="$(basename "$DEB" | sed -E 's/^zeropfin_(.*)_armhf\.deb$/\1/')"
NAME="zeropfin-${VERSION}"
STAGE="$ROOT/dist/$NAME"
rm -rf "$STAGE" "$ROOT/dist/$NAME-armhf.zip"
mkdir -p "$STAGE"
cp "$DEB" "$STAGE/"
install -m0755 "$ROOT/packaging/setup.sh" "$STAGE/setup.sh"
install -m0644 "$ROOT/packaging/INSTALL.md" "$STAGE/INSTALL.md"
(cd "$STAGE" && sha256sum "$(basename "$DEB")" INSTALL.md setup.sh > SHA256SUMS)
(cd "$ROOT/dist" && zip -qr -X "$NAME-armhf.zip" "$NAME")
cp "$DEB" "$ROOT/dist/"
(cd "$ROOT/dist" && sha256sum "$NAME-armhf.zip" "$(basename "$DEB")" > SHA256SUMS)
"$ROOT/scripts/check-release.sh" "$ROOT/dist/$NAME-armhf.zip"
cat "$ROOT/dist/SHA256SUMS"
