#!/bin/bash
# Build single-file miniOS executable for the host platform.
#
# Inputs (must already exist):
#   src/rvvm/release.linux.<arch>/rvvm_<arch>   (static, prebuilt)
#   images/fw_jump.bin
#   images/Image
#   images/rootfs.img
#
# Output:
#   dist/minios-linux-<arch>     (single static binary, ~14MB)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

ARCH="$(uname -m)"           # x86_64 / aarch64 / ...
OS_TAG="linux-${ARCH}"
EMBED_DIR="build/embed"
DIST_DIR="dist"
LAUNCHER_SRC="src/launcher/launcher.c"
RVVM_SRC="src/rvvm/release.linux.${ARCH}/rvvm_${ARCH}"

mkdir -p "$EMBED_DIR" "$DIST_DIR"

echo "[1/4] Verifying inputs..."
for f in "$RVVM_SRC" images/fw_jump.bin images/Image images/rootfs.img "$LAUNCHER_SRC"; do
    [ -f "$f" ] || { echo "  ERROR: missing $f"; exit 1; }
done
file "$RVVM_SRC" | grep -q "statically linked" \
    || { echo "  ERROR: $RVVM_SRC is not statically linked"; exit 1; }
echo "       OK"

echo "[2/4] Compressing assets (zstd)..."
cp -f "$RVVM_SRC" "$EMBED_DIR/rvvm"
zstd -19 -fq -o "$EMBED_DIR/rvvm.zst"        "$EMBED_DIR/rvvm"
zstd -19 -fq -o "$EMBED_DIR/fw_jump.bin.zst" images/fw_jump.bin
zstd -19 -fq -o "$EMBED_DIR/Image.zst"       images/Image
zstd -9  -fq -o "$EMBED_DIR/rootfs.img.zst"  images/rootfs.img
ls -lh "$EMBED_DIR"/*.zst

echo "[3/4] Building embed object files (ld -r -b binary)..."
# `ld -r -b binary` emits symbols based on the *input file basename*, with
# non-alphanumerics replaced by '_'. We rely on these symbol names in
# launcher.c (e.g. _binary_rvvm_zst_start).
( cd "$EMBED_DIR" && for n in rvvm.zst fw_jump.bin.zst Image.zst rootfs.img.zst; do
    ld -r -b binary -o "${n}.o" "$n"
  done )

echo "[4/4] Linking launcher..."
OUT="${DIST_DIR}/minios-${OS_TAG}"
gcc -O2 -Wall -Wextra \
    "$LAUNCHER_SRC" \
    "$EMBED_DIR/rvvm.zst.o" \
    "$EMBED_DIR/fw_jump.bin.zst.o" \
    "$EMBED_DIR/Image.zst.o" \
    "$EMBED_DIR/rootfs.img.zst.o" \
    -static -static-libgcc \
    /usr/lib64/libzstd.a \
    -o "$OUT"
strip "$OUT"

echo ""
echo "=== Build complete ==="
echo "  Output: $OUT ($(du -h "$OUT" | cut -f1))"
file "$OUT"
echo ""
echo "Run:    ./$OUT"
echo "        ./$OUT -portfwd tcp/127.0.0.1:2222=22   # forward host:2222 -> guest:22"
