# ADR 0001 — Native C11 daemon on libmicrohttpd, libcurl and cJSON

Status: accepted (2026-11-18)

## Context
The receiver must run on a 256 MiB Raspberry Pi 1 (ARM1176, ARMv6) next to ZeroPlay, with no
interpreter or JS runtime on the device.

## Decision
A single `zeropfin` executable in C11 (glibc feature macro `_GNU_SOURCE` for `pipe2`, `accept4`,
`SO_PEERCRED`, `strcasestr`; the language level stays C11) linking only distribution libraries:
libmicrohttpd (HTTP server), libcurl (Jellyfin client + media relay), cJSON (JSON). SHA-256 for
session-token hashing is implemented in-tree (FIPS 180-4 vectors in `tests/unit/test_util.c`) to
avoid pulling in a crypto library. Build system: CMake + Ninja/Make, `ctest` for tests.

Threads, all bounded: one libmicrohttpd event thread for the LAN listener (never blocks; Jellyfin
calls are moved to a 2-thread worker pool with a 32-entry queue via suspend/resume), one player
controller thread, one reporter thread, and thread-per-connection on the loopback relay capped at
6 connections / 4 upstream readers.

## Consequences
- Stripped ARMv6 binary 132 KiB (Raspbian Trixie gcc, `-O3`); x86_64 release ≈ 186 KiB. RSS on
  x86_64 ≈ 14 MiB including shared-library mappings; under qemu-arm the numbers include emulator
  overhead and are not meaningful. Real figures must come from the Pi (`docs/hardware-test-report.md`).
- Runtime packages on Raspberry Pi OS Trixie: `libmicrohttpd12t64`, `libcurl4t64`, `libcjson1`.
