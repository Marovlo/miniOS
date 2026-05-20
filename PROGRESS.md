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

### 当前卡点
**需要构建 rv64 Linux 镜像**，macOS 上 buildroot 兼容性问题太多（bash 版本、GNU 工具路径等），需要在真实 Linux 环境构建。

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
- 验证网络（DHCP 自动获取 IP，wget 能联网）
- 验证块设备（rootfs 可读写）
- 通过网络安装 tcc 和 micropython
- 实现快照 save/load（移植 v0.1 的逻辑）
- 实现 zstd 压缩嵌入（单文件打包）

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
│   └── build-images.sh        ← 镜像构建脚本
├── src/
│   └── rvvm/                  ← RVVM 源码（git clone）
├── images/
│   ├── fw_jump.bin            ← OpenSBI 固件（已有）
│   └── fw_payload.bin         ← OpenSBI + test payload（已有）
└── build/                     ← buildroot 构建目录（可删除重建）
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
