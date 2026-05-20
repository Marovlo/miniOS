# miniOS

一个单文件运行的极简 Linux 系统。基于 mini-rv32ima (RISC-V 模拟器) + buildroot Linux + busybox，支持快照保存/恢复。

## 目标

- **单文件运行**：编译后得到一个可执行文件，无外部依赖，随处可跑
- **完整 Linux**：真正的 Linux 内核 + busybox 工具集
- **快照**：随时保存系统状态，下次从断点恢复
- **可扩展**：可加网络、编译器等组件

## 架构

```
┌─────────────────────────────────────────┐
│         miniOS (单个可执行文件)           │
│                                         │
│  ┌───────────────────────────────────┐  │
│  │  RISC-V 模拟器 (mini-rv32ima)     │  │
│  │  - RV32IMA CPU 模拟              │  │
│  │  - 64MB RAM                      │  │
│  │  - UART (终端 I/O)               │  │
│  │  - CLINT (定时器中断)             │  │
│  │  - 快照 save/load                │  │
│  ├───────────────────────────────────┤  │
│  │  内嵌数据                         │  │
│  │  - Linux 内核 (rv32, ~1MB)       │  │
│  │  - rootfs (busybox, ~1MB)        │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
```

## 快速开始

### 依赖

仅需宿主机 C 编译器（gcc 或 clang）：

```bash
# macOS
xcode-select --install

# Ubuntu/Debian
sudo apt install build-essential
```

### 编译 & 运行

```bash
make          # 编译 miniOS
./miniOS      # 启动，进入 Linux shell
```

### 快照

```bash
./miniOS                        # 正常启动
# 在 shell 中工作...
# 按 Ctrl+\ 保存快照并退出

./miniOS --load snapshot.bin    # 从快照恢复，继续之前的状态
```

### 选项

```
./miniOS [选项]

选项:
  --load <file>     从快照文件恢复运行
  --save <file>     指定快照保存路径 (默认: snapshot.bin)
  --ram <MB>        指定 RAM 大小 (默认: 64)
  --help            显示帮助
```

## 系统内可用命令

基于 busybox，包含 200+ 常用命令，例如：

```
ls, cat, echo, cp, mv, rm, mkdir, rmdir,
vi, grep, find, sed, awk, sort, uniq, wc,
ps, top, kill, free, df, mount,
wget, ping, ifconfig, telnet,
tar, gzip, sh, ash, ...
```

## 项目结构

```
miniOS/
├── README.md
├── Makefile              # 一键编译
├── src/
│   ├── main.c           # 入口：参数解析、启动模拟器
│   ├── mini-rv32ima.h   # CPU 模拟核心 (来自上游)
│   ├── snapshot.c       # 快照 save/load 实现
│   └── snapshot.h
├── images/
│   ├── Image            # Linux 内核 (rv32, 预编译)
│   └── rootfs.bin       # 根文件系统 (busybox, 预编译)
├── configs/
│   ├── buildroot_config # buildroot 配置 (用于重新构建镜像)
│   └── linux_config     # 内核配置
└── scripts/
    └── build-images.sh  # 从源码重新构建内核和 rootfs
```

## 技术细节

### 模拟器

- 基于 [mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)（MIT 许可）
- 实现 RISC-V RV32IMA 指令集 (~47 条指令 + 原子操作 + 乘除法)
- 纯解释执行，约为 QEMU 一半性能，交互使用完全流畅
- Linux 启动约 2-3 秒

### Linux 内核

- Linux 5.x/6.x，使用 buildroot 交叉编译为 rv32
- 最小化配置：关闭网络/SMP/模块等，内核 ~1MB

### 根文件系统

- busybox 静态编译，单文件包含 200+ 命令
- 最终 rootfs 约 1-2MB

### 快照原理

系统全部状态 = CPU 寄存器 (100 字节) + RAM (64MB)。保存/恢复就是序列化这两样东西到文件。对内核完全透明。

## 开发计划

- [x] Phase 1: 基础运行 — 模拟器 + 内核 + shell 跑通
- [ ] Phase 2: 快照 — save/load 状态
- [ ] Phase 3: 单文件打包 — 内核和 rootfs 嵌入可执行文件
- [ ] Phase 4: 网络 — 添加虚拟网卡 + 宿主机桥接
- [ ] Phase 5: 编译器 — 系统内可运行 tcc 编译 C 程序

## 上游项目

| 项目 | 用途 | 许可证 |
|------|------|--------|
| [mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) | RISC-V 模拟器 | MIT |
| [buildroot](https://buildroot.org/) | 构建 Linux 内核 + rootfs | GPL |
| [busybox](https://busybox.net/) | 用户空间工具集 | GPL |

## 许可证

MIT
