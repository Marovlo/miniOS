# miniOS Makefile
CC ?= cc
CFLAGS = -O2 -Wall -Wno-unused-result
TARGET = miniOS
SRC = src/main.c
IMAGE = images/Image

.PHONY: all clean run image help standalone

all: $(TARGET)

$(TARGET): $(SRC) src/mini-rv32ima.h src/default64mbdtc.h
	$(CC) $(CFLAGS) -I src -o $@ $(SRC)

# Build standalone single-binary (kernel embedded in executable)
standalone: $(IMAGE) src/embedded_image.h
	$(CC) $(CFLAGS) -DEMBED_IMAGE -I src -o $(TARGET) $(SRC)
	@echo "Built standalone miniOS ($$(du -h $(TARGET) | cut -f1), no external files needed)"

# Build standalone with gzip-compressed kernel (smaller binary)
standalone-gz: $(IMAGE) src/embedded_image_gz.h
	$(CC) $(CFLAGS) -DEMBED_IMAGE_GZ -I src -o $(TARGET) $(SRC) src/miniz.c
	@echo "Built compressed miniOS ($$(du -h $(TARGET) | cut -f1), no external files needed)"

src/embedded_image.h: $(IMAGE)
	xxd -i images/Image > src/embedded_image.h

src/embedded_image_gz.h: $(IMAGE)
	gzip -k -9 -f images/Image
	xxd -i images/Image.gz > src/embedded_image_gz.h
	rm -f images/Image.gz

# Download pre-built Linux kernel image for rv32
image: $(IMAGE)

$(IMAGE):
	@mkdir -p images
	@echo "Downloading pre-built Linux kernel image for rv32..."
	@curl -sL "https://github.com/cnlohr/mini-rv32ima-images/raw/master/images/linux-6.1.14-rv32nommu-cnl-1.zip" -o images/linux.zip
	@cd images && unzip -o linux.zip && rm -f linux.zip
	@echo "Done. Image saved to $(IMAGE) ($$(du -h $(IMAGE) | cut -f1))"

# Run miniOS
run: $(TARGET) $(IMAGE)
	./$(TARGET) -f $(IMAGE)

# Run with snapshot restore
restore: $(TARGET)
	./$(TARGET) --load snapshot.bin

clean:
	rm -f $(TARGET) src/embedded_image.h

help:
	@echo "miniOS build targets:"
	@echo "  make            - Build miniOS (needs -f <image> to run)"
	@echo "  make standalone - Build single-file miniOS (kernel embedded, no deps)"
	@echo "  make image      - Download pre-built Linux kernel image"
	@echo "  make run        - Build and run miniOS"
	@echo "  make restore    - Restore from snapshot.bin"
	@echo "  make clean      - Remove built files"
	@echo ""
	@echo "Runtime controls:"
	@echo "  Ctrl+\\          - Save snapshot and exit"
	@echo "  Ctrl+C          - Exit without saving"
