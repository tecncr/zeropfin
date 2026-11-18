#!/bin/sh
# Capture the Pi's working display/audio/ZeroPlay configuration for
# docs/hardware.md and docs/hardware-test-report.md. Read-only; prints to stdout.
#   sh capture-hw-info.sh > pi-info.txt
echo "## date";            date -Iseconds
echo "## model";           tr -d '\0' < /proc/device-tree/model 2>/dev/null; echo
echo "## revision";        awk '/^Revision/ {print $3}' /proc/cpuinfo
echo "## cpu";             awk -F: '/model name|Features/ {print $2}' /proc/cpuinfo | sort -u
echo "## memory";          grep -E 'MemTotal|MemAvailable|SwapTotal' /proc/meminfo
echo "## os";              . /etc/os-release && echo "$PRETTY_NAME"; cat /etc/rpi-issue 2>/dev/null | head -2
echo "## kernel";          uname -a
for f in /boot/firmware/config.txt /boot/config.txt; do
  [ -f "$f" ] && { echo "## $f (non-comment lines)"; grep -vE '^\s*(#|$)' "$f"; break; }
done
for f in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
  [ -f "$f" ] && { echo "## $f"; cat "$f"; break; }
done
echo "## drm connectors";  for c in /sys/class/drm/card*-*; do [ -e "$c/status" ] && echo "$(basename "$c"): $(cat "$c/status") $(head -1 "$c/modes" 2>/dev/null)"; done
echo "## v4l2 devices";    ls /dev/video* 2>/dev/null; for d in /sys/class/video4linux/*; do [ -e "$d/name" ] && echo "$(basename "$d"): $(cat "$d/name")"; done
echo "## alsa cards";      cat /proc/asound/cards 2>/dev/null
echo "## alsa pcm (aplay -L)"; aplay -L 2>/dev/null | grep -vE '^\s' | head -30
echo "## zeroplay";        command -v zeroplay; zeroplay --help 2>&1 | head -3
echo "## zeropfin";         zeropfin --version 2>/dev/null
echo "## zeropfin status (player statusEvents = ZeroPlay fork extensions present)"
zeropfin status 2>/dev/null || echo "(run as root or the zeropfin user while the service runs)"
echo "## groups of zeropfin"; id zeropfin 2>/dev/null
