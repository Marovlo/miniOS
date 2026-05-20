# miniOS Makefile (RVVM-based)
RVVM_DIR = src/rvvm
RVVM_BIN = $(RVVM_DIR)/release.darwin.arm64/rvvm_arm64
TARGET = miniOS
IMAGES_DIR = images

.PHONY: all clean run help rvvm image

all: rvvm

# Build RVVM
rvvm:
	cd $(RVVM_DIR) && make -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc)
	@echo "RVVM built: $(RVVM_BIN)"

# Download firmware + Linux image
image:
	@echo "TODO: Download/build rv64 Linux image with tcc + micropython"

# Run miniOS
run: rvvm
	$(RVVM_BIN) $(IMAGES_DIR)/fw_jump.bin -k $(IMAGES_DIR)/Image -i $(IMAGES_DIR)/rootfs.img -m 512M

clean:
	cd $(RVVM_DIR) && make clean

help:
	@echo "miniOS (RVVM-based) build targets:"
	@echo "  make        - Build RVVM emulator"
	@echo "  make image  - Download/build Linux image"
	@echo "  make run    - Run miniOS"
	@echo "  make clean  - Clean build"
