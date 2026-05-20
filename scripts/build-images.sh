#!/bin/bash
# Build rv64 Linux kernel + rootfs for RVVM
#
# On Linux: just run it, no extra deps beyond standard build tools
#   apt install build-essential gcc g++ make wget git bc cpio rsync python3 perl file libncurses-dev libssl-dev bison flex
#
# On macOS: needs GNU tools from brew (see PROGRESS.md)
#
# Time: ~40-60 min first build, ~5 min incremental
# Disk: ~5GB

set -e

# macOS: ensure GNU tools are in PATH
if [[ "$(uname)" == "Darwin" ]]; then
    export PATH="/tmp/gnubin:/opt/homebrew/opt/findutils/libexec/gnubin:/opt/homebrew/opt/coreutils/libexec/gnubin:/opt/homebrew/opt/gnu-sed/libexec/gnubin:/opt/homebrew/opt/gnu-tar/libexec/gnubin:/opt/homebrew/opt/gawk/libexec/gnubin:/opt/homebrew/bin:$PATH"
    export HOSTCC="${HOSTCC:-gcc-14}"
    export HOSTCXX="${HOSTCXX:-g++-14}"
    mkdir -p /tmp/gnubin
    ln -sf /opt/homebrew/bin/gpatch /tmp/gnubin/patch 2>/dev/null || true
    export CONFIG_SHELL=/opt/homebrew/bin/bash
    export SHELL=/opt/homebrew/bin/bash
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
IMAGES_DIR="$PROJECT_DIR/images"
CONFIGS_DIR="$PROJECT_DIR/configs"

BUILDROOT_VERSION="2024.02.9"
BUILDROOT_URL="https://buildroot.org/downloads/buildroot-${BUILDROOT_VERSION}.tar.gz"

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

echo "=== miniOS: Building rv64 Linux image for RVVM ==="
echo "Build directory: $BUILD_DIR"
echo "Parallel jobs:   $NPROC"
echo "Kernel:          Linux 6.6.70 LTS"
echo ""

# Step 1: Download buildroot
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -d "buildroot-${BUILDROOT_VERSION}" ]; then
    echo "[1/5] Downloading buildroot ${BUILDROOT_VERSION}..."
    if [ ! -f buildroot.tar.gz ]; then
        wget -q --show-progress "$BUILDROOT_URL" -O buildroot.tar.gz
    fi
    tar xf buildroot.tar.gz
    rm -f buildroot.tar.gz
    echo "       Done."
else
    echo "[1/5] Buildroot already present."
fi

BR_DIR="$BUILD_DIR/buildroot-${BUILDROOT_VERSION}"
cd "$BR_DIR"

# Step 2: Apply buildroot defconfig
echo "[2/5] Applying miniOS buildroot configuration..."
cp "$CONFIGS_DIR/buildroot_defconfig" .config
make olddefconfig

# Step 3: Build (buildroot automatically downloads toolchain + all sources)
echo "[3/5] Building everything (toolchain + kernel + rootfs)..."
echo "       This takes 40-60 min on first run. Go grab a coffee."
echo ""
make -j"$NPROC"

# Step 4: Patch kernel config to ensure RVVM drivers are enabled as builtin (=y).
# Buildroot's arch defconfig may have NVMe/EXT4 as =m (modules); but we have no
# initramfs to load modules before mounting root, so they MUST be builtin.
echo "[4/5] Verifying kernel drivers for RVVM (must be =y, not =m)..."
KCONFIG="$BR_DIR/output/build/linux-6.6.70/.config"
if [ -f "$KCONFIG" ]; then
    NEEDS_REBUILD=0
    REQUIRED_Y="CONFIG_R8169 CONFIG_BLK_DEV_NVME CONFIG_NVME_CORE CONFIG_EXT4_FS CONFIG_PCI"
    for opt in $REQUIRED_Y; do
        if grep -q "^${opt}=y" "$KCONFIG"; then
            continue
        fi
        # Either =m or unset -> force =y
        echo "       Forcing ${opt}=y..."
        sed -i.bak -e "/^${opt}=/d" -e "/^# ${opt} is not set/d" "$KCONFIG"
        echo "${opt}=y" >> "$KCONFIG"
        NEEDS_REBUILD=1
    done
    rm -f "${KCONFIG}.bak"
    if [ "$NEEDS_REBUILD" -eq 1 ]; then
        echo "       Re-running olddefconfig + rebuilding kernel..."
        ( cd "$BR_DIR/output/build/linux-6.6.70" && \
          make ARCH=riscv \
               CROSS_COMPILE="$BR_DIR/output/host/bin/riscv64-buildroot-linux-musl-" \
               olddefconfig )
        # Drop stamps to force kernel rebuild + rootfs repack
        rm -f "$BR_DIR/output/build/linux-6.6.70/.stamp_built" \
              "$BR_DIR/output/build/linux-6.6.70/.stamp_target_installed" \
              "$BR_DIR/output/build/linux-6.6.70/.stamp_images_installed"
        rm -f "$BR_DIR/output/images/rootfs.ext2" \
              "$BR_DIR/output/images/rootfs.tar"
        ( cd "$BR_DIR" && make -j"$NPROC" )
    else
        echo "       All required drivers already builtin (=y)."
    fi
fi

# Step 5: Copy outputs
echo "[5/5] Copying outputs..."
mkdir -p "$IMAGES_DIR"
cp output/images/Image "$IMAGES_DIR/Image"
cp output/images/rootfs.ext2 "$IMAGES_DIR/rootfs.img"

echo ""
echo "=== Build complete ==="
echo "  Kernel: $IMAGES_DIR/Image ($(du -h "$IMAGES_DIR/Image" | cut -f1))"
echo "  Rootfs: $IMAGES_DIR/rootfs.img ($(du -h "$IMAGES_DIR/rootfs.img" | cut -f1))"
echo ""
echo "Run with:"
echo "  cd $PROJECT_DIR && make run"
