# miniOS

一个单文件运行的极简 Linux 系统。基于 RVVM (RISC-V 虚拟机) + 完整 Linux + 网络/存储/编译器。

## 目标

- **单文件运行**：编译后一个可执行文件，随处可跑
- **完整 Linux**：带 MMU 的 rv64 Linux，vi/gcc/python 全部正常
- **网络**：内置用户态网络栈，可联网
- **块设备**：NVMe 存储，挂载宿主机磁盘镜像
- **快照**：保存/恢复系统状态
- **内置工具**：tcc (C 编译器) + micropython

## 架构

```
┌─────────────────────────────────────────────┐
│  miniOS (单个可执行文件)                     │
│                                             │
│  ┌───────────────────────────────────────┐  │
│  │  RVVM (RISC-V 虚拟机)                 │  │
│  │  - rv64imafdcb + JIT 加速             │  │
│  │  - MMU (Sv39/Sv48 分页)               │  │
│  │  - NVMe 块设备                        │  │
│  │  - 用户态网络栈 (RTL8169)             │  │
│  │  - UART 终端                          │  │
│  │  - 快照 save/load                     │  │
│  ├───────────────────────────────────────┤  │
│  │  内嵌数据 (zstd 压缩)                 │  │
│  │  - OpenSBI 固件                       │  │
│  │  - Linux 内核                         │  │
│  │  - rootfs (busybox + tcc + micropython)│  │
│  └───────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

## 快速开始

```bash
make          # 编译 RVVM
make image    # 下载/构建 Linux 镜像
make run      # 启动 miniOS
```

## 相比 v0.1 (mini-rv32ima) 的改进

| 特性 | v0.1 (mini-rv32ima) | v0.2 (RVVM) |
|------|-------|------|
| MMU | ❌ nommu | ✓ 完整 Sv39 |
| 架构 | rv32ima | rv64imafdcb |
| JIT | ❌ 纯解释 | ✓ ARM64/x86_64 JIT |
| 网络 | ❌ | ✓ 用户态网络 |
| 存储 | ❌ (纯 initramfs) | ✓ NVMe |
| vi/编辑器 | ❌ 崩溃 | ✓ 正常 |
| C 编译器 | ❌ | ✓ tcc |
| Python | ❌ | ✓ micropython |
| fork/exec | ⚠️ 受限 | ✓ 完整 |
| 性能 | ~1100 CoreMark | ~10000+ CoreMark (JIT) |

## 上游项目

| 项目 | 用途 | 许可证 |
|------|------|--------|
| [RVVM](https://github.com/LekKit/RVVM) | RISC-V 虚拟机 | GPL-3.0 / MPL-2.0 |
| [OpenSBI](https://github.com/riscv-software-src/opensbi) | M-mode 固件 | BSD-2 |
| [buildroot](https://buildroot.org/) | 构建内核+rootfs | GPL |
| [tcc](https://bellard.org/tcc/) | C 编译器 | LGPL |
| [micropython](https://micropython.org/) | Python 解释器 | MIT |

## 开发计划

- [x] RVVM 编译通过
- [ ] 获取/构建 rv64 Linux 镜像 (含 tcc + micropython)
- [ ] 快照 save/load
- [ ] zstd 压缩嵌入
- [ ] 单文件打包
- [ ] 验证网络/存储/编译

## 许可证

GPL-3.0 (因 RVVM 的 GPL 许可证传染)
