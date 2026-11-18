# Third-party components

## Shipped in the release package (`zeropfin_<version>_armhf.deb`)

| Component | Where | License |
|---|---|---|
| ZeroPlay, built from tecncr/zeroplay (fork of HorseyofCoursey/zeroplay with control-mode extensions) | `/usr/lib/zeropfin/zeroplay`; license text in `/usr/share/doc/zeropfin/zeroplay-LICENSE` | MIT |
| Inter variable font (Latin, Latin Extended) via @fontsource-variable/inter 5.3.0 | `usr/share/zeropfin/web/assets/inter-*.woff2` | SIL Open Font License 1.1 — full text in `usr/share/zeropfin/web/licenses/Inter-OFL.txt` |
| Alpine.js (@alpinejs/csp 3.17.5) | bundled in `assets/index-*.js` | MIT, © Caleb Porzio and contributors |
| Tailwind CSS 4.3.5 (generated CSS only) | `assets/style-*.css` | MIT, © Tailwind Labs, Inc. |

## Runtime dependencies installed by apt from Raspberry Pi OS (declared, not bundled)

| Library | Debian package (Trixie) | License |
|---|---|---|
| GNU libmicrohttpd | `libmicrohttpd12t64` | LGPL-2.1-or-later |
| libcurl | `libcurl4t64` | curl (MIT-style) |
| cJSON | `libcjson1` | MIT |
| FFmpeg (libavformat, libavcodec, libavutil, libswresample, libswscale), used by ZeroPlay | `libavformat61` etc. | LGPL-2.1+ / GPL-2+ depending on the Raspberry Pi OS build |
| libdrm, ALSA (libasound2), FreeType, used by ZeroPlay | `libdrm2`, `libasound2t64`, `libfreetype6` | MIT / LGPL-2.1+ / FTL or GPL-2 |

The exact package list comes from `dpkg-shlibdeps` and is shown in the `.deb`'s `Depends` field.

## Development only (never on the Pi)
Vite, vitest, playwright-core, @tailwindcss/vite — see `web/pnpm-lock.yaml` for exact versions.
