# miniOS 当前进展与迁移指南

## 当前状态

### 已完成
1. **v0.1 (mini-rv32ima 版本)** — 已打 tag `v0.1-mini-rv32ima`
   - 单文件 1.8MB 可执行
   - 能启动 Linux（nommu）
   - 快照 save/load（zstd-9 压缩，64MB→1.9MB）
   - 内核 zstd-19 压缩嵌入

2. **切换到 RVVM** — 已完成
   - RVVM 源码在 `src/rvvm/`（已编译通过 macOS ARM64）
   - OpenSBI v1.4 固件已下载到 `images/fw_jump.bin` + `images/fw_payload.bin`
   - 验证 OpenSBI 在 RVVM 上正常启动

3. **rv64 Linux 镜像构建（云服务器 Linux 上完成）** — ✅ **已完成 (2026-05-20)**
   - buildroot 2024.02.9 + Linux 6.6.70 + musl
   - `images/Image` 22MB（kernel，含 NVMe/R8169/EXT4 builtin）
   - `images/rootfs.img` 256MB（busybox + dropbear + dhcpcd + wget + nano + htop + make）
   - RVVM Linux 版二进制已存在于 `src/rvvm/release.linux.x86_64/rvvm_x86_64`

4. **端到端烟测通过** — ✅ **已验证 (2026-05-20)**
   - OpenSBI → Linux 6.6.70 启动
   - NVMe 控制器（PCI 1f31:4512）识别，挂载 ext4 rootfs
   - RTL8169 网卡识别，DHCP 拿到 192.168.0.100
   - dropbear SSH 启动
   - **`minios login:` 提示符正常出现**

5. **v0.2 单文件可执行（Linux x86_64）** — ✅ **已完成 (2026-05-20)**
   - 产物：`dist/minios-linux-x86_64`，**14.3 MB 单文件**，纯静态链接（`ldd: not a dynamic executable`）
   - 含 RVVM 模拟器 + OpenSBI 固件 + Linux 6.6.70 + ext4 rootfs，**无任何外部依赖**
   - 直接 `./dist/minios-linux-x86_64` 即启动一个完整 Linux，约 5 秒后 `minios login:`
   - 退出后自动清理 `/tmp/minios.XXXXXX` 工作目录
   - 实现要点：
     - RVVM 用 `-static -static-libgcc` + 关闭 GUI/SDL/X11/Wayland/Sound/VFIO/dlopen → 1.2 MB 静态二进制
     - 4 个资源压缩：rvvm/fw_jump/Image (zstd -19) + rootfs.img (zstd -9)
     - launcher.c 通过 `ld -r -b binary` 把 4 个 .zst 嵌入符号 `_binary_<name>_start/end`
     - 启动时 `mkdtemp` → `ZSTD_decompress` → `fork`+`execv rvvm` → `waitpid` → `nftw(rm)`

### 压缩比一览
| 资源 | 原始 | 压缩后 | 比率 |
|---|---|---|---|
| `fw_jump.bin` | 264 K | 67 K | 25% |
| `Image` (zstd -19) | 22 M | 6.0 M | 28% |
| `rootfs.img` (zstd -9) | 256 M (43 M used) | 7.1 M | 16% |
| `rvvm` (zstd -19) | 1.2 M | 449 K | 37% |
| **总产物（单文件）** | — | **14.3 M** | — |

### 当前卡点
无。Linux x86_64 单文件目标达成。下一步：跨平台覆盖。

### 关键修复（已写回脚本，下次重建会自动应用）
- `BR2_LINUX_KERNEL_USE_DEFCONFIG="defconfig"` 会被 buildroot 拼成 `defconfig_defconfig`（不存在）。
  RISC-V 必须用 `BR2_LINUX_KERNEL_USE_ARCH_DEFAULT_CONFIG=y` 直接走 `make ARCH=riscv defconfig`。
- riscv arch defconfig 默认把 `NVME_CORE`/`BLK_DEV_NVME`/`EXT4_FS` 设为 `=m`。
  无 initramfs 时模块来不及加载 → 内核 panic（"Unable to mount root fs"）。
  `scripts/build-images.sh` 的 [4/5] 阶段已升级：检测后强制 `=y` 并触发 kernel rebuild + rootfs 重打包。
- buildroot 2024.02.9 的 `linux.hash` / `linux-headers.hash` 不含 6.6.70 条目。
  `scripts/build-images.sh` 已知会处理（dl 中预存 + 写 hash 兜底；如果想完全避免可选 6.6.63）。
- RVVM 静态构建 useflags（写入 `scripts/build-rvvm-static.sh`，待添加）：
  `USE_GUI=0 USE_SDL=0 USE_X11=0 USE_WAYLAND=0 USE_SOUND=0 USE_VFIO=0 USE_NO_DLIB=1`，
  保留 `USE_NET=1 USE_JIT=1`，链接选项 `-static -static-libgcc`。

### 实测时间（32 核 Linux 环境，本次构建）
- buildroot 全量：约 25 分钟（含下载、host-gcc-initial/final、target packages、kernel build）
- 增量重编 kernel + 重打 rootfs：约 30 秒
- RVVM 静态版编译：约 30 秒
- 单文件打包（zstd 压缩 + 链接）：约 15 秒

### 需要构建的镜像
- **内核**：Linux 6.6 LTS，rv64gc，需要开启以下驱动：
  - `CONFIG_R8169=y`（RTL8169 网卡，RVVM 网络设备）
  - `CONFIG_BLK_DEV_NVME=y`（NVMe，RVVM 块设备）
  - `CONFIG_EXT4_FS=y`
  - `CONFIG_PCI=y`
  - `CONFIG_SERIAL_8250=y` + `CONFIG_SERIAL_8250_CONSOLE=y`
- **根文件系统**：ext4，256MB，包含：
  - busybox（完整工具集）
  - dhcpcd（网络自动配置）
  - dropbear（SSH 服务器）
  - wget/curl
  - nano
  - make

---

## 云服务器上需要做的事

### 1. 安装依赖
```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y build-essential gcc g++ make wget git unzip \
    bc cpio rsync python3 perl file libncurses-dev libssl-dev \
    bison flex
```

### 2. 构建镜像
```bash
cd /path/to/miniOS
./scripts/build-images.sh
```

构建脚本会自动：
- 下载 buildroot 2024.02.9
- 下载并编译 riscv64 交叉工具链
- 编译 Linux 6.6.70 内核
- 编译 busybox + 所有包
- 输出 `images/Image` + `images/rootfs.img`

### 3. 编译 RVVM（Linux 版）
```bash
cd src/rvvm
make clean
make -j$(nproc)
# 产物在 release.linux.x86_64/rvvm_x86_64（或对应架构）
```

### 4. 测试运行
```bash
./src/rvvm/release.linux.*/rvvm_* images/fw_jump.bin \
    -k images/Image \
    -i images/rootfs.img \
    -m 512M \
    -portfwd tcp/127.0.0.1:2222=22
```

### 5. 后续工作
- ✅ 验证网络（DHCP 自动获取 IP，wget 能联网） — 烟测中 udhcpc 已成功拿到 192.168.0.100
- ✅ 验证块设备（rootfs 可读写） — `EXT4-fs (nvme0n1): re-mounted ... r/w`
- ✅ 实现 zstd 压缩嵌入（单文件打包）— `dist/minios-linux-x86_64` 14.3 M，已通过端到端启动测试
- ⏳ 跨平台覆盖：Linux aarch64 / macOS arm64 / macOS x86_64 / Windows x64（见下文路线）
- ⏳ 通过网络安装 tcc 和 micropython
- ⏳ 实现快照 save/load（移植 v0.1 的逻辑）

---

## 跨平台路线（层次 A：每平台一个单文件）

**核心思路**：guest 三件套（`fw_jump.bin` / `Image` / `rootfs.img`）是 RISC-V 字节码 + ext4，**与宿主平台无关**，
一次构建到处复用；host 侧只需为每个目标平台分别编一份"静态 RVVM + launcher"。

### 平台矩阵
| 目标 | 状态 | RVVM 编译 | launcher 链接 | 备注 |
|---|---|---|---|---|
| linux-x86_64 | ✅ 完成 | `gcc -static` | `-static -static-libgcc` | 14.3 M |
| linux-aarch64 | ⏳ 待做 | 同上（需 aarch64 主机或交叉链） | 同上 | 在 ARM Linux 主机上跑 `build-single.sh` 即可 |
| macos-arm64 | ⏳ 待做 | RVVM 上游已支持，需在 mac 上构建 | mac 不支持完全静态，需链 `libSystem.dylib`（系统稳定 ABI） | launcher 需把 `nftw` 换成可移植实现 |
| macos-x86_64 | ⏳ 待做 | 同 macos-arm64 | 同上 | 或 mac universal binary（lipo 合并） |
| windows-x64 | ⏳ 待做 | mingw-w64 静态链接 | 改用 Win32 API（CreateProcess、SHFileOperation 删目录） | launcher 需 `#ifdef _WIN32` 分支 |

### 立刻能做的事
1. **Linux aarch64** —— 如果有 ARM Linux 机器或 Docker `--platform=linux/arm64`，直接跑 `scripts/build-single.sh` 应该开箱可用，
   产物为 `dist/minios-linux-aarch64`。
2. **macOS** —— 把当前 host 上的 guest 三件套（`images/fw_jump.bin Image rootfs.img`）拷到 mac，
   在 mac 上只需重编 RVVM + 跑一份 mac 版 build-single（约 1-2 小时 launcher 适配工作量）。

### Mac/Win 的 launcher 适配点（已识别）
当前 `src/launcher/launcher.c` 用了几个 POSIX-only API：
- `mkdtemp` —— mac 有；Win 用 `GetTempPathW` + `_mkdir` 自实现
- `nftw` —— mac 有；Win 用 `SHFileOperation` 或递归 `FindFirstFile`
- `fork`/`execv`/`waitpid` —— mac 有；Win 用 `CreateProcess` + `WaitForSingleObject`
- `ld -r -b binary` —— mac (`ld -r -platform_version`) 略不同，但 `objcopy` 同样可用；Win 用 `windres` 或 `xxd -i`

**结论**：launcher 改造一次就能跨三平台，约 100-150 行 `#ifdef`。先做 Linux aarch64（零代码改动，最有性价比）。

---

## 项目文件结构

```
miniOS/
├── PROGRESS.md          ← 本文件
├── README.md            ← 项目说明
├── Makefile             ← 构建入口
├── .gitignore
├── configs/
│   ├── buildroot_defconfig    ← buildroot 配置
│   └── linux_fragment.config  ← 内核额外配置
├── scripts/
│   ├── build-images.sh        ← guest 镜像构建脚本（buildroot）
│   └── build-single.sh        ← 单文件打包脚本（zstd 压缩 + ld -r -b binary 嵌入）
├── src/
│   ├── rvvm/                  ← RVVM 源码（git clone）
│   └── launcher/launcher.c    ← 单文件启动器（POSIX）
├── images/
│   ├── fw_jump.bin            ← OpenSBI 固件
│   ├── fw_payload.bin         ← OpenSBI + test payload
│   ├── Image                  ← Linux 6.6.70 内核（22 M）
│   └── rootfs.img             ← ext4 rootfs（256 M，实占 43 M）
├── dist/
│   └── minios-linux-x86_64    ← 单文件可执行（14.3 M，纯静态）
└── build/                     ← buildroot + embed 中间产物（可删除重建）
```

---

## 关键参数速查

| 项目 | 值 |
|------|-----|
| buildroot 版本 | 2024.02.9 |
| Linux 内核版本 | 6.6.70 LTS |
| 架构 | riscv64 (rv64gc) |
| C 库 | musl |
| 文件系统 | ext4, 256MB |
| RVVM 网卡 | RTL8169 (CONFIG_R8169) |
| RVVM 存储 | NVMe (CONFIG_BLK_DEV_NVME) |
| RVVM 串口 | 8250 UART |
| 默认 RAM | 512MB |
| root 密码 | root |
| SSH 端口转发 | 宿主机 2222 → 虚拟机 22 |

---

## 注意事项

- `build/` 目录很大（~5GB），迁移时可以不带，让脚本重新下载
- `src/rvvm/` 是 git clone 的，迁移时也可以重新 clone
- 核心需要迁移的是：`configs/`、`scripts/`、`images/fw_*.bin`、`Makefile`、`README.md`
- buildroot 构建脚本 (`scripts/build-images.sh`) 的 shebang 是 `/opt/homebrew/bin/bash`，在 Linux 上需要改回 `#!/bin/bash`
