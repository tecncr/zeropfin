# ADR 0002 — ZeroPlay as a supervised `--control` child, with opt-in status extensions

Status: accepted (2026-11-18)

## Context
Upstream ZeroPlay control mode (checked in `src/main.c` of the user's fork, upstream commit
0347d99) documents `load/loadloop/pause/resume/stop/seek/quit` and the events `ready`/`ended`.
It also implements (undocumented) `osd <ms> <text>` / `osdclear` → `osd_ok`. Findings that matter:

1. A failed `load` produces **no stdout event** — only `zeroplay: failed to open '…'` on stderr.
2. There is **no position, duration or state telemetry**.
3. `--pos` applies to every `load` in control mode, so resume needs `load` + `seek`.
4. Unknown commands are silently ignored.
5. ZeroPlay uses the first H.264 video, the **first** audio and the **first** subtitle stream;
   there is no track selection.

## Decision
- Zeropfin spawns exactly one `zeroplay --control [allowlisted args]` with `posix_spawn` (no shell,
  absolute configured path, reset signal mask/dispositions, own process group), private pipes,
  and a pidfd for exit detection. Commands are validated (`zp_proto.c`): loopback relay URLs only,
  no CR/LF/NUL/space/parentheses, overflow-checked milliseconds.
- A minimal, backward-compatible patch to the fork (branch `feat/control-status-events` in
  `~/Projects/zeroplay`, uncommitted, intended for upstream) adds:
  `status` → `status <idle|playing|paused|image> <posMs> <durMs>`; `events on|off` → ack and
  enables `loaded <durMs>` / `error load`; `loadat <ms> <path>`.
- Zeropfin probes with `events on` after `ready` (1.5 s timeout). With the extension it polls
  `status` every second and labels positions **verified**; without it, positions are a monotonic
  estimate labelled **estimated**, load failures are detected from stderr within the load timeout,
  and playback is assumed to have started 2 s after `load`.
- Stale `ended`: with extensions, events before `loaded`/`error load` of a new item belong to the
  previous one (events are written in command order). Stock: an `ended` within 1.5 s after a load
  is attributed to the previous item. Residual ambiguity is documented in `docs/architecture.md`.
- A crash is never completion: an unexpected exit marks the item ERROR, reports
  `Failed: true` with the last verified (or estimated, if allowed) position, and restarts the
  player with exponential backoff (1 s → 30 s).

## Consequences
Works with stock upstream ZeroPlay (tested with the mock in `--stock` mode); better accuracy
with the fork. The patch must still be compiled and exercised on the Pi (NEEDS HARDWARE VALIDATION).
