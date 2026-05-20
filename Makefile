# miniOS Makefile (RVVM-based)
RVVM_DIR = src/rvvm
# Auto-detect RVVM binary across host platforms (Linux x86_64, macOS arm64, ...)
RVVM_BIN = $(firstword $(wildcard $(RVVM_DIR)/release.*/rvvm_*))
IMAGES_DIR = images

.PHONY: all clean run help rvvm image

all: rvvm

# Build RVVM emulator
rvvm:
	cd $(RVVM_DIR) && make -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc)
	@echo "RVVM built: $(firstword $(wildcard $(RVVM_DIR)/release.*/rvvm_*))"

# Build Linux kernel + rootfs from source
image:
	./scripts/build-images.sh

# Run miniOS (needs firmware + kernel + rootfs)
run: rvvm
	$(RVVM_BIN) $(IMAGES_DIR)/fw_jump.bin \
		-k $(IMAGES_DIR)/Image \
		-i $(IMAGES_DIR)/rootfs.img \
		-m 512M \
		-portfwd tcp/127.0.0.1:2222=22

clean:
	cd $(RVVM_DIR) && make clean

help:
	@echo "miniOS (RVVM-based) build targets:"
	@echo "  make        - Build RVVM emulator"
	@echo "  make image  - Build Linux kernel + rootfs (40-60 min first time)"
	@echo "  make run    - Run miniOS (needs image built first)"
	@echo "  make clean  - Clean RVVM build"
	@echo ""
	@echo "Features:"
	@echo "  - Full MMU Linux (rv64gc)"
	@echo "  - JIT acceleration (ARM64/x86_64)"
	@echo "  - NVMe storage (rootfs.img)"
	@echo "  - RTL8169 networking (DHCP, SSH on localhost:2222)"
	@echo "  - Ctrl+A; X to exit"
