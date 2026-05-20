/*
 * miniOS launcher: self-contained single-file Linux system on RVVM.
 *
 * Embeds 4 zstd-compressed assets via ld -r -b binary:
 *   - rvvm        (static RVVM emulator binary)
 *   - fw_jump.bin (OpenSBI M-mode firmware)
 *   - Image       (Linux kernel)
 *   - rootfs.img  (ext4 root filesystem)
 *
 * Operation modes:
 *   Default (daemon + PTY relay):
 *     First run:  extracts assets to data/, starts RVVM as background daemon
 *                 with PTY, connects client to Unix socket for terminal I/O.
 *     Subsequent: detects running instance via socket, attaches as client.
 *     Ctrl+A;D:   detach (VM keeps running in background).
 *     Ctrl+A;X:   passed to RVVM (shuts down VM).
 *
 *   --ephemeral:  old foreground mode, no daemon, no detach. Temp dir cleaned on exit.
 *   --stop:       kill running daemon instance.
 *   --reset:      stop instance + delete data dir + start fresh.
 *
 * Data directory (default: <exe-dir>/data/):
 *   rootfs.img, rvvm, fw_jump.bin, Image, minios.sock, minios.pid
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
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>
#include <ftw.h>
#include <termios.h>
#include <poll.h>

#ifdef __APPLE__
#include <util.h>           /* openpty() on macOS */
#include <mach-o/dyld.h>    /* _NSGetExecutablePath() */
#else
#include <pty.h>            /* openpty() on Linux */
#endif

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

/* ──────────────────────────────────────────────────────────────────────────── */
/* Utility helpers                                                              */
/* ──────────────────────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    fprintf(stderr, "minios: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static int rm_cb(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void path_join(char *out, size_t outsz, const char *a, const char *b) {
    snprintf(out, outsz, "%s/%s", a, b);
}

/* mkdir -p */
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

/* Resolve default data directory: <exe-dir>/data */
static void resolve_datadir(char *out, size_t outsz) {
    char exepath[512];
    int found = 0;

#ifdef __APPLE__
    uint32_t bufsize = sizeof(exepath);
    if (_NSGetExecutablePath(exepath, &bufsize) == 0) {
        /* Resolve symlinks */
        char resolved[512];
        if (realpath(exepath, resolved))
            snprintf(exepath, sizeof(exepath), "%s", resolved);
        found = 1;
    }
#else
    ssize_t n = readlink("/proc/self/exe", exepath, sizeof(exepath) - 1);
    if (n > 0) {
        exepath[n] = '\0';
        found = 1;
    }
#endif

    if (found) {
        char *slash = strrchr(exepath, '/');
        if (slash) *slash = '\0';
        snprintf(out, outsz, "%s/data", exepath);
    } else {
        if (!getcwd(out, outsz - 5)) snprintf(out, outsz, ".");
        strcat(out, "/data");
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Sparse file writing                                                          */
/* ──────────────────────────────────────────────────────────────────────────── */

static int is_zero(const unsigned char *p, size_t len) {
    const unsigned long *wp = (const unsigned long *)p;
    size_t nwords = len / sizeof(unsigned long);
    for (size_t i = 0; i < nwords; i++)
        if (wp[i]) return 0;
    for (size_t i = nwords * sizeof(unsigned long); i < len; i++)
        if (p[i]) return 0;
    return 1;
}

static void write_sparse(int fd, const unsigned char *buf, size_t len) {
    const size_t PAGE = 4096;
    size_t off = 0;
    while (off < len) {
        size_t blk = (len - off < PAGE) ? (len - off) : PAGE;
        if (is_zero(buf + off, blk)) {
            if (lseek(fd, (off_t)blk, SEEK_CUR) < 0) die("lseek");
        } else {
            size_t w = 0;
            while (w < blk) {
                ssize_t n = write(fd, buf + off + w, blk - w);
                if (n < 0) { if (errno == EINTR) continue; die("write"); }
                w += (size_t)n;
            }
        }
        off += blk;
    }
    if (ftruncate(fd, (off_t)len) < 0) die("ftruncate");
}

/* Decompress zstd blob to file. Sparse writes for zero pages. */
static void extract_blob(const char *outpath,
                         const unsigned char *in, size_t insize,
                         int executable, int overwrite) {
    if (!overwrite) {
        struct stat st;
        if (stat(outpath, &st) == 0 && st.st_size > 0)
            return;
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
    write_sparse(fd, (const unsigned char *)buf, got);
    close(fd);
    free(buf);
    if (executable) chmod(outpath, 0755);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* PID file helpers                                                             */
/* ──────────────────────────────────────────────────────────────────────────── */

static void write_pidfile(const char *path, pid_t pid) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    (void)!write(fd, buf, len);
    close(fd);
}

static pid_t read_pidfile(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[32] = {0};
    (void)!read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return (pid_t)atoi(buf);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Socket helpers                                                               */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Try to connect to an existing server. Returns fd or -1. */
static int try_connect(const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Create a listening Unix socket. Returns listen fd. */
static int create_listen_socket(const char *sock_path) {
    unlink(sock_path); /* remove stale socket */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(fd, 2) < 0) die("listen");
    return fd;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Terminal helpers                                                              */
/* ──────────────────────────────────────────────────────────────────────────── */

static struct termios g_orig_termios;
static int g_terminal_raw = 0;

static void restore_terminal(void) {
    if (g_terminal_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_terminal_raw = 0;
    }
}

static void set_raw_terminal(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_terminal_raw = 1;
    atexit(restore_terminal);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Server (daemon): manages PTY ↔ Unix socket relay                            */
/* ──────────────────────────────────────────────────────────────────────────── */

static void server_main(const char *datadir, const char *rvvm_path, char **rvvm_argv) {
    char sock_path[512], pid_path[512];
    path_join(sock_path, sizeof(sock_path), datadir, "minios.sock");
    path_join(pid_path,  sizeof(pid_path),  datadir, "minios.pid");

    /* Create PTY pair */
    int pty_master, pty_slave;
    if (openpty(&pty_master, &pty_slave, NULL, NULL, NULL) < 0)
        die("openpty");

    /* Fork RVVM on PTY slave */
    pid_t rvvm_pid = fork();
    if (rvvm_pid < 0) die("fork rvvm");
    if (rvvm_pid == 0) {
        /* Child: RVVM process */
        close(pty_master);
        setsid();
        dup2(pty_slave, STDIN_FILENO);
        dup2(pty_slave, STDOUT_FILENO);
        dup2(pty_slave, STDERR_FILENO);
        if (pty_slave > 2) close(pty_slave);
        execv(rvvm_path, rvvm_argv);
        _exit(127);
    }
    close(pty_slave);

    /* Set PTY master non-blocking */
    fcntl(pty_master, F_SETFL, fcntl(pty_master, F_GETFL) | O_NONBLOCK);

    /* Install SIGTERM handler: kill RVVM child before exiting */
    signal(SIGTERM, SIG_IGN); /* Temporarily ignore while we set up */

    /* Create listen socket */
    int listen_fd = create_listen_socket(sock_path);

    /* Write PID file (our daemon PID) */
    write_pidfile(pid_path, getpid());

    /* Now install proper SIGTERM handler */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = SIG_DFL; /* Will terminate us, but first we kill RVVM via atexit-like */
    sigaction(SIGTERM, &sa_term, NULL);

    /* Use atexit-like behavior: on SIGTERM, kill RVVM before we die.
     * Simplest: just kill RVVM in the stop_instance sender side.
     * But also protect with a signal handler that kills the child: */
    {
        /* We rely on the stop_instance code sending SIGTERM to RVVM's pid too.
         * Store RVVM pid alongside daemon pid in the pidfile. */
        char pid_content[64];
        snprintf(pid_content, sizeof(pid_content), "%d\n%d\n", (int)getpid(), (int)rvvm_pid);
        FILE *pf = fopen(pid_path, "w");
        if (pf) { fputs(pid_content, pf); fclose(pf); }
    }

    /* Replay buffer: stores last N bytes of PTY output.
     * When a new client connects, we replay this so they see the current
     * screen state (including shell prompt) immediately. */
    #define REPLAY_SIZE 4096
    char replay_buf[REPLAY_SIZE];
    size_t replay_len = 0;   /* how much is filled (up to REPLAY_SIZE) */
    size_t replay_pos = 0;   /* write position (circular) */
    int replay_wrapped = 0;  /* whether buffer has wrapped around */

    /* Server event loop */
    int client_fd = -1;
    char buf[4096];
    int running = 1;

    while (running) {
        struct pollfd fds[3];
        int nfds = 0;

        fds[nfds].fd = pty_master;
        fds[nfds].events = POLLIN;
        int idx_pty = nfds++;

        fds[nfds].fd = listen_fd;
        fds[nfds].events = POLLIN;
        int idx_listen = nfds++;

        int idx_client = -1;
        if (client_fd >= 0) {
            fds[nfds].fd = client_fd;
            fds[nfds].events = POLLIN;
            idx_client = nfds++;
        }

        int ret = poll(fds, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* PTY master readable → data from RVVM */
        if (fds[idx_pty].revents & POLLIN) {
            ssize_t n = read(pty_master, buf, sizeof(buf));
            if (n > 0) {
                /* Append to replay buffer (circular) */
                for (ssize_t i = 0; i < n; i++) {
                    replay_buf[replay_pos] = buf[i];
                    replay_pos = (replay_pos + 1) % REPLAY_SIZE;
                }
                if ((size_t)n + replay_len > REPLAY_SIZE) {
                    replay_wrapped = 1;
                    replay_len = REPLAY_SIZE;
                } else if (!replay_wrapped) {
                    replay_len += n;
                }

                /* Forward to client if connected */
                if (client_fd >= 0) {
                    (void)!write(client_fd, buf, n);
                }
            }
        }
        if (fds[idx_pty].revents & (POLLHUP | POLLERR)) {
            /* RVVM exited */
            running = 0;
            break;
        }

        /* Listen socket → new client connection */
        if (fds[idx_listen].revents & POLLIN) {
            int new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                if (client_fd >= 0) close(client_fd); /* replace old client */
                client_fd = new_fd;

                /* Replay buffered output to new client so they see
                 * the current screen (including shell prompt). */
                if (replay_wrapped) {
                    /* Buffer is full and wrapped: send from replay_pos to end, then 0 to replay_pos */
                    (void)!write(client_fd, replay_buf + replay_pos, REPLAY_SIZE - replay_pos);
                    (void)!write(client_fd, replay_buf, replay_pos);
                } else if (replay_len > 0) {
                    /* Buffer hasn't wrapped yet: send 0..replay_len */
                    (void)!write(client_fd, replay_buf, replay_len);
                }
            }
        }

        /* Client readable → data from user terminal */
        if (idx_client >= 0 && fds[idx_client].revents & POLLIN) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n > 0) {
                (void)!write(pty_master, buf, n);  /* send to RVVM */
            } else {
                /* Client disconnected (detach) */
                close(client_fd);
                client_fd = -1;
            }
        }
        if (idx_client >= 0 && fds[idx_client].revents & (POLLHUP | POLLERR)) {
            close(client_fd);
            client_fd = -1;
        }

        /* Check if RVVM is still alive */
        int wstatus;
        pid_t w = waitpid(rvvm_pid, &wstatus, WNOHANG);
        if (w > 0) {
            running = 0;
        }
    }

    /* Cleanup */
    if (client_fd >= 0) close(client_fd);
    close(listen_fd);
    close(pty_master);
    unlink(sock_path);
    unlink(pid_path);

    /* Reap RVVM if not already */
    waitpid(rvvm_pid, NULL, 0);
    _exit(0);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Client: connects to server socket, relays terminal ↔ socket                 */
/* ──────────────────────────────────────────────────────────────────────────── */

static int client_main(int sock_fd) {
    set_raw_terminal();
    fprintf(stderr,
        "\r\n\033[1mminios:\033[0m attached.\r\n"
        "  Ctrl+A;D detach | Ctrl+A;X shutdown | Ctrl+A;? help\r\n\r\n");

    int escape_state = 0; /* 0=normal, 1=saw Ctrl+A */
    int detached = 0;
    int server_died = 0;
    char buf[4096];

    while (1) {
        struct pollfd fds[2];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = sock_fd;
        fds[1].events = POLLIN;

        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* stdin → socket (with escape detection) */
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break; /* stdin closed (terminal gone) */

            for (ssize_t i = 0; i < n; i++) {
                unsigned char ch = (unsigned char)buf[i];
                if (escape_state) {
                    escape_state = 0;
                    if (ch == 'd' || ch == 'D') {
                        /* Detach! */
                        detached = 1;
                        goto done;
                    } else if (ch == 'x' || ch == 'X') {
                        /* Pass Ctrl+A;X to RVVM (shutdown) */
                        char seq[2] = {0x01, (char)ch};
                        (void)!write(sock_fd, seq, 2);
                    } else if (ch == '?') {
                        /* Show help */
                        const char *help =
                            "\r\n\033[1m--- miniOS escape menu ---\033[0m\r\n"
                            "  Ctrl+A; D  - detach (VM keeps running)\r\n"
                            "  Ctrl+A; X  - shutdown VM\r\n"
                            "  Ctrl+A; ?  - this help\r\n"
                            "  Ctrl+A; A  - send literal Ctrl+A\r\n"
                            "\033[1m--------------------------\033[0m\r\n";
                        (void)!write(STDOUT_FILENO, help, strlen(help));
                    } else if (ch == 'a' || ch == 'A') {
                        /* Send literal Ctrl+A */
                        char ctrl_a = 0x01;
                        (void)!write(sock_fd, &ctrl_a, 1);
                    } else {
                        /* Unknown: send both Ctrl+A + char */
                        char ctrl_a = 0x01;
                        (void)!write(sock_fd, &ctrl_a, 1);
                        (void)!write(sock_fd, &ch, 1);
                    }
                } else if (ch == 0x01) { /* Ctrl+A */
                    escape_state = 1;
                    /* Show escape prompt */
                    const char *prompt = "\r\n\033[33m[Ctrl+A]\033[0m D=detach X=shutdown ?=help: ";
                    (void)!write(STDOUT_FILENO, prompt, strlen(prompt));
                } else {
                    (void)!write(sock_fd, &ch, 1);
                }
            }
        }

        /* socket → stdout */
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(sock_fd, buf, sizeof(buf));
            if (n <= 0) { server_died = 1; break; }
            (void)!write(STDOUT_FILENO, buf, n);
        }
        if (fds[1].revents & (POLLHUP | POLLERR)) {
            server_died = 1;
            break;
        }
    }

done:
    restore_terminal();
    close(sock_fd);

    if (detached) {
        fprintf(stderr, "\r\nminios: detached. Run again to reattach.\r\n");
        return 0;
    } else if (server_died) {
        fprintf(stderr, "\r\nminios: VM exited.\r\n");
        return 0;
    }
    /* stdin closed — just exit quietly */
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Instance management                                                          */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Stop a running instance. Returns 0 if stopped, -1 if none running. */
static int stop_instance(const char *datadir) {
    char pid_path[512], sock_path[512];
    path_join(pid_path,  sizeof(pid_path),  datadir, "minios.pid");
    path_join(sock_path, sizeof(sock_path), datadir, "minios.sock");

    /* Read pidfile (may contain two lines: daemon_pid and rvvm_pid) */
    FILE *pf = fopen(pid_path, "r");
    if (!pf) {
        unlink(sock_path);
        return -1;
    }
    pid_t daemon_pid = 0, rvvm_pid = 0;
    char line[64];
    if (fgets(line, sizeof(line), pf)) daemon_pid = atoi(line);
    if (fgets(line, sizeof(line), pf)) rvvm_pid = atoi(line);
    fclose(pf);

    if (daemon_pid <= 0) {
        unlink(sock_path);
        unlink(pid_path);
        return -1;
    }

    /* Check if daemon is alive */
    if (kill(daemon_pid, 0) < 0 && (rvvm_pid <= 0 || kill(rvvm_pid, 0) < 0)) {
        /* Both dead, clean up stale files */
        unlink(sock_path);
        unlink(pid_path);
        return -1;
    }

    /* Kill both daemon and RVVM */
    if (rvvm_pid > 0) kill(rvvm_pid, SIGTERM);
    kill(daemon_pid, SIGTERM);
    for (int i = 0; i < 30; i++) { /* wait up to 3 seconds */
        usleep(100000);
        int daemon_dead = (kill(daemon_pid, 0) < 0);
        int rvvm_dead = (rvvm_pid <= 0 || kill(rvvm_pid, 0) < 0);
        if (daemon_dead && rvvm_dead) break;
    }
    /* Force kill if still alive */
    if (kill(daemon_pid, 0) == 0) kill(daemon_pid, SIGKILL);
    if (rvvm_pid > 0 && kill(rvvm_pid, 0) == 0) kill(rvvm_pid, SIGKILL);
    usleep(200000);

    unlink(sock_path);
    unlink(pid_path);
    fprintf(stderr, "minios: stopped (pid %d).\n", (int)daemon_pid);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Ephemeral (legacy) foreground mode                                           */
/* ──────────────────────────────────────────────────────────────────────────── */

static char g_workdir[512] = {0};

static void cleanup_workdir(void) {
    if (g_workdir[0]) {
        nftw(g_workdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        g_workdir[0] = '\0';
    }
}

static void on_signal_ephemeral(int sig) {
    cleanup_workdir();
    signal(sig, SIG_DFL);
    raise(sig);
}

static int run_ephemeral(int rvvm_argc, char **rvvm_args) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_workdir, sizeof(g_workdir), "%s/minios.XXXXXX", tmp);
    if (!mkdtemp(g_workdir)) die("mkdtemp");

    atexit(cleanup_workdir);
    struct sigaction sa = { .sa_handler = on_signal_ephemeral };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    char rvvm_path[512], fw_path[512], img_path[512], rootfs_path[512];
    path_join(rvvm_path,   sizeof(rvvm_path),   g_workdir, "rvvm");
    path_join(fw_path,     sizeof(fw_path),     g_workdir, "fw_jump.bin");
    path_join(img_path,    sizeof(img_path),    g_workdir, "Image");
    path_join(rootfs_path, sizeof(rootfs_path), g_workdir, "rootfs.img");

    extract_blob(rvvm_path,   BLOB_PTR(rvvm_zst),         BLOB_SIZE(rvvm_zst),         1, 1);
    extract_blob(fw_path,     BLOB_PTR(fw_jump_bin_zst),  BLOB_SIZE(fw_jump_bin_zst),  0, 1);
    extract_blob(img_path,    BLOB_PTR(Image_zst),        BLOB_SIZE(Image_zst),        0, 1);
    extract_blob(rootfs_path, BLOB_PTR(rootfs_img_zst),   BLOB_SIZE(rootfs_img_zst),   0, 1);

    char **rargv = calloc(rvvm_argc + 16, sizeof(char*));
    if (!rargv) die("calloc");
    int n = 0;
    rargv[n++] = rvvm_path;
    rargv[n++] = fw_path;
    rargv[n++] = (char*)"-k";    rargv[n++] = img_path;
    rargv[n++] = (char*)"-i";    rargv[n++] = rootfs_path;
    rargv[n++] = (char*)"-m";    rargv[n++] = (char*)"512M";
    rargv[n++] = (char*)"-smp";  rargv[n++] = (char*)"2";
    rargv[n++] = (char*)"-nogui";
    rargv[n++] = (char*)"-nosound";
    for (int i = 0; i < rvvm_argc; i++) rargv[n++] = rvvm_args[i];
    rargv[n] = NULL;

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
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Help                                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] [-- rvvm-options...]\n"
        "\n"
        "Options:\n"
        "  --stop        Stop running instance\n"
        "  --reset       Stop + delete all data, start fresh\n"
        "  --ephemeral   One-shot mode (no persist, no detach)\n"
        "  --datadir P   Custom data directory (default: <exe-dir>/data)\n"
        "  --help        Show this help\n"
        "\n"
        "Controls (while attached):\n"
        "  Ctrl+A; D     Detach (VM keeps running in background)\n"
        "  Ctrl+A; X     Shutdown VM (passed to RVVM)\n"
        "\n"
        "RVVM options (passed through):\n"
        "  -portfwd tcp/127.0.0.1:2222=22   Port forwarding for SSH\n"
        "  -m 1G                             Memory (default 512M)\n"
        "  -smp 4                            CPU cores (default 2)\n"
        "\n"
        "Examples:\n"
        "  %s                       # Start or reattach\n"
        "  %s --stop                # Stop background VM\n"
        "  %s --reset               # Factory reset\n"
        "  %s --ephemeral           # Throwaway session (no detach)\n"
        , prog, prog, prog, prog, prog);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Main                                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int ephemeral = 0;
    int reset = 0;
    int stop = 0;
    char datadir[512] = {0};
    int rvvm_argc = 0;
    char *rvvm_args[64];

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ephemeral") == 0) {
            ephemeral = 1;
        } else if (strcmp(argv[i], "--reset") == 0) {
            reset = 1;
        } else if (strcmp(argv[i], "--stop") == 0) {
            stop = 1;
        } else if (strcmp(argv[i], "--datadir") == 0 && i + 1 < argc) {
            snprintf(datadir, sizeof(datadir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            if (rvvm_argc < 62) rvvm_args[rvvm_argc++] = argv[i];
        }
    }

    /* Resolve data directory */
    if (!datadir[0]) resolve_datadir(datadir, sizeof(datadir));

    /* --stop: kill running instance and exit */
    if (stop) {
        if (stop_instance(datadir) < 0)
            fprintf(stderr, "minios: no running instance found.\n");
        return 0;
    }

    /* --reset: stop + wipe data dir */
    if (reset) {
        stop_instance(datadir);
        fprintf(stderr, "minios: resetting data directory: %s\n", datadir);
        nftw(datadir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    }

    /* --ephemeral: old foreground mode, no daemon */
    if (ephemeral) {
        return run_ephemeral(rvvm_argc, rvvm_args);
    }

    /* ── Daemon mode (default) ── */

    /* Try to connect to existing instance */
    char sock_path[512];
    path_join(sock_path, sizeof(sock_path), datadir, "minios.sock");

    int conn = try_connect(sock_path);
    if (conn >= 0) {
        fprintf(stderr, "minios: connecting to running instance...\n");
        return client_main(conn);
    }

    /* No running instance → extract assets and start server */
    mkdirs(datadir);
    fprintf(stderr, "minios: data directory: %s\n", datadir);

    char rvvm_path[512], fw_path[512], img_path[512], rootfs_path[512];
    path_join(rvvm_path,   sizeof(rvvm_path),   datadir, "rvvm");
    path_join(fw_path,     sizeof(fw_path),     datadir, "fw_jump.bin");
    path_join(img_path,    sizeof(img_path),    datadir, "Image");
    path_join(rootfs_path, sizeof(rootfs_path), datadir, "rootfs.img");

    fprintf(stderr, "minios: extracting assets...\n");
    extract_blob(rvvm_path,   BLOB_PTR(rvvm_zst),        BLOB_SIZE(rvvm_zst),        1, 1);
    extract_blob(fw_path,     BLOB_PTR(fw_jump_bin_zst), BLOB_SIZE(fw_jump_bin_zst), 0, 1);
    extract_blob(img_path,    BLOB_PTR(Image_zst),       BLOB_SIZE(Image_zst),       0, 1);
    extract_blob(rootfs_path, BLOB_PTR(rootfs_img_zst),  BLOB_SIZE(rootfs_img_zst),  0, 0);

    /* Build RVVM argv */
    char **rargv = calloc(rvvm_argc + 16, sizeof(char*));
    if (!rargv) die("calloc");
    int n = 0;
    rargv[n++] = rvvm_path;
    rargv[n++] = fw_path;
    rargv[n++] = (char*)"-k";    rargv[n++] = img_path;
    rargv[n++] = (char*)"-i";    rargv[n++] = rootfs_path;
    rargv[n++] = (char*)"-m";    rargv[n++] = (char*)"512M";
    rargv[n++] = (char*)"-smp";  rargv[n++] = (char*)"2";
    rargv[n++] = (char*)"-nogui";
    rargv[n++] = (char*)"-nosound";
    for (int i = 0; i < rvvm_argc; i++) rargv[n++] = rvvm_args[i];
    rargv[n] = NULL;

    /* Fork server daemon */
    fprintf(stderr, "minios: starting VM...\n");
    pid_t daemon_pid = fork();
    if (daemon_pid < 0) die("fork daemon");
    if (daemon_pid == 0) {
        /* Child becomes daemon server */
        setsid(); /* detach from terminal */
        /* Close stdin/stdout/stderr for daemon (will use PTY) */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        server_main(datadir, rvvm_path, rargv);
        _exit(0); /* never reached */
    }
    free(rargv);

    /* Parent: wait for socket to become available, then connect */
    for (int i = 0; i < 50; i++) { /* up to 5 seconds */
        usleep(100000);
        conn = try_connect(sock_path);
        if (conn >= 0) break;
        /* Check if daemon is still alive */
        if (waitpid(daemon_pid, NULL, WNOHANG) > 0) {
            fprintf(stderr, "minios: server failed to start.\n");
            return 1;
        }
    }
    if (conn < 0) {
        fprintf(stderr, "minios: timeout connecting to server.\n");
        return 1;
    }

    return client_main(conn);
}
