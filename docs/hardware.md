# Hardware notes — owner's working Pi 1B configuration

> **Status: TEMPLATE — to be filled from the owner's Pi.** Nothing below has been observed by the
> Zeropfin developers. Do not change a working display configuration to match this file; record it.

Capture the facts with the read-only script shipped in the release (and in `scripts/`):

```sh
sh capture-hw-info.sh > pi-info.txt
```

## Board
- Model / revision: _(e.g. Raspberry Pi Model B Rev 2, 256 MiB — `/proc/device-tree/model`, `Revision`)_
- RAM total / available at idle: _
- Storage (SD card model/size), power supply: _

## OS
- Raspberry Pi OS release (`/etc/os-release`, `/etc/rpi-issue`): _(Trixie Lite 32-bit, image date)_
- Kernel (`uname -a`): _

## Display (composite CRT, 4:3)
- `/boot/firmware/config.txt` non-comment lines: _
- `/boot/firmware/cmdline.txt` (look for `video=Composite-1:…`, `vc4.tv_norm=`): _
- DRM connectors and modes (`/sys/class/drm/card*-*/{status,modes}`): _
- TV norm (NTSC/PAL), aspect ratio, overscan notes: _

## Audio
- Output (analog 3.5 mm jack): _
- ALSA cards (`/proc/asound/cards`) and the device ZeroPlay uses: _
- Resulting `zeroplay_args` in `/etc/zeropfin/zeropfin.conf`: _

## ZeroPlay
- Install path and how it was built: _
- Upstream commit / fork branch (`feat/control-status-events`?) and patches: _
- Known-good test file (codec, resolution, bitrate, container): _
- Verified manually: `zeroplay --control` + `load <file>` shows picture and sound: _yes/no_

## Network
- Ethernet / Wi-Fi adapter, link speed, Jellyfin server address and version: _
