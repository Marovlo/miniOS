# miniOS 当前进展与迁移指南

## 当前状态

### 已完成
1. **v0.1 (mini-rv32ima 版本)** — 已打 tag `v0.1-mini-rv32ima`
   - 单文件 1.8MB 可执行
   - 能启动 Linux（nommu）
   - 快照 save/load（zstd-9 压缩，64MB→1.9MB）
   - 内核 zstd-19 压缩嵌入

2. **切换到 RVVM** — 已完成
   - RVVM 源码在 `src/rvvm/`（已编译通过 macOS ARM64 + Linux x86_64）
   - OpenSBI v1.4 固件已下载到 `images/fw_jump.bin`
   - 验证 OpenSBI 在 RVVM 上正常启动

3. **rv64 Linux 镜像构建** — ✅ 已完成
   - buildroot 2024.02.9 + Linux 6.6.70 + musl
   - `images/Image` 22MB（kernel，含 NVMe/R8169/EXT4 builtin）
   - `images/rootfs.img` 2GB（busybox + dropbear + dhcpcd + wget + nano + htop + make）
   - Guest 镜像与宿主平台无关（RISC-V 字节码 + ext4），一次构建全平台复用

4. **端到端烟测通过** — ✅ 已验证
   - OpenSBI → Linux 6.6.70 启动
   - NVMe 控制器识别，挂载 ext4 rootfs
   - RTL8169 网卡识别，DHCP 拿到 192.168.0.100
   - dropbear SSH 启动，`minios login:` 提示符正常出现

5. **v0.2 单文件可执行（Linux x86_64）** — ✅ 已完成
   - 产物：`dist/minios-linux-x86_64`，**~15 MB 单文件**，纯静态链接
   - 含 RVVM + OpenSBI + Linux 6.6.70 + ext4 rootfs，**无任何外部依赖**
   - 实现：zstd 压缩 + `ld -r -b binary` 嵌入 + 静态链接

6. **持久化文件系统** — ✅ 已完成
   - 默认 rootfs.img 持久化到 `<exe-dir>/data/` 目录
   - 稀疏文件写入（2GB 逻辑 → ~43MB 物理磁盘占用）
   - 命令行参数：`--ephemeral` / `--reset` / `--datadir PATH`

7. **Detach/Reattach（PTY relay 守护进程）** — ✅ **已完成 (2026-05-20)**
   - 架构：server daemon（管理 PTY + Unix socket）+ client（终端透传）
   - 首次运行启动后台守护进程；再次运行自动重连（秒级）
   - 转义序列：`Ctrl+A;D` 分离 / `Ctrl+A;X` 关机 / `Ctrl+A;?` 帮助
   - 按下 `Ctrl+A` 后显示提示菜单（`D=detach X=shutdown ?=help`）
   - 4KB 环形回放缓冲区：重连时回放最近输出（立刻看到 shell 提示符）
   - `--stop` 杀掉后台实例
   - 所有运行时文件统一在 `<exe-dir>/data/`（rootfs.img, rvvm, fw_jump.bin, Image, minios.sock, minios.pid）

### 压缩比一览
| 资源 | 原始 | 压缩后 | 比率 |
|---|---|---|---|
| `fw_jump.bin` | 264 K | 67 K | 25% |
| `Image` (zstd -19) | 22 M | 6.0 M | 28% |
| `rootfs.img` (zstd -9) | 2 GB (43 M used) | 7.1 M | 0.3% |
| `rvvm` (zstd -19) | 1.2 M | 449 K | 37% |
| **总产物（单文件）** | — | **~15 M** | — |

### 当前卡点
无。Linux x86_64 版本功能完整。**下一步：macOS arm64 跨平台适配。**

---

## 下一步：macOS arm64 单文件适配

### 目标
在 Mac（Apple Silicon）上实现 `./minios-macos-arm64` 单文件运行，体验与 Linux 版一致。

### 核心原则
Guest 三件套（`fw_jump.bin` / `Image` / `rootfs.img`）已构建完毕，**不需要重新编译**。
只需在 Mac 上编译 host 侧组件（RVVM + launcher）并打包。

### 需要适配的差异

#### 1. RVVM 编译（Mac 版）
- RVVM 上游已原生支持 macOS（Makefile 内有 Darwin 分支）
- 不能 `-static`（macOS 不支持全静态），但只依赖 `libSystem.dylib`（系统稳定 ABI，所有 Mac 都有）
- 编译参数：
  ```bash
  make CC=clang \
       USE_GUI=0 USE_SDL=0 USE_X11=0 USE_WAYLAND=0 \
       USE_SOUND=0 USE_VFIO=0 USE_NO_DLIB=1 \
       USE_NET=1 USE_JIT=1 USE_LIB=0 \
       -j$(sysctl -n hw.ncpu)
  ```
- 产物：`release.darwin.arm64/rvvm_arm64`

#### 2. launcher.c 适配点
| API | Linux | macOS | 需要改动 |
|-----|-------|-------|----------|
| `readlink("/proc/self/exe")` | ✅ | ❌ 无 procfs | 改用 `_NSGetExecutablePath()` |
| `openpty()` | `<pty.h>` | `<util.h>` | `#ifdef __APPLE__` 切换 header |
| `nftw()` | ✅ | ✅ | 无需改动 |
| `fork/execv/waitpid` | ✅ | ✅ | 无需改动 |
| `poll()` | ✅ | ✅ | 无需改动 |
| Unix socket | ✅ | ✅ | 无需改动 |
| 链接 `-lutil` | 需要 | 不需要（openpty 在 libSystem 中） | 条件编译 |

#### 3. 二进制嵌入方式
- Linux 用 `ld -r -b binary file.zst` 产生 `.o` 文件
- macOS 的 ld64 不支持 `-b binary`，替代方案：
  - **方案 A**：用 `ld -r -sectcreate __DATA __rvvm_zst file.zst -o file.o`（Mac linker 原生支持）
  - **方案 B**：用 `llvm-objcopy --input-target=binary --output-target=mach-o-arm64` 转换
  - **方案 C**：用 `xxd -i` 生成 C 数组（最便携，但编译慢）
  - **推荐方案 A**（最简洁、零额外依赖），需要调整 launcher 中 blob 的引用方式

#### 4. 链接方式
- 不能 `-static`，但可以尽量减少动态依赖：
  ```bash
  clang -O2 -Wall launcher.c embed/*.o -lzstd -o minios-macos-arm64
  # 或者静态链 libzstd：
  clang -O2 -Wall launcher.c embed/*.o /opt/homebrew/lib/libzstd.a -o minios-macos-arm64
  ```
- 最终产物只依赖 `/usr/lib/libSystem.B.dylib`（所有 macOS 系统自带）

### 实施步骤

```bash
# === 在 Mac 上执行 ===

# 0. 克隆仓库
git clone https://github.com/Marovlo/miniOS && cd miniOS

# 1. 克隆 RVVM 并编译 Mac 版
git clone https://github.com/LekKit/RVVM src/rvvm
cd src/rvvm
make CC=clang USE_GUI=0 USE_SDL=0 USE_X11=0 USE_WAYLAND=0 \
     USE_SOUND=0 USE_VFIO=0 USE_NO_DLIB=1 USE_NET=1 USE_JIT=1 USE_LIB=0 \
     -j$(sysctl -n hw.ncpu)
cd ../..

# 2. 准备 guest 镜像（从 Linux 服务器拷贝，或从 release 下载）
# images/fw_jump.bin, images/Image, images/rootfs.img

# 3. 压缩 + 嵌入（需要写 macOS 版 build-single-macos.sh）
# zstd 压缩各资源 → ld -sectcreate 嵌入 → 链接 launcher

# 4. 测试
./dist/minios-macos-arm64
```

### 预估工作量
| 任务 | 预估时间 |
|------|----------|
| launcher.c `#ifdef __APPLE__` 适配 | 30 分钟 |
| `scripts/build-single-macos.sh` 打包脚本 | 1 小时 |
| RVVM 编译验证 | 10 分钟 |
| 端到端测试 | 30 分钟 |
| **合计** | **~2 小时** |

---

## 项目文件结构

```
miniOS/
├── PROGRESS.md          ← 本文件
├── CLAUDE.md            ← Claude Code 指引
├── README.md            ← 项目说明
├── Makefile             ← 构建入口
├── .gitignore
├── configs/
│   ├── buildroot_defconfig    ← buildroot 配置
│   └── linux_fragment.config  ← 内核额外配置
├── scripts/
│   ├── build-images.sh        ← guest 镜像构建（buildroot，仅 Linux）
│   ├── build-rvvm-static.sh   ← RVVM 静态编译（Linux）
│   └── build-single.sh        ← 单文件打包（Linux，zstd + ld -r -b binary）
├── src/
│   ├── rvvm/                  ← RVVM 源码（git clone，gitignored）
│   └── launcher/launcher.c    ← 启动器（daemon + PTY relay + client）
├── images/                    ← guest 镜像（gitignored，由 build-images.sh 产生）
│   ├── fw_jump.bin
│   ├── Image
│   └── rootfs.img
├── dist/                      ← 产物（gitignored）
│   └── minios-linux-x86_64
└── build/                     ← 中间产物（可删除重建）
    ├── buildroot-2024.02.9/
    └── embed/*.zst.o
```

---

## 关键参数速查

| 项目 | 值 |
|------|-----|
| buildroot 版本 | 2024.02.9 |
| Linux 内核版本 | 6.6.70 LTS |
| 架构 | riscv64 (rv64gc) |
| C 库 | musl |
| 文件系统 | ext4, 2GB |
| RVVM 网卡 | RTL8169 (CONFIG_R8169) |
| RVVM 存储 | NVMe (CONFIG_BLK_DEV_NVME) |
| RVVM 串口 | 8250 UART |
| 默认 RAM | 512MB |
| root 密码 | root |
| SSH 端口转发 | 宿主机 2222 → 虚拟机 22 |

---

## 给下一个 AI 的接续指引

### Git 状态
- 仓库：`https://github.com/Marovlo/miniOS`，branch `master`
- 最新已推送 commit：`3f466b2` (config: increase rootfs size 256M → 2G)
- 本地未推送改动：launcher.c 重写（PTY relay + detach/reattach + sparse write + 回放缓冲）

### 当前环境（云服务器 /data/workspace/miniOS）
- `build/buildroot-2024.02.9/` — buildroot 已编译完成，增量 rebuild 秒级
- `src/rvvm/` — RVVM 源码，已编译静态版于 `release.linux.x86_64/rvvm_x86_64`
- `images/` — fw_jump.bin + Image + rootfs.img 就绪
- `dist/minios-linux-x86_64` — ~15 MB 单文件产物（含 PTY relay 版 launcher）
- `build/embed/*.zst.o` — 已打好的 embed object 文件

### 一键重建命令（Linux x86_64）
```bash
cd /data/workspace/miniOS
# launcher 改动后快速重编（~5 秒）：
gcc -O2 -Wall src/launcher/launcher.c \
    build/embed/rvvm.zst.o build/embed/fw_jump.bin.zst.o \
    build/embed/Image.zst.o build/embed/rootfs.img.zst.o \
    -static -static-libgcc /usr/lib64/libzstd.a -lutil \
    -o dist/minios-linux-x86_64 && strip dist/minios-linux-x86_64

# 全量重建：
./scripts/build-images.sh      # ~25 min（增量 ~30 sec）
./scripts/build-rvvm-static.sh # ~30 sec
./scripts/build-single.sh      # ~15 sec
```

### 立即下一步：macOS arm64 适配
见上文「下一步：macOS arm64 单文件适配」章节。核心任务：
1. launcher.c 加 `#ifdef __APPLE__`（`_NSGetExecutablePath`、`<util.h>` for openpty）
2. 写 `scripts/build-single-macos.sh`（用 `ld -sectcreate` 替代 `ld -r -b binary`）
3. 在 Mac 上编译 RVVM + 链接 launcher + 验证

### 设计决策记录
- **为什么用 RVVM 而不是 QEMU**：单文件 1.2 MB 静态链接，30K LOC 可维护，上游活跃
- **为什么 guest 是 RISC-V 而不是 x86**：RVVM 只支持 RISC-V；x86 模拟器太大
- **为什么不用 initramfs**：rootfs 直接挂 NVMe ext4 更简单，持久化只需保留一个文件
- **为什么用 PTY relay 而不是 tmux/SSH**：保持零依赖原则，单文件自包含
- **性能定位**：RVVM + RVJIT 比原生慢 5-20x，定位教学/演示/轻量沙盒
- **持久化策略**：默认持久化（rootfs 在 data/），--ephemeral 退出即丢

---

## 关键修复记录

- buildroot RISC-V 内核配置必须用 `BR2_LINUX_KERNEL_USE_ARCH_DEFAULT_CONFIG=y`（不能用 `USE_DEFCONFIG="defconfig"`）
- riscv defconfig 默认 `NVMe/EXT4=m`，无 initramfs 时内核 panic → `build-images.sh` 自动强制 `=y`
- buildroot 2024.02.9 的 `linux.hash` 不含 6.6.70 条目 → 脚本兜底处理
- rootfs 必须稀疏写（2GB ext4 文件，零页用 lseek 跳过，物理仅占 ~43MB）
