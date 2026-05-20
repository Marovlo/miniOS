/*
 * miniOS launcher: self-contained single-file Linux system on RVVM.
 *
 * Embeds 4 zstd-compressed assets via objcopy --binary:
 *   - rvvm        (static RVVM emulator binary)
 *   - fw_jump.bin (OpenSBI M-mode firmware)
 *   - Image       (Linux kernel)
 *   - rootfs.img  (ext4 root filesystem)
 *
 * On startup:
 *   1. mkdtemp() a workdir under $TMPDIR (default /tmp)
 *   2. zstd-decompress all 4 assets into workdir
 *   3. chmod +x the rvvm binary
 *   4. fork; child execve(rvvm, [...]); parent waits, then rm -rf workdir
 *
 * Designed to be small and portable (POSIX). See companion launcher.c
 * code paths for non-Linux hosts (no major changes expected).
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

/* Symbols emitted by `ld -r -b binary`:
 *   _binary_<name>_start, _binary_<name>_end
 * The size symbol is also emitted but we compute it from start/end. */
#define DECL_BLOB(sym)                                            \
    extern const unsigned char _binary_##sym##_start[];           \
    extern const unsigned char _binary_##sym##_end[]

DECL_BLOB(rvvm_zst);
DECL_BLOB(fw_jump_bin_zst);
DECL_BLOB(Image_zst);
DECL_BLOB(rootfs_img_zst);

#define BLOB_PTR(sym)  (_binary_##sym##_start)
#define BLOB_SIZE(sym) ((size_t)(_binary_##sym##_end - _binary_##sym##_start))

static char g_workdir[256] = {0};

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
    /* Best-effort cleanup; child rvvm will get the same signal via process group. */
    cleanup_workdir();
    /* Re-raise default. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Decompress a zstd-framed blob to a file. */
static void extract_blob(const char *outpath,
                         const unsigned char *in, size_t insize,
                         int executable) {
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
    /* For the rvvm binary, ensure exec bit even if umask interfered. */
    if (executable) chmod(outpath, 0755);
}

static void path_join(char *out, size_t outsz, const char *a, const char *b) {
    snprintf(out, outsz, "%s/%s", a, b);
}

int main(int argc, char **argv) {
    /* 1. Create temp workdir */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_workdir, sizeof(g_workdir), "%s/minios.XXXXXX", tmp);
    if (!mkdtemp(g_workdir)) die("mkdtemp");

    /* Cleanup on normal exit + common signals */
    atexit(cleanup_workdir);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    /* 2. Extract assets */
    char rvvm_path[512], fw_path[512], img_path[512], rootfs_path[512];
    path_join(rvvm_path,   sizeof(rvvm_path),   g_workdir, "rvvm");
    path_join(fw_path,     sizeof(fw_path),     g_workdir, "fw_jump.bin");
    path_join(img_path,    sizeof(img_path),    g_workdir, "Image");
    path_join(rootfs_path, sizeof(rootfs_path), g_workdir, "rootfs.img");

    extract_blob(rvvm_path,   BLOB_PTR(rvvm_zst),         BLOB_SIZE(rvvm_zst),         1);
    extract_blob(fw_path,     BLOB_PTR(fw_jump_bin_zst),  BLOB_SIZE(fw_jump_bin_zst),  0);
    extract_blob(img_path,    BLOB_PTR(Image_zst),        BLOB_SIZE(Image_zst),        0);
    extract_blob(rootfs_path, BLOB_PTR(rootfs_img_zst),   BLOB_SIZE(rootfs_img_zst),   0);

    /* 3. Build argv: pass through user args after our defaults */
    char **rargv = calloc(argc + 16, sizeof(char*));
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
    /* Pass-through user args (e.g. -portfwd, -cmdline, ...) */
    for (int i = 1; i < argc; i++) rargv[n++] = argv[i];
    rargv[n] = NULL;

    /* 4. Fork & exec, so we can clean up afterwards */
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
    /* atexit handler will rm -rf workdir */
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
