#!/bin/bash
# Build Linux kernel + rootfs from source using buildroot
# This is optional - you can use the pre-built image instead (make image)
#
# Prerequisites:
#   - git, make, gcc, wget, etc. (standard build tools)
#   - ~5GB disk space
#   - ~30 min build time

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
IMAGES_DIR="$PROJECT_DIR/images"

BUILDROOT_VERSION="2024.02"
BUILDROOT_URL="https://buildroot.org/downloads/buildroot-${BUILDROOT_VERSION}.tar.gz"

echo "=== miniOS: Building kernel + rootfs from source ==="
echo "Build directory: $BUILD_DIR"
echo ""

# Download buildroot
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -d "buildroot-${BUILDROOT_VERSION}" ]; then
    echo "[1/4] Downloading buildroot ${BUILDROOT_VERSION}..."
    wget -q "$BUILDROOT_URL" -O buildroot.tar.gz
    tar xf buildroot.tar.gz
    rm buildroot.tar.gz
else
    echo "[1/4] Buildroot already downloaded."
fi

cd "buildroot-${BUILDROOT_VERSION}"

# Apply miniOS configuration
echo "[2/4] Configuring buildroot..."
cat > .config << 'EOF'
BR2_riscv=y
BR2_RISCV_32=y
BR2_riscv_g=y
BR2_TOOLCHAIN_BUILDROOT_MUSL=y
BR2_LINUX_KERNEL=y
BR2_LINUX_KERNEL_CUSTOM_VERSION=y
BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="6.1"
BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG=y
BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="$(BR2_EXTERNAL)/linux.config"
BR2_LINUX_KERNEL_IMAGE_TARGET_CUSTOM=y
BR2_LINUX_KERNEL_IMAGE_TARGET_NAME="Image"
BR2_TARGET_ROOTFS_INITRAMFS=y
BR2_PACKAGE_BUSYBOX=y
BR2_PACKAGE_BUSYBOX_SHOW_OTHERS=y
BR2_TARGET_GENERIC_GETTY_PORT="console"
BR2_SYSTEM_DHCP="eth0"
EOF

echo "[3/4] Building (this may take 20-40 minutes)..."
make olddefconfig
make -j$(nproc)

# Copy outputs
echo "[4/4] Copying outputs..."
mkdir -p "$IMAGES_DIR"
cp output/images/Image "$IMAGES_DIR/Image"

echo ""
echo "=== Build complete ==="
echo "Kernel: $IMAGES_DIR/Image"
echo ""
echo "Run with: cd $PROJECT_DIR && make run"
