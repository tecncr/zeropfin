#!/usr/bin/env bash
# Build Zeropfin for Raspberry Pi 1 / Zero (ARMv6, hard-float) inside a
# Raspbian (Raspberry Pi OS 32-bit) Trixie armhf root filesystem, emulated with
# qemu-user binfmt. Raspbian packages are built for ARMv6+VFP, unlike Debian's
# armhf port (ARMv7), so the binary links against genuinely ARMv6 libraries.
#
#   scripts/build-armv6.sh                 # build + ELF checks -> dist/armv6/
#   scripts/build-armv6.sh --test          # also run all ctest suites on an emulated ARM1176 CPU
#   scripts/build-armv6.sh --zeroplay DIR  # also compile a ZeroPlay checkout (e.g. the fork)
#   scripts/build-armv6.sh --zeroplay DIR --package --install-test
#                                          # also build zeropfin_<ver>_armhf.deb (bundling that
#                                          # ZeroPlay) and test-install it in a fresh container
#
# Requirements: podman (or docker via CONTAINER_ENGINE=docker) and a qemu-arm
# binfmt handler with the F flag (Fedora: qemu-user-static; GitHub Actions:
# docker/setup-qemu-action). Network access to archive.raspbian.org on first run.
# ZEROPFIN_OFFLINE=1 makes --install-test use dpkg -i with the libraries already
# in the image instead of downloading apt indexes (metered connections).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${CONTAINER_ENGINE:-podman}"
SUITE="${RASPBIAN_SUITE:-trixie}"
IMAGE="${ARMV6_IMAGE:-localhost/zeropfin-raspbian-${SUITE}-armhf:1}"
MIRROR="${RASPBIAN_MIRROR:-http://archive.raspbian.org/raspbian}"
# The keyring comes from raspbian-archive-keyring 20120528.4, whose key carries
# SHA-256 self-signatures (Debian Trixie's sqv rejects the SHA-1-only 2012 key
# at http://archive.raspbian.org/raspbian.public.key). Trust is anchored twice:
# the .deb's SHA-256 is pinned and the primary key fingerprint is checked.
KEYRING_DEB_URL="http://archive.raspbian.org/raspbian/pool/main/r/raspbian-archive-keyring/raspbian-archive-keyring_20120528.4_all.deb"
KEYRING_DEB_SHA256="eb2bc175ecfad128ece8222b42eefabd0a2846afd14f3af04364f4a047cbc88f"
KEY_FPR="A0DA38D0D76E8B5D638872819165938D90FDDD2E" # Raspbian Archive Signing Key (ARMv6+VFP)
OUT="${OUT_DIR:-$ROOT/dist/armv6}"
ZF_VERSION="${ZF_VERSION:-development build}"
ZF_COMMIT="${ZF_COMMIT:-n/a}"
RUN_TESTS=0
ZEROPLAY_SRC=""
PACKAGE=0
INSTALL_TEST=0

while [ $# -gt 0 ]; do
  case "$1" in
    --test) RUN_TESTS=1 ;;
    --zeroplay) ZEROPLAY_SRC="$(cd "$2" && pwd)"; shift ;;
    --package) PACKAGE=1 ;;
    --install-test) INSTALL_TEST=1 ;;
    --rebuild-image) "$ENGINE" rmi -f "$IMAGE" >/dev/null 2>&1 || true ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

if [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ] || ! grep -q '^flags:.*F' /proc/sys/fs/binfmt_misc/qemu-arm; then
  echo "error: no qemu-arm binfmt handler with the F flag; install qemu-user-static (or run docker/setup-qemu-action)" >&2
  exit 1
fi

image_exists() { "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; }

build_image() {
  echo "==> creating Raspbian $SUITE armhf build image $IMAGE (one-time)"
  local tmp; tmp="$(mktemp -d)"
  # Stage 1 (x86): fetch + verify the archive key, bootstrap the armhf rootfs.
  "$ENGINE" run --rm -v "$tmp:/out:Z" docker.io/library/debian:trixie bash -euc "
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq mmdebstrap gpg curl ca-certificates >/dev/null
    curl -fsSL '$KEYRING_DEB_URL' -o /tmp/keyring.deb
    echo '$KEYRING_DEB_SHA256  /tmp/keyring.deb' | sha256sum -c -
    dpkg-deb -x /tmp/keyring.deb /tmp/keyring
    cp /tmp/keyring/usr/share/keyrings/raspbian-archive-keyring.gpg /tmp/raspbian.gpg
    fpr=\$(gpg --show-keys --with-colons /tmp/raspbian.gpg | awk -F: '/^fpr/ {print \$10; exit}')
    [ \"\$fpr\" = '$KEY_FPR' ] || { echo \"unexpected Raspbian key fingerprint \$fpr\" >&2; exit 1; }
    mmdebstrap --mode=root --arch=armhf --variant=apt --keyring=/tmp/raspbian.gpg \
      --include=build-essential,cmake,ninja-build,pkg-config,file,binutils,ca-certificates,python3,libmicrohttpd-dev,libcurl4-openssl-dev,libcjson-dev,libavformat-dev,libavcodec-dev,libavutil-dev,libswresample-dev,libswscale-dev,libdrm-dev,libasound2-dev,libfreetype-dev \
      '$SUITE' /out/rootfs.tar '$MIRROR'
  "
  [ -s "$tmp/rootfs.tar" ] || { echo "error: bootstrap produced no rootfs" >&2; exit 1; }
  "$ENGINE" import --change 'CMD ["/bin/bash"]' "$tmp/rootfs.tar" "$IMAGE" >/dev/null
  rm -rf "$tmp"
}

if ! image_exists; then
  build_image
fi

if [ "$PACKAGE" = 1 ] && [ -z "$ZEROPLAY_SRC" ]; then
  echo "error: --package bundles ZeroPlay; pass --zeroplay DIR" >&2
  exit 2
fi
# Debian version: the release version, or a sortable development version.
if [[ "$ZF_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]]; then
  DEB_VERSION="$ZF_VERSION"
else
  DEB_VERSION="0.0.0~dev$(date -u +%Y%m%d%H%M)+$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo local)"
fi
ZEROPLAY_COMMIT="${ZEROPLAY_COMMIT:-$( [ -n "$ZEROPLAY_SRC" ] && git -C "$ZEROPLAY_SRC" rev-parse HEAD 2>/dev/null || echo unknown)}"

mkdir -p "$OUT"
echo "==> building zeropfin for armv6 ($ZF_VERSION)"
rm -f "$OUT"/zeropfin_*_armhf.deb
"$ENGINE" run --rm -e QEMU_CPU=arm1176 -e RUN_TESTS="$RUN_TESTS" -e PACKAGE="$PACKAGE" \
  -e DEB_VERSION="$DEB_VERSION" -e ZF_COMMIT="$ZF_COMMIT" -e ZEROPLAY_COMMIT="$ZEROPLAY_COMMIT" \
  -v "$ROOT:/src:ro,Z" -v "$OUT:/out:Z" ${ZEROPLAY_SRC:+-v "$ZEROPLAY_SRC:/zeroplay:ro,Z"} \
  "$IMAGE" bash -euc "
    . /etc/os-release; echo \"build root: \$PRETTY_NAME (\$(uname -m) emulated as ARM1176)\"
    gcc -v 2>&1 | grep -o -- '--with-arch=[^ ]*\\|--with-fpu=[^ ]*\\|--with-float=[^ ]*' | tr '\n' ' '; echo
    cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DZF_WERROR=ON \
      -DZF_VERSION='$ZF_VERSION' -DZF_COMMIT='$ZF_COMMIT' -DZF_BUILD_TESTS=\$( [ \$RUN_TESTS = 1 ] && echo ON || echo OFF ) >/dev/null
    cmake --build /tmp/build
    if [ \$RUN_TESTS = 1 ]; then
      # zeropfin refuses to run as root: run every test (unit, component and the
      # end-to-end integration suite) as an unprivileged user.
      chmod -R a+rwX /tmp/build
      setpriv --reuid=65534 --regid=65534 --clear-groups env HOME=/tmp \
        ctest --test-dir /tmp/build --output-on-failure
    fi
    strip --strip-unneeded -o /out/zeropfin /tmp/build/zeropfin
    file /out/zeropfin
    if [ -d /zeroplay ]; then
      echo '==> compiling ZeroPlay from /zeroplay'
      cp -r /zeroplay /tmp/zp && make -C /tmp/zp -s clean && make -C /tmp/zp -s
      strip -o /out/zeroplay /tmp/zp/zeroplay && file /out/zeroplay
      /src/scripts/check-elf.sh --any-deps /out/zeroplay
    fi
    /src/scripts/check-elf.sh /out/zeropfin
    if [ \$PACKAGE = 1 ]; then /src/scripts/make-deb.sh; fi
  "

if [ "$INSTALL_TEST" = 1 ]; then
  deb="$(ls "$OUT"/zeropfin_*_armhf.deb)"
  echo "==> test-installing $(basename "$deb") in a fresh Raspbian container"
  "$ENGINE" run --rm -e QEMU_CPU=arm1176 -e OFFLINE="${ZEROPFIN_OFFLINE:-0}" -v "$OUT:/out:ro,Z" "$IMAGE" bash -euc "
    if [ \"\$OFFLINE\" = 1 ]; then
      # No downloads: the build image already contains every runtime library;
      # dpkg still enforces the declared Depends.
      dpkg -i /out/$(basename "$deb") >/dev/null
    else
      apt-get update -qq
      DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends /out/$(basename "$deb") >/dev/null
    fi
    dpkg -s zeropfin | grep -E '^(Status|Version|Depends):'
    zeropfin --version
    id zeropfin
    stat -c '%U %a %n' /var/lib/zeropfin
    test -x /usr/lib/zeropfin/zeroplay && test -f /usr/share/zeropfin/web/index.html
    grep -q '^zeroplay_path = /usr/lib/zeropfin/zeroplay' /etc/zeropfin/zeropfin.conf
    ldd /usr/bin/zeropfin /usr/lib/zeropfin/zeroplay | grep 'not found' && exit 1 || true
    # the daemon starts with the packaged config as the service user (no display here,
    # so ZeroPlay itself fails; the HTTP side must still come up)
    setpriv --reuid=zeropfin --regid=zeropfin --init-groups env RUNTIME_DIRECTORY=/tmp STATE_DIRECTORY=/var/lib/zeropfin \
      zeropfin --config /etc/zeropfin/zeropfin.conf --listen 127.0.0.1 serve >/tmp/zf.log 2>&1 &
    for i in \$(seq 50); do python3 -c \"import urllib.request;urllib.request.urlopen('http://127.0.0.1:8080/healthz')\" 2>/dev/null && break; sleep 0.3; done
    python3 -c \"import urllib.request;print(urllib.request.urlopen('http://127.0.0.1:8080/healthz').read().decode())\"
    python3 -c \"import urllib.request;r=urllib.request.urlopen('http://127.0.0.1:8080/');print(r.status, r.headers['Content-Type'])\"
    kill \$!; wait || true
    apt-get purge -y -qq zeropfin >/dev/null && [ ! -e /var/lib/zeropfin ] && echo 'purge OK'
  "
fi
echo "==> artifacts in $OUT"
ls -la "$OUT"
