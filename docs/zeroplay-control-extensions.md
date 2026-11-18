# ZeroPlay control-mode extensions (proposed upstream change)

Location: `~/Projects/zeroplay`, branch `feat/control-status-events` (uncommitted working tree),
based on upstream `0347d99`. Files: `src/main.c`, `README.md`. Compiles against Raspbian Trixie's
FFmpeg (ARMv6, in `scripts/build-armv6.sh --zeroplay`). **Not yet run on a Pi.**

All additions are backward compatible: existing commands and the `ready`/`ended` events are
unchanged, new output appears only on request, and unknown commands were already ignored.

| Command | Reply / effect |
|---|---|
| `status` | `status <idle\|playing\|paused\|image> <positionMs> <durationMs>` — position is the PTS of the last presented frame (`current_pts`); `durationMs` 0 when unknown |
| `events on` / `events off` | `events on` / `events off`; while on, every `load`/`loadloop`/`loadat` is acknowledged with `loaded <durationMs>` or `error load` |
| `loadat <positionMs> <path>` | like `load`, starting at the given position (control mode otherwise applies the global `--pos` to every clip) |

Why: without these, a controller cannot tell a failed load from a slow one, cannot report a
trustworthy playback position, and cannot attribute an `ended` event to the right clip after a
quick replacement. Ordering guarantee used by Zeropfin: ZeroPlay writes events in command order, so
an `ended` read before a clip's `loaded`/`error load` belongs to the previous clip.

Manual test on the Pi:
```sh
printf 'events on\nloadat 60000 /home/pi/test.mp4\nstatus\npause\nstatus\nquit\n' | zeroplay --control
# expect: ready, events on, loaded <ms>, status playing ~60000 <ms>, status paused ...
```
