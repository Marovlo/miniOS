/*
 * miniOS launcher: self-contained single-file Linux system on RVVM.
 *
 * Embeds 4 zstd-compressed assets via ld -r -b binary:
 *   - rvvm        (static RVVM emulator binary)
 *   - fw_jump.bin (OpenSBI M-mode firmware)
 *   - Image       (Linux kernel)
 *   - rootfs.img  (ext4 root filesystem)
 *
 * Default behavior (persistent mode):
 *   - rootfs.img is extracted to a data directory (~/.local/share/minios/)
 *     on first run and reused across launches (state is preserved).
 *   - rvvm, fw_jump.bin, Image are extracted to a temp dir every time
 *     (they are read-only) and cleaned up on exit.
 *
 * Flags:
 *   --ephemeral   Don't persist rootfs; extract to tmpdir and discard on exit.
 *   --reset       Delete data directory, re-extract rootfs from embedded image.
 *   --datadir P   Use P as data directory instead of the default.
 *   (All other args are passed through to RVVM.)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <ftw.h>

#include <zstd.h>

/* Binary blob declarations (emitted by `ld -r -b binary`) */
#define DECL_BLOB(sym)                                            \
    extern const unsigned char _binary_##sym##_start[];           \
    extern const unsigned char _binary_##sym##_end[]

DECL_BLOB(rvvm_zst);
DECL_BLOB(fw_jump_bin_zst);
DECL_BLOB(Image_zst);
DECL_BLOB(rootfs_img_zst);

#define BLOB_PTR(sym)  (_binary_##sym##_start)
#define BLOB_SIZE(sym) ((size_t)(_binary_##sym##_end - _binary_##sym##_start))

static char g_workdir[512] = {0};

static void die(const char *msg) {
    fprintf(stderr, "minios: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static int rm_cb(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void cleanup_workdir(void) {
    if (g_workdir[0]) {
        nftw(g_workdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        g_workdir[0] = '\0';
    }
}

static void on_signal(int sig) {
    cleanup_workdir();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Decompress a zstd-framed blob to a file (only if file doesn't exist). */
static void extract_blob(const char *outpath,
                         const unsigned char *in, size_t insize,
                         int executable, int overwrite) {
    if (!overwrite) {
        struct stat st;
        if (stat(outpath, &st) == 0 && st.st_size > 0)
            return; /* already exists */
    }
    unsigned long long const decoded = ZSTD_getFrameContentSize(in, insize);
    if (decoded == ZSTD_CONTENTSIZE_ERROR || decoded == ZSTD_CONTENTSIZE_UNKNOWN) {
        fprintf(stderr, "minios: bad zstd frame for %s\n", outpath);
        exit(1);
    }
    void *buf = malloc((size_t)decoded);
    if (!buf) die("malloc");
    size_t const got = ZSTD_decompress(buf, (size_t)decoded, in, insize);
    if (ZSTD_isError(got)) {
        fprintf(stderr, "minios: zstd decompress failed for %s: %s\n",
                outpath, ZSTD_getErrorName(got));
        exit(1);
    }
    int mode = executable ? 0755 : 0644;
    int fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) die(outpath);
    size_t off = 0;
    while (off < got) {
        ssize_t n = write(fd, (char*)buf + off, got - off);
        if (n < 0) { if (errno == EINTR) continue; die("write"); }
        off += (size_t)n;
    }
    close(fd);
    free(buf);
    if (executable) chmod(outpath, 0755);
}

static void path_join(char *out, size_t outsz, const char *a, const char *b) {
    snprintf(out, outsz, "%s/%s", a, b);
}

/* mkdir -p (simple, non-recursive for 2 levels max) */
static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Resolve default data directory: $XDG_DATA_HOME/minios or ~/.local/share/minios */
static void resolve_datadir(char *out, size_t outsz) {
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s/minios", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) home = "/tmp";
        snprintf(out, outsz, "%s/.local/share/minios", home);
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [launcher-options] [-- rvvm-options...]\n"
        "\n"
        "Launcher options:\n"
        "  --ephemeral   Discard filesystem on exit (don't persist rootfs)\n"
        "  --reset       Delete saved state and start fresh\n"
        "  --datadir P   Use P as data directory (default: ~/.local/share/minios)\n"
        "  --help        Show this help\n"
        "\n"
        "RVVM options (passed through):\n"
        "  -portfwd tcp/127.0.0.1:2222=22   Port forwarding\n"
        "  -m 1G                             Memory (default 512M)\n"
        "  -smp 4                            CPU cores (default 2)\n"
        "\n"
        "Examples:\n"
        "  %s                                # Persistent mode (default)\n"
        "  %s --ephemeral                    # Temporary, clean every time\n"
        "  %s --reset                        # Wipe saved state, start fresh\n"
        "  %s -portfwd tcp/127.0.0.1:2222=22 # SSH access via port 2222\n"
        , prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    int ephemeral = 0;
    int reset = 0;
    char datadir[512] = {0};
    int rvvm_argc = 0;
    char *rvvm_args[64];

    /* Parse launcher-specific args; collect the rest for RVVM */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ephemeral") == 0) {
            ephemeral = 1;
        } else if (strcmp(argv[i], "--reset") == 0) {
            reset = 1;
        } else if (strcmp(argv[i], "--datadir") == 0 && i + 1 < argc) {
            snprintf(datadir, sizeof(datadir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            /* Everything else → RVVM */
            if (rvvm_argc < 62) rvvm_args[rvvm_argc++] = argv[i];
        }
    }

    /* Resolve data directory */
    if (!datadir[0]) resolve_datadir(datadir, sizeof(datadir));

    /* Handle --reset: rm -rf datadir */
    if (reset) {
        fprintf(stderr, "minios: resetting data directory: %s\n", datadir);
        nftw(datadir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    }

    /* Create workdir for ephemeral assets (rvvm, fw_jump, Image) */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_workdir, sizeof(g_workdir), "%s/minios.XXXXXX", tmp);
    if (!mkdtemp(g_workdir)) die("mkdtemp");

    atexit(cleanup_workdir);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    /* Extract read-only assets to workdir (always fresh) */
    char rvvm_path[512], fw_path[512], img_path[512];
    path_join(rvvm_path, sizeof(rvvm_path), g_workdir, "rvvm");
    path_join(fw_path,   sizeof(fw_path),   g_workdir, "fw_jump.bin");
    path_join(img_path,  sizeof(img_path),  g_workdir, "Image");

    extract_blob(rvvm_path, BLOB_PTR(rvvm_zst),        BLOB_SIZE(rvvm_zst),        1, 1);
    extract_blob(fw_path,   BLOB_PTR(fw_jump_bin_zst), BLOB_SIZE(fw_jump_bin_zst), 0, 1);
    extract_blob(img_path,  BLOB_PTR(Image_zst),       BLOB_SIZE(Image_zst),       0, 1);

    /* Rootfs: persistent (in datadir) or ephemeral (in workdir) */
    char rootfs_path[512];
    if (ephemeral) {
        path_join(rootfs_path, sizeof(rootfs_path), g_workdir, "rootfs.img");
        extract_blob(rootfs_path, BLOB_PTR(rootfs_img_zst), BLOB_SIZE(rootfs_img_zst), 0, 1);
    } else {
        mkdirs(datadir);
        path_join(rootfs_path, sizeof(rootfs_path), datadir, "rootfs.img");
        /* Only extract if not present (preserves user state) */
        extract_blob(rootfs_path, BLOB_PTR(rootfs_img_zst), BLOB_SIZE(rootfs_img_zst), 0, 0);
        fprintf(stderr, "minios: data directory: %s\n", datadir);
    }

    /* Build RVVM argv */
    char **rargv = calloc(rvvm_argc + 16, sizeof(char*));
    if (!rargv) die("calloc");
    int n = 0;
    rargv[n++] = rvvm_path;
    rargv[n++] = fw_path;
    rargv[n++] = (char*)"-k";        rargv[n++] = img_path;
    rargv[n++] = (char*)"-i";        rargv[n++] = rootfs_path;
    rargv[n++] = (char*)"-m";        rargv[n++] = (char*)"512M";
    rargv[n++] = (char*)"-smp";      rargv[n++] = (char*)"2";
    rargv[n++] = (char*)"-nogui";
    rargv[n++] = (char*)"-nosound";
    /* Append user RVVM args */
    for (int i = 0; i < rvvm_argc; i++) rargv[n++] = rvvm_args[i];
    rargv[n] = NULL;

    /* Fork & exec */
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        execv(rvvm_path, rargv);
        die("execv rvvm");
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        die("waitpid");
    }
    free(rargv);
    /* atexit → cleanup_workdir removes tmpdir (rvvm/fw/Image);
     * rootfs in datadir is intentionally NOT deleted. */
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
