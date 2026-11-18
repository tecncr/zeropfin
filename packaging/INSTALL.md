# Installing Zeropfin

For Raspberry Pi OS Lite **32-bit Trixie** on a Raspberry Pi 1 Model B or Zero / Zero W whose
composite display already works with the kernel's DRM/KMS driver.

```sh
unzip zeropfin-<version>-armhf.zip
cd zeropfin-<version>
sudo ./setup.sh
sudo -u zeropfin zeropfin setup
```

Then open `http://<pi-hostname>.local:8080/` in Safari and pair.

The bundle contains:
- `zeropfin_<version>_armhf.deb`, with the Zeropfin daemon, the web remote and a ZeroPlay build
  that includes the control-mode status extensions, installed at `/usr/lib/zeropfin/zeroplay`.
- `setup.sh`, which checks the bundle and installs the package with apt so dependencies are
  resolved.
- `SHA256SUMS`.

You can also install the `.deb` directly: `sudo apt install ./zeropfin_<version>_armhf.deb`.
Uninstall with `sudo apt remove zeropfin`; `sudo apt purge zeropfin` also deletes credentials and
paired remotes.

Zeropfin never changes your display or audio configuration. Analog audio usually needs
`zeroplay_args = --audio-device plughw:CARD=Headphones,DEV=0` in `/etc/zeropfin/zeropfin.conf`.
Full documentation is in `/usr/share/doc/zeropfin/`.
