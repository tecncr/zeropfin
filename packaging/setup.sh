#!/bin/sh
# Zeropfin setup for Raspberry Pi OS Lite 32-bit (Pi 1 Model B / Zero / Zero W).
#
#   unzip zeropfin-<version>-armhf.zip && cd zeropfin-<version> && sudo ./setup.sh
#
# Verifies the bundle, then installs zeropfin_<version>_armhf.deb with apt, which
# downloads the runtime libraries it declares (libmicrohttpd, libcurl, cJSON,
# FFmpeg, libdrm, ALSA, FreeType) from your Raspberry Pi OS mirrors. Nothing is
# compiled on the Pi. Re-running upgrades in place; /etc/zeropfin/zeropfin.conf
# and /var/lib/zeropfin (credentials, paired remotes) are kept.
set -eu
cd "$(dirname "$0")"
if [ "$(id -u)" != 0 ]; then exec sudo sh "$0" "$@"; fi

arch="$(dpkg --print-architecture)"
if [ "$arch" != armhf ]; then
  echo "This bundle is for Raspberry Pi OS 32-bit (armhf); this system is $arch." >&2
  exit 1
fi
. /etc/os-release
if [ "${VERSION_CODENAME:-}" != trixie ]; then
  echo "Warning: built for Raspberry Pi OS Trixie; this system is ${PRETTY_NAME:-unknown}." >&2
  echo "Dependencies may not resolve. Press Ctrl-C to abort, Enter to continue." >&2
  read -r _
fi
sha256sum -c --quiet SHA256SUMS
deb="$(ls zeropfin_*_armhf.deb)"

echo "==> Installing $deb and its runtime dependencies"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "./$deb"

echo
zeropfin --version | head -1
if systemctl is-active --quiet zeropfin; then echo "zeropfin.service is running."; fi
host="$(hostname)"
cat <<MSG

Next steps:
  1. Optional: edit /etc/zeropfin/zeropfin.conf (analog audio device, port), then
       sudo systemctl restart zeropfin
  2. Sign in to Jellyfin (the password is not stored):
       sudo -u zeropfin zeropfin setup
  3. On your phone, open http://$host.local:8080/ and pair
     ("Show code on TV", or: sudo -u zeropfin zeropfin pair)
MSG
