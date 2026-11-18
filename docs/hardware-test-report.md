# Hardware test report

> **Status: NEEDS HARDWARE VALIDATION.** As of 2026-11-18 no Zeropfin build has run on a
> Raspberry Pi. Everything in `docs/testing.md` ran on x86_64 (native) or under qemu-user
> emulating an ARM1176 CPU. Fill in one section per test session; keep raw numbers.

## Session template

- Date / tester: _
- Pi model, revision, RAM: _
- Raspberry Pi OS release, kernel: _
- Zeropfin version (`zeropfin --version`, both lines): _
- ZeroPlay commit / fork branch, `statusEvents` in `zeropfin status`: _
- Jellyfin server version and host; network (Ethernet/Wi-Fi): _
- Display configuration (composite, norm, mode): _
- Media under test (container, H.264 profile/level, resolution, fps, bitrate, audio): _

## Checklist

| # | Test | Procedure | Pass criteria | Result |
|---|---|---|---|---|
| H1 | Install | `sudo ./setup.sh` from the release zip on a fresh Trixie Lite 32-bit | apt resolves all dependencies; service active; `readelf -A /usr/bin/zeropfin` shows `v6`; `ldd` finds every library for `zeropfin` and `/usr/lib/zeropfin/zeroplay` | |
| H2 | Setup | `sudo -u zeropfin zeropfin setup` | token stored 0600; password not on disk (`grep -r` for it fails) | |
| H3 | Pairing | Safari → Show code on TV, and `zeropfin pair` | code readable on the CRT; pairing works; wrong code rejected | |
| H4 | Composite picture | play the known-good H.264 file | correct aspect, no tearing/flash between items | |
| H5 | Analog audio | same | audible, in sync (clap test at start and after 30 min) | |
| H6 | Pause/resume | 5× each | resumes from the same frame; position correct ±1 s | |
| H7 | Seek | ±30 s, scrub to 10/50/90 % | lands within ±2 s (verified) / note error (estimated); latency in seconds | |
| H8 | Resume from Jellyfin | start an item with a resume point | starts at the saved position | |
| H9 | Jellyfin reporting | watch Dashboard → Activity / Now playing | session visible as Zeropfin; progress updates; stop position saved; finished item marked played | |
| H10 | Incompatible media | HEVC and Hi10P items | refused with a clear reason, nothing sent to ZeroPlay | |
| H11 | Replacement | start B while A plays | A reported stopped once; B plays | |
| H12 | Crash | `sudo pkill -9 zeroplay` during playback | UI shows error; Jellyfin not marked played; player back to `ready` | |
| H13 | Service restart | `systemctl restart zeropfin` while playing | clean stop report; sessions survive; UI recovers | |
| H14 | Safari sleep | lock the phone 10 min during playback, unlock | playback continued on the Pi; UI resyncs within 2 s | |
| H15 | Wi-Fi loss (phone) | toggle phone Wi-Fi | "Can't reach the receiver" banner, recovers | |
| H16 | Jellyfin loss | stop Jellyfin during playback | playback continues from buffer, then stalls/errors visibly; reports fail quietly; recovers on restart | |
| H17 | 2-hour endurance | play a ≥2 h H.264 file | completes; no audio drift; no OOM; stats below | |
| H18 | SD writes | `cat /proc/$(pidof zeropfin)/io` before/after H17 | `write_bytes` ≈ 0 during playback | |
| H19 | Reboot | power-cycle | service starts; ZeroPlay ready; pairing persists | |

## Measurements (H17)

| Metric | Command | Idle | Browsing | Playing (relay active) |
|---|---|---|---|---|
| MemAvailable | `grep MemAvailable /proc/meminfo` | | | |
| zeropfin RSS / PSS | `grep -E 'VmRSS|Pss' /proc/$(pidof zeropfin)/smaps_rollup` | | | |
| zeroplay RSS | `grep VmRSS /proc/$(pidof zeroplay)/status` | | | |
| zeropfin CPU % | `pidstat -p $(pidof zeropfin) 5` | | | |
| zeroplay CPU % | `pidstat -p $(pidof zeroplay) 5` | | | |
| relay throughput | `zeropfin status` bytesOut over time | | | |
| seek latency | stopwatch: tap → picture | | | |
| dropped frames / stalls | observation + journal | | | |

Targets (not yet measured): zeropfin idle RSS ≤ 16 MiB, browsing ≤ 32 MiB, excluding ZeroPlay.
