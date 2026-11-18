#!/usr/bin/env bash
# Verify an ELF really targets the Raspberry Pi 1 / Zero: 32-bit ARM, ARMv6
# architecture tag, VFP hard-float calling convention, no NEON requirement,
# and only expected shared-library dependencies. Prints the evidence.
set -euo pipefail
any_deps=0
if [ "${1:-}" = --any-deps ]; then any_deps=1; shift; fi
bin="$1"
fail() { echo "ELF CHECK FAILED: $*" >&2; exit 1; }

hdr="$(readelf -h "$bin")"
attrs="$(readelf -A "$bin")"
dyn="$(readelf -d "$bin")"
echo "$hdr" | grep -E 'Class|Machine|Flags'
echo "$attrs" | grep -E 'Tag_CPU_name|Tag_CPU_arch:|Tag_FP_arch|Tag_Advanced_SIMD_arch|Tag_ABI_VFP_args' || true

echo "$hdr" | grep -q 'ELF32' || fail "not a 32-bit ELF"
echo "$hdr" | grep -q 'Machine:.*ARM' || fail "not an ARM binary"
echo "$hdr" | grep -q 'hard-float ABI' || fail "not hard-float"
echo "$attrs" | grep -Eq 'Tag_CPU_arch: v6(KZ|Z|K)?$' || fail "CPU arch tag is not ARMv6 ($(echo "$attrs" | grep Tag_CPU_arch:))"
echo "$attrs" | grep -q 'Tag_ABI_VFP_args: VFP registers' || fail "VFP argument passing (armhf) not declared"
if echo "$attrs" | grep -q 'Tag_Advanced_SIMD_arch: NEON'; then fail "NEON required (not available on ARM1176)"; fi
if echo "$attrs" | grep -Eq 'Tag_FP_arch: (VFPv3|VFPv4|FP for ARMv8)'; then fail "FPU newer than VFPv2 required"; fi

echo "NEEDED:"; echo "$dyn" | awk '/NEEDED/ {print "  " $NF}'
allowed='^\[(libmicrohttpd\.so\.12|libcurl\.so\.4|libcjson\.so\.1|libc\.so\.6|libm\.so\.6|ld-linux-armhf\.so\.3)\]$'
[ "$any_deps" = 1 ] && { echo "ELF check passed: ARMv6 hard-float ($(basename "$bin"))"; exit 0; }
for lib in $(echo "$dyn" | awk '/NEEDED/ {print $NF}'); do
  [[ "$lib" =~ $allowed ]] || fail "unexpected dependency $lib"
done
echo "ELF check passed: ARMv6 hard-float, dependencies OK"
