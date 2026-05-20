// miniOS - A single-binary minimal Linux system
// Based on mini-rv32ima by Charles Lohr (MIT/BSD/CC0)
// Snapshot and embedding additions are MIT licensed.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// Default DTB for 64MB RAM
#include "default64mbdtc.h"

// Embedded kernel image (when compiled with EMBED_IMAGE)
#ifdef EMBED_IMAGE
#include "embedded_image.h"
#endif

// Zstd compressed embedded kernel image (when compiled with EMBED_IMAGE_ZST)
#ifdef EMBED_IMAGE_ZST
#include "embedded_image_zst.h"
#endif

// --- Configuration ---
static uint32_t ram_amt = 64*1024*1024;
static int fail_on_all_faults = 0;
static const char *snapshot_save_path = "snapshot.bin";
static const char *snapshot_load_path = NULL;
static volatile int save_requested = 0;

// --- Forward declarations ---
static uint32_t HandleException( uint32_t ir, uint32_t retval );
static uint32_t HandleControlStore( uint32_t addy, uint32_t val );
static uint32_t HandleControlLoad( uint32_t addy );
static void HandleOtherCSRWrite( uint8_t * image, uint16_t csrno, uint32_t value );
static int32_t HandleOtherCSRRead( uint8_t * image, uint16_t csrno );
static int IsKBHit();
static int ReadKBByte();

// --- Emulator configuration macros ---
#define MINIRV32WARN( x... ) printf( x );
#define MINIRV32_DECORATE static
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC( pc, ir, retval ) { if( retval > 0 ) { if( fail_on_all_faults ) { printf( "FAULT\n" ); return 3; } else retval = HandleException( ir, retval ); } }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL( addy, val ) if( HandleControlStore( addy, val ) ) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL( addy, rval ) rval = HandleControlLoad( addy );
#define MINIRV32_OTHERCSR_WRITE( csrno, value )  HandleOtherCSRWrite( image, csrno, value );
#define MINIRV32_OTHERCSR_READ( csrno, value ) value = HandleOtherCSRRead( image, csrno );

#include "mini-rv32ima.h"

// --- Global state ---
static uint8_t *ram_image = NULL;
static struct MiniRV32IMAState *core;

// =========================================================================
// Snapshot save/load (with zstd -9 compression)
// =========================================================================

#include "zstd.h"

#define SNAPSHOT_MAGIC 0x4D494E49  // "MINI"
#define SNAPSHOT_VERSION 2         // v2 = compressed

struct SnapshotHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t ram_size;
	uint32_t state_size;
	uint32_t compressed_size;  // 0 = uncompressed (v1 compat)
};

static int snapshot_save(const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "\nminiOS: failed to open '%s' for writing\n", path);
		return -1;
	}

	// Compress RAM with zstd level 9
	size_t comp_bound = ZSTD_compressBound(ram_amt);
	uint8_t *comp_buf = malloc(comp_bound);
	if (!comp_buf) {
		fprintf(stderr, "\nminiOS: failed to allocate compression buffer\n");
		fclose(f);
		return -1;
	}

	size_t comp_size = ZSTD_compress(comp_buf, comp_bound, ram_image, ram_amt, 9);
	if (ZSTD_isError(comp_size)) {
		fprintf(stderr, "\nminiOS: compression failed: %s\n", ZSTD_getErrorName(comp_size));
		free(comp_buf);
		fclose(f);
		return -1;
	}

	struct SnapshotHeader hdr = {
		.magic = SNAPSHOT_MAGIC,
		.version = SNAPSHOT_VERSION,
		.ram_size = ram_amt,
		.state_size = sizeof(struct MiniRV32IMAState),
		.compressed_size = (uint32_t)comp_size
	};

	fwrite(&hdr, sizeof(hdr), 1, f);
	fwrite(core, sizeof(struct MiniRV32IMAState), 1, f);
	fwrite(comp_buf, comp_size, 1, f);
	fclose(f);
	free(comp_buf);

	fprintf(stderr, "\nminiOS: snapshot saved to '%s' (%u MB -> %.1f MB compressed, zstd-9)\n",
		path, ram_amt / (1024*1024), (float)comp_size / (1024*1024));
	return 0;
}

static int snapshot_load(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "miniOS: failed to open '%s' for reading\n", path);
		return -1;
	}

	struct SnapshotHeader hdr;
	if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
		fprintf(stderr, "miniOS: failed to read snapshot header\n");
		fclose(f);
		return -1;
	}

	if (hdr.magic != SNAPSHOT_MAGIC) {
		fprintf(stderr, "miniOS: invalid snapshot file (bad magic)\n");
		fclose(f);
		return -1;
	}

	if (hdr.version != SNAPSHOT_VERSION && hdr.version != 1) {
		fprintf(stderr, "miniOS: unsupported snapshot version %u\n", hdr.version);
		fclose(f);
		return -1;
	}

	// Adjust RAM size to match snapshot
	ram_amt = hdr.ram_size;
	ram_image = realloc(ram_image, ram_amt);
	if (!ram_image) {
		fprintf(stderr, "miniOS: failed to allocate %u bytes for RAM\n", ram_amt);
		fclose(f);
		return -1;
	}

	// Core state lives at end of RAM
	core = (struct MiniRV32IMAState *)(ram_image + ram_amt - sizeof(struct MiniRV32IMAState));

	if (fread(core, sizeof(struct MiniRV32IMAState), 1, f) != 1) {
		fprintf(stderr, "miniOS: failed to read CPU state\n");
		fclose(f);
		return -1;
	}

	if (hdr.version == 1 || hdr.compressed_size == 0) {
		// Uncompressed v1 format
		if (fread(ram_image, ram_amt, 1, f) != 1) {
			fprintf(stderr, "miniOS: failed to read RAM image\n");
			fclose(f);
			return -1;
		}
	} else {
		// Compressed v2 format (zstd)
		uint8_t *comp_buf = malloc(hdr.compressed_size);
		if (!comp_buf) {
			fprintf(stderr, "miniOS: failed to allocate decompression buffer\n");
			fclose(f);
			return -1;
		}
		if (fread(comp_buf, hdr.compressed_size, 1, f) != 1) {
			fprintf(stderr, "miniOS: failed to read compressed data\n");
			free(comp_buf);
			fclose(f);
			return -1;
		}
		size_t result = ZSTD_decompress(ram_image, ram_amt, comp_buf, hdr.compressed_size);
		free(comp_buf);
		if (ZSTD_isError(result)) {
			fprintf(stderr, "miniOS: snapshot decompression failed: %s\n", ZSTD_getErrorName(result));
			fclose(f);
			return -1;
		}
	}

	fclose(f);
	fprintf(stderr, "miniOS: snapshot restored from '%s'\n", path);
	return 0;
}

// =========================================================================
// Terminal I/O
// =========================================================================

static struct termios orig_termios;

static void ResetKeyboardInput() {
	tcsetattr(0, TCSANOW, &orig_termios);
}

static void CaptureKeyboardInput() {
	tcgetattr(0, &orig_termios);
	atexit(ResetKeyboardInput);

	struct termios term = orig_termios;
	term.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(0, TCSANOW, &term);
}

static int is_eofd = 0;

static int ReadKBByte() {
	if (is_eofd) return 0xffffffff;
	char rxchar = 0;
	int rread = read(fileno(stdin), &rxchar, 1);
	if (rread > 0) return rxchar;
	return -1;
}

static int IsKBHit() {
	if (is_eofd) return -1;
	int byteswaiting;
	ioctl(0, FIONREAD, &byteswaiting);
	if (!byteswaiting && write(fileno(stdin), 0, 0) != 0) { is_eofd = 1; return -1; }
	return !!byteswaiting;
}

// =========================================================================
// Signal handler for snapshot (Ctrl+\)
// =========================================================================

static void handle_sigquit(int sig) {
	(void)sig;
	save_requested = 1;
}

// =========================================================================
// MMIO handlers
// =========================================================================

static uint32_t HandleException(uint32_t ir, uint32_t code) {
	return code;
}

static uint32_t HandleControlStore(uint32_t addy, uint32_t val) {
	if (addy == 0x10000000) { // UART data
		putchar(val);
		fflush(stdout);
	} else if (addy == 0x11004004) { // CLINT timermatchh
		core->timermatchh = val;
	} else if (addy == 0x11004000) { // CLINT timermatchl
		core->timermatchl = val;
	} else if (addy == 0x11100000) { // SYSCON (poweroff/reboot)
		core->pc = core->pc + 4;
		return val;
	}
	return 0;
}

static uint32_t HandleControlLoad(uint32_t addy) {
	if (addy == 0x10000005)
		return 0x60 | IsKBHit();
	else if (addy == 0x10000000 && IsKBHit())
		return ReadKBByte();
	else if (addy == 0x1100bffc)
		return core->timerh;
	else if (addy == 0x1100bff8)
		return core->timerl;
	return 0;
}

static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value) {
	if (csrno == 0x136) { printf("%d", value); fflush(stdout); }
	else if (csrno == 0x137) { printf("%08x", value); fflush(stdout); }
	else if (csrno == 0x138) {
		uint32_t ptrstart = value - MINIRV32_RAM_IMAGE_OFFSET;
		uint32_t ptrend = ptrstart;
		if (ptrstart >= ram_amt) { printf("DEBUG PASSED INVALID PTR (%08x)\n", value); return; }
		while (ptrend < ram_amt) { if (image[ptrend] == 0) break; ptrend++; }
		if (ptrend != ptrstart) fwrite(image + ptrstart, ptrend - ptrstart, 1, stdout);
	}
	else if (csrno == 0x139) { putchar(value); fflush(stdout); }
}

static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno) {
	(void)image;
	if (csrno == 0x140) {
		if (!IsKBHit()) return -1;
		return ReadKBByte();
	}
	return 0;
}

// =========================================================================
// Time
// =========================================================================

static uint64_t GetTimeMicroseconds() {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_usec + ((uint64_t)(tv.tv_sec)) * 1000000LL;
}

// =========================================================================
// Main
// =========================================================================

static void print_usage(const char *prog) {
	fprintf(stderr,
		"miniOS - A single-binary minimal Linux system\n"
		"\n"
		"Usage: %s [options] [-f <kernel_image>]\n"
		"\n"
		"Options:\n"
		"  -f <file>       Linux kernel image to boot (required if no snapshot)\n"
		"  --load <file>   Restore from snapshot file\n"
		"  --save <file>   Snapshot save path (default: snapshot.bin)\n"
		"  -m <MB>         RAM size in megabytes (default: 64)\n"
		"  -h, --help      Show this help\n"
		"\n"
		"Controls:\n"
		"  Ctrl+\\          Save snapshot and exit\n"
		"  Ctrl+C          Exit without saving\n"
		"\n", prog);
}

int main(int argc, char **argv) {
	const char *image_file = NULL;
	const char *dtb_file = NULL;
	const char *kernel_command_line = NULL;

	// Parse arguments
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0 && i+1 < argc) {
			image_file = argv[++i];
		} else if (strcmp(argv[i], "--load") == 0 && i+1 < argc) {
			snapshot_load_path = argv[++i];
		} else if (strcmp(argv[i], "--save") == 0 && i+1 < argc) {
			snapshot_save_path = argv[++i];
		} else if (strcmp(argv[i], "-m") == 0 && i+1 < argc) {
			ram_amt = atoi(argv[++i]) * 1024 * 1024;
		} else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) {
			dtb_file = argv[++i];
		} else if (strcmp(argv[i], "-k") == 0 && i+1 < argc) {
			kernel_command_line = argv[++i];
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		} else {
			// Treat as image file if no flag
			if (!image_file) image_file = argv[i];
		}
	}

	if (!image_file && !snapshot_load_path) {
#if defined(EMBED_IMAGE) || defined(EMBED_IMAGE_ZST)
		// Use embedded image - no file needed
#else
		print_usage(argv[0]);
		fprintf(stderr, "Error: must specify -f <kernel_image> or --load <snapshot>\n");
		return 1;
#endif
	}

	// Allocate RAM
	ram_image = malloc(ram_amt);
	if (!ram_image) {
		fprintf(stderr, "miniOS: failed to allocate %u MB RAM\n", ram_amt / (1024*1024));
		return 1;
	}
	memset(ram_image, 0, ram_amt);

	// Core state lives at the end of RAM
	core = (struct MiniRV32IMAState *)(ram_image + ram_amt - sizeof(struct MiniRV32IMAState));

	if (snapshot_load_path) {
		// --- Restore from snapshot ---
		if (snapshot_load(snapshot_load_path) != 0) return 1;
	} else {
		// --- Fresh boot ---
		long flen = 0;

		if (image_file) {
			// Load from file
			FILE *f = fopen(image_file, "rb");
			if (!f) {
				fprintf(stderr, "miniOS: cannot open '%s'\n", image_file);
				return 1;
			}
			fseek(f, 0, SEEK_END);
			flen = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (flen > (long)ram_amt) {
				fprintf(stderr, "miniOS: kernel image too large (%ld bytes > %u)\n", flen, ram_amt);
				fclose(f);
				return 1;
			}
			if (fread(ram_image, flen, 1, f) != 1) {
				fprintf(stderr, "miniOS: failed to read kernel image\n");
				fclose(f);
				return 1;
			}
			fclose(f);
		}
#ifdef EMBED_IMAGE
		else {
			// Use embedded image
			flen = images_Image_len;
			if (flen > (long)ram_amt) {
				fprintf(stderr, "miniOS: embedded image too large\n");
				return 1;
			}
			memcpy(ram_image, images_Image, flen);
		}
#endif
#ifdef EMBED_IMAGE_ZST
		else {
			// Decompress embedded zstd image
			unsigned long long const decompSize = ZSTD_getFrameContentSize(images_Image_zst, images_Image_zst_len);
			if (decompSize == ZSTD_CONTENTSIZE_ERROR || decompSize == ZSTD_CONTENTSIZE_UNKNOWN) {
				fprintf(stderr, "miniOS: invalid zstd data\n");
				return 1;
			}
			if (decompSize > ram_amt) {
				fprintf(stderr, "miniOS: decompressed image too large\n");
				return 1;
			}
			size_t result = ZSTD_decompress(ram_image, ram_amt, images_Image_zst, images_Image_zst_len);
			if (ZSTD_isError(result)) {
				fprintf(stderr, "miniOS: zstd decompression failed: %s\n", ZSTD_getErrorName(result));
				return 1;
			}
			flen = result;
			fprintf(stderr, "miniOS: decompressed kernel %u -> %ld bytes (zstd)\n",
				images_Image_zst_len, flen);
		}
#endif

		// Load DTB
		int dtb_ptr = 0;
		if (dtb_file) {
			FILE *df = fopen(dtb_file, "rb");
			if (!df) { fprintf(stderr, "miniOS: cannot open DTB '%s'\n", dtb_file); return 1; }
			fseek(df, 0, SEEK_END);
			long dtblen = ftell(df);
			fseek(df, 0, SEEK_SET);
			dtb_ptr = ram_amt - dtblen - sizeof(struct MiniRV32IMAState);
			fread(ram_image + dtb_ptr, dtblen, 1, df);
			fclose(df);
		} else {
			// Use default DTB
			dtb_ptr = ram_amt - sizeof(default64mbdtb) - sizeof(struct MiniRV32IMAState);
			memcpy(ram_image + dtb_ptr, default64mbdtb, sizeof(default64mbdtb));
			if (kernel_command_line) {
				strncpy((char*)(ram_image + dtb_ptr + 0xc0), kernel_command_line, 54);
			}
		}

		// Initialize CPU state
		core->pc = MINIRV32_RAM_IMAGE_OFFSET;
		core->regs[10] = 0x00;  // hart ID
		core->regs[11] = dtb_ptr ? (dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET) : 0;  // dtb_pa
		core->extraflags |= 3;  // Machine-mode

		// Update RAM size in DTB if using default
		if (!dtb_file) {
			uint32_t *dtb = (uint32_t*)(ram_image + dtb_ptr);
			if (dtb[0x13c/4] == 0x00c0ff03) {
				uint32_t validram = dtb_ptr;
				dtb[0x13c/4] = (validram>>24) | (((validram>>16)&0xff)<<8) | (((validram>>8)&0xff)<<16) | ((validram&0xff)<<24);
			}
		}
	}

	// Setup terminal and signals
	CaptureKeyboardInput();
	signal(SIGQUIT, handle_sigquit);  // Ctrl+\ to save

	fprintf(stderr, "miniOS: booting... (Ctrl+\\ to save snapshot, Ctrl+C to quit)\n");

	// --- Main execution loop ---
	uint64_t lastTime = GetTimeMicroseconds();
	int instrs_per_flip = 1024;

	while (1) {
		// Check if snapshot was requested
		if (save_requested) {
			snapshot_save(snapshot_save_path);
			break;
		}

		uint64_t *this_ccount = ((uint64_t*)&core->cyclel);
		uint32_t elapsedUs = GetTimeMicroseconds() - lastTime;
		lastTime += elapsedUs;

		int ret = MiniRV32IMAStep(core, ram_image, 0, elapsedUs, instrs_per_flip);
		switch (ret) {
			case 0: break;
			case 1: usleep(500); *this_ccount += instrs_per_flip; break;
			case 3: goto done;
			case 0x7777: // restart
				fprintf(stderr, "miniOS: system restart requested\n");
				// Re-init would go here; for now just exit
				goto done;
			case 0x5555: // poweroff
				fprintf(stderr, "miniOS: system powered off\n");
				goto done;
			default: break;
		}
	}

done:
	ResetKeyboardInput();
	free(ram_image);
	return 0;
}
