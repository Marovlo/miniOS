#!/bin/bash
# Build a static, headless RVVM emulator binary for the host platform.
# Output: src/rvvm/release.linux.<arch>/rvvm_<arch>  (statically linked, ~1.2 MB)
#
# Used by scripts/build-single.sh as input.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RVVM_DIR="$PROJECT_DIR/src/rvvm"

[ -d "$RVVM_DIR" ] || { echo "ERROR: $RVVM_DIR not found"; exit 1; }

cd "$RVVM_DIR"

# Clean previous build to avoid stale dynamic-linked artifacts.
make clean >/dev/null 2>&1 || true

# Headless + static build:
#   USE_GUI=0       no display server
#   USE_SDL/X11/WAYLAND=0  drop all GUI backends
#   USE_SOUND=0     no ALSA/audio
#   USE_VFIO=0      no PCI passthrough (needs root + libvfio-user)
#   USE_NO_DLIB=1   no dlopen() probes -> truly static
#   USE_NET=1       keep userspace slirp networking (for DHCP/SSH)
#   USE_JIT=1       keep RVJIT (huge perf win on x86_64/arm64)
make CC="${CC:-gcc}" \
     USE_GUI=0 USE_SDL=0 USE_X11=0 USE_WAYLAND=0 \
     USE_SOUND=0 USE_VFIO=0 USE_NO_DLIB=1 \
     USE_NET=1 USE_JIT=1 USE_LIB=0 \
     LDFLAGS="-static -static-libgcc" \
     CFLAGS="-O2" \
     -j"$(nproc 2>/dev/null || echo 4)"

ARCH="$(uname -m)"
OUT="$RVVM_DIR/release.linux.${ARCH}/rvvm_${ARCH}"
[ -f "$OUT" ] || { echo "ERROR: build did not produce $OUT"; exit 1; }
strip "$OUT"

echo ""
echo "=== RVVM static build complete ==="
echo "  Output: $OUT ($(du -h "$OUT" | cut -f1))"
file "$OUT" | sed 's/^/  /'
ldd "$OUT" 2>&1 | sed 's/^/  /'
