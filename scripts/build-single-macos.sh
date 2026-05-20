#!/bin/bash
# Build miniOS single-file executable for macOS (arm64 or x86_64).
#
# Prerequisites:
#   - Xcode command-line tools (clang, ld)
#   - brew install zstd
#   - RVVM compiled: src/rvvm/release.darwin.*/rvvm_*
#   - Guest images: images/fw_jump.bin, images/Image, images/rootfs.img
#
# Output: dist/minios-macos-<arch>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
EMBED_DIR="$BUILD_DIR/embed"
DIST_DIR="$PROJECT_DIR/dist"
IMAGES_DIR="$PROJECT_DIR/images"
RVVM_DIR="$PROJECT_DIR/src/rvvm"

ARCH="$(uname -m)"  # arm64 or x86_64

echo "=== miniOS: Building single-file for macOS ${ARCH} ==="

# Verify prerequisites
command -v clang >/dev/null || { echo "ERROR: clang not found. Install Xcode CLT."; exit 1; }
command -v zstd  >/dev/null || { echo "ERROR: zstd not found. Run: brew install zstd"; exit 1; }

# Locate RVVM binary
RVVM_BIN=$(find "$RVVM_DIR" -path "*/release.darwin.*/rvvm_*" -type f ! -name "*.o" ! -name "*.d" | head -1)
[ -n "$RVVM_BIN" ] || { echo "ERROR: RVVM not compiled. Run: cd src/rvvm && make ..."; exit 1; }
echo "  RVVM binary: $RVVM_BIN"

# Verify guest images
for f in fw_jump.bin Image rootfs.img; do
    [ -f "$IMAGES_DIR/$f" ] || { echo "ERROR: $IMAGES_DIR/$f not found."; exit 1; }
done

# Create dirs
mkdir -p "$EMBED_DIR" "$DIST_DIR"

# Step 1: Compress assets with zstd
echo "[1/4] Compressing assets..."
zstd -19 -f -q "$RVVM_BIN"             -o "$EMBED_DIR/rvvm.zst"
zstd -19 -f -q "$IMAGES_DIR/fw_jump.bin" -o "$EMBED_DIR/fw_jump.bin.zst"
zstd -19 -f -q "$IMAGES_DIR/Image"       -o "$EMBED_DIR/Image.zst"
zstd -9  -f -q "$IMAGES_DIR/rootfs.img"  -o "$EMBED_DIR/rootfs.img.zst"

echo "  rvvm.zst:         $(du -h "$EMBED_DIR/rvvm.zst" | cut -f1)"
echo "  fw_jump.bin.zst:  $(du -h "$EMBED_DIR/fw_jump.bin.zst" | cut -f1)"
echo "  Image.zst:        $(du -h "$EMBED_DIR/Image.zst" | cut -f1)"
echo "  rootfs.img.zst:   $(du -h "$EMBED_DIR/rootfs.img.zst" | cut -f1)"

# Step 2: Generate assembly files with .incbin for each blob
# This is the cross-platform way to embed binary data on macOS (no ld -b binary).
echo "[2/4] Generating embed assembly..."

generate_embed_asm() {
    local name="$1"    # e.g., "rvvm_zst"
    local file="$2"    # e.g., "build/embed/rvvm.zst"
    local asm_file="$EMBED_DIR/${name}.s"
    local abs_file
    abs_file="$(cd "$(dirname "$file")" && pwd)/$(basename "$file")"

    cat > "$asm_file" <<EOF
    .section __DATA,__const
    .globl __binary_${name}_start
    .globl __binary_${name}_end
    .p2align 3
__binary_${name}_start:
    .incbin "${abs_file}"
__binary_${name}_end:
EOF
}

generate_embed_asm "rvvm_zst"         "$EMBED_DIR/rvvm.zst"
generate_embed_asm "fw_jump_bin_zst"  "$EMBED_DIR/fw_jump.bin.zst"
generate_embed_asm "Image_zst"        "$EMBED_DIR/Image.zst"
generate_embed_asm "rootfs_img_zst"   "$EMBED_DIR/rootfs.img.zst"

# Step 3: Compile assembly to object files
echo "[3/4] Assembling embed objects..."
for name in rvvm_zst fw_jump_bin_zst Image_zst rootfs_img_zst; do
    clang -c "$EMBED_DIR/${name}.s" -o "$EMBED_DIR/${name}.o"
done

# Step 4: Compile launcher and link everything
echo "[4/4] Compiling and linking..."

# Find libzstd.a (prefer static to avoid runtime dependency)
LIBZSTD=""
for candidate in \
    /opt/homebrew/lib/libzstd.a \
    /usr/local/lib/libzstd.a \
    "$(brew --prefix zstd 2>/dev/null)/lib/libzstd.a"; do
    if [ -f "$candidate" ]; then
        LIBZSTD="$candidate"
        break
    fi
done
[ -n "$LIBZSTD" ] || { echo "ERROR: libzstd.a not found. Run: brew install zstd"; exit 1; }

OUTPUT="$DIST_DIR/minios-macos-${ARCH}"

clang -O2 -Wall \
    "$PROJECT_DIR/src/launcher/launcher.c" \
    "$EMBED_DIR/rvvm_zst.o" \
    "$EMBED_DIR/fw_jump_bin_zst.o" \
    "$EMBED_DIR/Image_zst.o" \
    "$EMBED_DIR/rootfs_img_zst.o" \
    "$LIBZSTD" \
    -I"$(brew --prefix zstd 2>/dev/null)/include" \
    -o "$OUTPUT"

strip "$OUTPUT"

echo ""
echo "=== Build complete ==="
echo "  Output: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
file "$OUTPUT" | sed 's/^/  /'
echo ""
echo "  Run with: ./$OUTPUT"
echo "  Dependencies:"
otool -L "$OUTPUT" | grep -v "^$OUTPUT" | sed 's/^/    /'
