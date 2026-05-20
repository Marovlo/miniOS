# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

miniOS is a single-file, self-contained Linux system. It embeds a RISC-V virtual machine (RVVM), OpenSBI firmware, Linux 6.6.70 kernel, and an ext4 rootfs into one static executable (~15 MB). Running it launches a full rv64gc Linux with networking, NVMe storage, and SSH — no dependencies required. Supports detach/reattach (VM runs in background).

## Build Commands

```bash
# Full pipeline (on Linux x86_64 with build deps installed):
./scripts/build-images.sh       # Build kernel + rootfs via buildroot (~40 min first time, ~30s incremental)
./scripts/build-rvvm-static.sh  # Build static RVVM binary (~30s)
./scripts/build-single.sh       # Pack everything into single executable (~15s)

# Quick rebuild after editing launcher.c only:
gcc -O2 -Wall src/launcher/launcher.c \
    build/embed/rvvm.zst.o build/embed/fw_jump.bin.zst.o \
    build/embed/Image.zst.o build/embed/rootfs.img.zst.o \
    -static -static-libgcc /usr/lib64/libzstd.a -lutil \
    -o dist/minios-linux-x86_64 && strip dist/minios-linux-x86_64

# Development-mode run (without single-file packing):
make run    # Uses RVVM directly with images/ directory
```

## Architecture

The system has three layers:

1. **launcher** (`src/launcher/launcher.c`) — C program with daemon/client architecture:
   - **Server (daemon)**: decompresses zstd blobs, starts RVVM on a PTY, listens on a Unix socket for client connections. Stays alive in background.
   - **Client**: connects to server socket, sets terminal to raw mode, relays I/O with escape sequence detection (`Ctrl+A;D` to detach).
   - Links against libzstd and libutil (for `openpty`) statically. Binary blobs embedded via `ld -r -b binary` producing symbols like `_binary_rvvm_zst_start/end`.

2. **RVVM** (`src/rvvm/`, gitignored) — Upstream RISC-V VM, built headless+static with: `USE_GUI=0 USE_SDL=0 USE_X11=0 USE_WAYLAND=0 USE_SOUND=0 USE_VFIO=0 USE_NO_DLIB=1 USE_NET=1 USE_JIT=1 LDFLAGS="-static -static-libgcc"`. Produces ~1.2 MB binary.

3. **Guest images** (built by buildroot, gitignored) — `images/fw_jump.bin` (OpenSBI), `images/Image` (Linux kernel), `images/rootfs.img` (ext4 with busybox+dropbear+dhcpcd). The kernel must have NVMe/R8169/EXT4 as `=y` (builtin, not modules) since there's no initramfs.

## Data Directory Layout

All runtime files live in `<exe-dir>/data/`:
```
data/
├── rootfs.img      (persistent disk image, sparse file)
├── rvvm            (extracted RVVM binary)
├── fw_jump.bin     (extracted firmware)
├── Image           (extracted kernel)
├── minios.sock     (Unix socket for client↔server)
└── minios.pid      (server daemon PID)
```

## Key Design Decisions

- **Daemon + PTY relay**: RVVM runs on a PTY inside a background daemon. Clients connect/disconnect freely via Unix socket. This enables detach/reattach without killing the VM.
- **Sparse file writing**: rootfs.img (2GB logical, ~43MB actual) is written sparse on first extraction — zero pages become filesystem holes via `lseek` skipping.
- Guest images are platform-independent (RISC-V bytecode + ext4) — only the host-side RVVM+launcher needs per-platform builds.
- The `build-images.sh` script post-patches the kernel config after buildroot's initial build to force `CONFIG_BLK_DEV_NVME=y`, `CONFIG_R8169=y`, `CONFIG_EXT4_FS=y` (buildroot's riscv defconfig sets these as `=m`).

## RVVM Runtime Parameters

Default RVVM invocation (hardcoded in launcher.c):
- `-m 512M` (RAM), `-smp 2` (cores), `-nogui`, `-nosound`
- Firmware: `fw_jump.bin`, Kernel: `-k Image`, Disk: `-i rootfs.img`
- User can pass extra args (e.g., `-portfwd tcp/127.0.0.1:2222=22`) which get appended.

## Usage

```bash
./minios                          # Start VM or reattach to running instance
./minios --stop                   # Stop background VM
./minios --reset                  # Factory reset (wipe data dir)
./minios --ephemeral              # One-shot foreground mode (no persist, no detach)
./minios --datadir /path/to/dir   # Custom data directory
```

Controls while attached:
- `Ctrl+A; D` — Detach (VM keeps running)
- `Ctrl+A; X` — Shutdown VM (passed to RVVM)

## Current Status

Linux x86_64 single-file with persistent rootfs + detach/reattach is complete. Pending work:
- Cross-platform: Linux aarch64, macOS arm64/x86_64, Windows x64
- Install tcc + micropython in guest (via network)
- Snapshot save/load (RVVM upstream doesn't support this)

The launcher needs `#ifdef` branches for macOS (no full static linking, different `ld` for binary embedding) and Windows (`CreateProcess` instead of fork/exec, Win32 temp/dir APIs).

## Build Dependencies (Linux)

```
build-essential gcc g++ make wget git bc cpio rsync python3 perl file
libncurses-dev libssl-dev bison flex zstd libzstd-dev
```

## Guest Credentials

- root password: `root`
- SSH via: `-portfwd tcp/127.0.0.1:2222=22` then `ssh root@localhost -p 2222`
