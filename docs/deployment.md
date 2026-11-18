# Deployment on Raspberry Pi OS Lite 32-bit (Pi 1 Model B / Zero)

Target: Raspberry Pi OS Lite **32-bit, Trixie**, with a working ZeroPlay + composite CRT setup.
Zeropfin never changes display, KMS or audio configuration.

## 1. Install a release (recommended — nothing is compiled on the Pi)

Download `zeropfin-<version>-armhf.zip` from the GitHub release, copy it to the Pi, then:

```sh
unzip zeropfin-<version>-armhf.zip && cd zeropfin-<version>
sudo ./setup.sh                        # checksums, then: apt-get install ./zeropfin_<version>_armhf.deb
sudoedit /etc/zeropfin/zeropfin.conf     # optional: zeroplay_args (analog audio device), port
sudo systemctl restart zeropfin
sudo -u zeropfin zeropfin setup          # Jellyfin URL, username, password (password is not stored)
```

`setup.sh` verifies the bundle and lets apt install the `.deb` together with every runtime library
it declares. The declared list comes from `dpkg-shlibdeps` over both binaries: libmicrohttpd,
libcurl, cJSON, the FFmpeg libraries, libdrm, ALSA and FreeType. The postinst creates the `zeropfin`
system user (in groups `video`, `render` and `audio`) and `/var/lib/zeropfin` (0700), then enables
and starts `zeropfin.service`. You can also skip the script: `sudo apt install ./zeropfin_<version>_armhf.deb`.

Installed files:
- `/usr/bin/zeropfin`
- `/usr/lib/zeropfin/zeroplay` (the bundled ZeroPlay)
- `/usr/share/zeropfin/web/`
- `/usr/lib/systemd/system/zeropfin.service`
- `/etc/zeropfin/zeropfin.conf` (a conffile, so it's preserved on upgrade)
- `/usr/share/doc/zeropfin/` (including `BUILDINFO`, which records the Zeropfin and ZeroPlay commits)

State lives in `/var/lib/zeropfin/` (credentials and sessions). The runtime socket is
`/run/zeropfin/control.sock`. No Node.js, pnpm or compilers are needed on the Pi.

### ZeroPlay
The package bundles ZeroPlay built from `tecncr/zeroplay` (upstream plus the control-mode status
extensions from ADR 0002) at a private path, so an existing `/usr/local/bin/zeroplay` is never
touched. To use your own build instead, set `zeroplay_path` in `/etc/zeropfin/zeropfin.conf`. Stock
upstream builds also work, with positions reported as "estimated". Check the bundled player by hand
(stop the service first, since only one process can hold the display):
`sudo systemctl stop zeropfin; /usr/lib/zeropfin/zeroplay --control`, then type `load /path/to/test.mp4`.

## 2. Build a release yourself (developer machine or CI)

```sh
(cd web && pnpm install --frozen-lockfile && pnpm build)
scripts/build-armv6.sh --test --zeroplay ~/Projects/zeroplay --package --install-test
scripts/package.sh        # dist/zeropfin-<version>-armhf.zip, dist/zeropfin_<version>_armhf.deb, SHA256SUMS
```
Set `ZF_VERSION=x.y.z` for a release version; otherwise the package gets a `0.0.0~dev…` version.
On GitHub, pushing a tag `x.y.z` (or `vx.y.z`) runs the same steps and publishes a release with
`docs/releases/x.y.z.md` as the notes.

## 3. Pair a phone


Open `http://<pi-hostname>.local:8080/` (or the IP) in Safari. Either tap **Show code on TV**, or
run `sudo -u zeropfin zeropfin pair` over SSH, and type the code. Manage remotes with
`zeropfin sessions` / `zeropfin revoke <id|all>`. Optional: Safari → Share → Add to Home Screen.

## Upgrade

Unzip the new bundle and run `sudo ./setup.sh` again (or `sudo apt install ./zeropfin_<new>_armhf.deb`).
dpkg keeps a modified `/etc/zeropfin/zeropfin.conf` (asking before replacing it) and never touches
`/var/lib/zeropfin`; the service is restarted. Uninstall: `sudo apt remove zeropfin`; `sudo apt purge
zeropfin` also deletes `/var/lib/zeropfin` (credentials, paired remotes). The system user is kept.

## Troubleshooting

| Symptom | Check |
|---|---|
| Page does not load from the phone | `systemctl status zeropfin`; firewall; same subnet; `listen_address` |
| `421 Misdirected request` | you used a host name not in `allowed_hosts` |
| `Forbidden: this receiver only accepts LAN clients` | client address not private; add `allowed_networks` |
| "Jellyfin isn't set up" | `sudo -u zeropfin zeropfin setup`, then it reloads automatically |
| "Jellyfin rejected the stored access token" | token revoked in Jellyfin's Devices page: run setup again |
| Player status `restarting`, error in `zeropfin status` | `journalctl -u zeropfin` shows ZeroPlay's stderr; check `zeroplay_path`, groups `video render audio` (`id zeropfin`), that nothing else (another ZeroPlay, a console app) holds the display |
| No sound on the analog jack | set `zeroplay_args = --audio-device plughw:CARD=Headphones,DEV=0` (see `aplay -L`) |
| Item says "Can't play on this receiver" | the reasons are literal; re-encode to H.264 8-bit ≤ level 4.1 with AAC first audio, or adjust limits after testing |
| Position shows "Estimated" | stock ZeroPlay: install the fork build for verified positions |
| Pairing code shown on TV is unwanted | `pairing_show_on_display = false` |
