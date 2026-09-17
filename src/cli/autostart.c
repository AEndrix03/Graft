/* graft CLI — daemon auto-start.
 *
 * If a connect attempt fails (daemon not running), spawn graftd next
 * to the CLI binary as a detached process and poll the socket until it's
 * ready, with a hard deadline. The first command of a session pays this
 * cost (~1-2s on a warm OS, more on first model load); later commands hit
 * the already-running daemon.
 *
 * Resolution order:
 *   - daemon binary: <dir-of-cli>/graftd[.exe]
 *   - config file:   $GRAFT_CONFIG, then <cwd>/config.yaml,
 *                    then <cwd>/config.example.yaml,
 *                    then <dir-of-cli>/../config.example.yaml
 *
 * Cross-platform spawn:
 *   - Windows: CreateProcessA with DETACHED_PROCESS and a PATH that
 *     includes third_party/llama.cpp/build/bin so the DLLs resolve.
 *   - POSIX:   double fork + setsid + execvp; child redirects std{in,out,err}
 *     to a log file under the CLI's binary directory.
 *
 * Polling: connect every 200ms, up to MG_AUTOSTART_TIMEOUT_MS.
 */

#include "autostart.h"

#include "graft/error.h"
#include "../daemon/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define MG_PATH_SEP    '\\'
#  define MG_PATH_LIST_SEP ';'
#  define MG_EXE_SUFFIX  ".exe"
typedef HANDLE mg_proc_t;
#  define MG_PROC_NONE NULL
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <sys/file.h>
#  include <fcntl.h>
#  include <time.h>
#  include <errno.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
#  define MG_PATH_SEP    '/'
#  define MG_PATH_LIST_SEP ':'
#  define MG_EXE_SUFFIX  ""
typedef int mg_proc_t;
#  define MG_PROC_NONE 0
#endif

#define MG_AUTOSTART_TIMEOUT_MS 20000
#define MG_AUTOSTART_POLL_MS    200

/* ----- own-exe-dir ----- */

static int mg_own_exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return -1;
#elif defined(__APPLE__)
    char buf[4096];
    char raw[4096];
    uint32_t raw_len = (uint32_t)sizeof(raw);
    if (_NSGetExecutablePath(raw, &raw_len) != 0) return -1;
    char resolved[4096];
    const char *path = realpath(raw, resolved);
    const char *src = path ? path : raw;
    if (snprintf(buf, sizeof(buf), "%s", src) >= (int)sizeof(buf)) return -1;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
#endif
    /* strip last component */
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] != MG_PATH_SEP) len--;
    if (len == 0) return -1;
    if (len >= cap) return -1;
    memcpy(out, buf, len);
    out[len - 1] = '\0'; /* drop trailing separator */
    return 0;
}

static int mg_path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s%c%s", a, MG_PATH_SEP, b);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int mg_file_exists(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
#else
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
#endif
}

/* ----- config resolution ----- */

static int mg_resolve_config(const char *cli_dir, char *out, size_t cap) {
    const char *env = getenv("GRAFT_CONFIG");
    if (env && *env && mg_file_exists(env)) {
        if (snprintf(out, cap, "%s", env) >= (int)cap) return -1;
        return 0;
    }
    /* When the user runs `graft` from anywhere on the PATH after a
     * regular install, the config lives at $GRAFT_HOME/config.yaml. */
    const char *mh = getenv("GRAFT_HOME");
    char buf[1024];
    if (mh && *mh) {
        if (snprintf(buf, sizeof(buf), "%s%cconfig.yaml", mh, MG_PATH_SEP)
            < (int)sizeof(buf) && mg_file_exists(buf)) {
            if (snprintf(out, cap, "%s", buf) >= (int)cap) return -1;
            return 0;
        }
    } else {
        /* Same default that profile.c uses for GRAFT_HOME. */
#ifdef _WIN32
        const char *base = getenv("USERPROFILE");
        if (!base || !*base) base = getenv("LOCALAPPDATA");
#else
        const char *base = getenv("HOME");
#endif
        if (base && *base) {
            if (snprintf(buf, sizeof(buf),
#ifdef _WIN32
                         "%s\\.graft\\config.yaml",
#else
                         "%s/.graft/config.yaml",
#endif
                         base) < (int)sizeof(buf) && mg_file_exists(buf)) {
                if (snprintf(out, cap, "%s", buf) >= (int)cap) return -1;
                return 0;
            }
        }
    }
    /* Dev fallbacks: cwd, then alongside the binary. */
    const char *candidates[] = {
        "config.yaml",
        "config.example.yaml",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
        if (mg_file_exists(candidates[i])) {
            if (snprintf(out, cap, "%s", candidates[i]) >= (int)cap) return -1;
            return 0;
        }
    }
    if (snprintf(buf, sizeof(buf), "%s%c..%cconfig.example.yaml",
                 cli_dir, MG_PATH_SEP, MG_PATH_SEP) >= (int)sizeof(buf))
        return -1;
    if (mg_file_exists(buf)) {
        if (snprintf(out, cap, "%s", buf) >= (int)cap) return -1;
        return 0;
    }
    return -1;
}

/* ----- daemon log -----
 *
 * The daemon prints the real reason it refused to start ("embed init failed",
 * "storage open failed", "socket listen failed") on stderr. Those lines are the
 * whole diagnosis, so they must land in a file the user can be pointed at:
 * $GRAFT_HOME/graftd.log, which is what the docs promise, falling back to the
 * binary's own directory for dev trees. */

static int mg_log_path(const char *cli_dir, char *out, size_t cap) {
    const char *mh = getenv("GRAFT_HOME");
    if (mh && *mh) {
        return snprintf(out, cap, "%s%cgraftd.log", mh, MG_PATH_SEP) < (int)cap ? 0 : -1;
    }
#ifdef _WIN32
    const char *base = getenv("USERPROFILE");
    if (!base || !*base) base = getenv("LOCALAPPDATA");
#else
    const char *base = getenv("HOME");
#endif
    if (base && *base) {
        return snprintf(out, cap,
#ifdef _WIN32
                        "%s\\.graft\\graftd.log",
#else
                        "%s/.graft/graftd.log",
#endif
                        base) < (int)cap ? 0 : -1;
    }
    return snprintf(out, cap, "%s%cgraftd.log", cli_dir, MG_PATH_SEP) < (int)cap ? 0 : -1;
}

/* Last few lines of the daemon log, for appending to an error message. */
static void mg_log_tail(const char *log_path, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    FILE *f = fopen(log_path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long end = ftell(f);
    if (end <= 0) { fclose(f); return; }
    long want = (long)cap - 1;
    if (want > 800) want = 800;
    long start = end > want ? end - want : 0;
    if (fseek(f, start, SEEK_SET) != 0) { fclose(f); return; }
    size_t n = fread(out, 1, (size_t)(end - start), f);
    fclose(f);
    out[n] = '\0';
    /* Drop a partial first line when we cut into the middle of one. */
    if (start > 0) {
        char *nl = strchr(out, '\n');
        if (nl && *(nl + 1)) memmove(out, nl + 1, strlen(nl + 1) + 1);
    }
    while (n > 0 && (out[strlen(out) - 1] == '\n' || out[strlen(out) - 1] == '\r')) {
        out[strlen(out) - 1] = '\0';
    }
}

/* ----- spawn ----- */

#ifdef _WIN32
static int mg_spawn_daemon(const char *daemon_path,
                           const char *cli_dir,
                           const char *config_path,
                           const char *log_path,
                           mg_proc_t *out_proc,
                           char *err, size_t err_cap) {
    /* Augment PATH with third_party/llama.cpp/build/bin (relative to cli_dir's
     * parent). The exe layout is <repo>/build/graft.exe, so llama.cpp dlls
     * live at <repo>/third_party/llama.cpp/build/bin. */
    char llama_bin[1024];
    snprintf(llama_bin, sizeof(llama_bin),
             "%s%c..%cthird_party%cllama.cpp%cbuild%cbin",
             cli_dir, MG_PATH_SEP, MG_PATH_SEP, MG_PATH_SEP,
             MG_PATH_SEP, MG_PATH_SEP);

    const char *old_path = getenv("PATH");
    char new_path[8192];
    snprintf(new_path, sizeof(new_path), "PATH=%s%c%s",
             llama_bin, MG_PATH_LIST_SEP, old_path ? old_path : "");

    /* Build env block: copy current env, replacing PATH. */
    LPCH env_block = GetEnvironmentStringsA();
    char  envbuf[32768];
    size_t epos = 0;
    if (env_block) {
        for (LPCH p = env_block; *p; ) {
            size_t l = strlen(p);
            if (_strnicmp(p, "PATH=", 5) != 0 && epos + l + 1 < sizeof(envbuf)) {
                memcpy(envbuf + epos, p, l + 1);
                epos += l + 1;
            }
            p += l + 1;
        }
        FreeEnvironmentStringsA(env_block);
    }
    size_t np_len = strlen(new_path);
    if (epos + np_len + 2 < sizeof(envbuf)) {
        memcpy(envbuf + epos, new_path, np_len + 1);
        epos += np_len + 1;
        envbuf[epos++] = '\0'; /* double-null terminator */
    }

    char cmdline[2048];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" --config \"%s\"",
             daemon_path, config_path);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = { 0 };

    /* Hand the detached daemon an inheritable handle to the log file, so its
     * stderr is preserved instead of being dropped on the floor. Without this
     * a failed start on Windows leaves the user with a bare "socket did not
     * become ready" and nothing to read. */
    SECURITY_ATTRIBUTES sa = { 0 };
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hlog = CreateFileA(log_path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL inherit = FALSE;
    if (hlog != INVALID_HANDLE_VALUE) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = NULL;
        si.hStdOutput = hlog;
        si.hStdError  = hlog;
        inherit = TRUE;
    }

    BOOL ok = CreateProcessA(
        daemon_path,
        cmdline,
        NULL, NULL, inherit,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        envbuf, cli_dir,
        &si, &pi);
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    if (!ok) {
        DWORD e = GetLastError();
        snprintf(err, err_cap, "CreateProcess failed (err=%lu)", (unsigned long)e);
        return -1;
    }
    CloseHandle(pi.hThread);
    /* Handed to the caller so it can notice a daemon that died on startup
     * instead of waiting out the full timeout. */
    *out_proc = pi.hProcess;
    return 0;
}

/* Non-zero once the spawned daemon has exited. */
static int mg_daemon_exited(mg_proc_t proc) {
    if (proc == NULL) return 0;
    return WaitForSingleObject(proc, 0) == WAIT_OBJECT_0 ? 1 : 0;
}

static void mg_proc_release(mg_proc_t proc) {
    if (proc) CloseHandle(proc);
}

static void mg_sleep_ms(int ms) { Sleep((DWORD)ms); }

#else /* POSIX */

static int mg_spawn_daemon(const char *daemon_path,
                           const char *cli_dir,
                           const char *config_path,
                           const char *log_path,
                           char *err, size_t err_cap) {
    (void)cli_dir;
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_cap, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* first child */
        if (setsid() < 0) _exit(127);
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0);
        /* grandchild: redirect std fds to the log file */
        int devnull = open("/dev/null", O_RDONLY);
        /* Daemon log may contain query text, error messages with paths,
         * and other content we don't want other local users to read. */
        int log_fd  = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0600);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        if (log_fd  >= 0) {
            dup2(log_fd, 1);
            dup2(log_fd, 2);
            close(log_fd);
        }
        char *const argv[] = {
            (char *)daemon_path,
            "--config", (char *)config_path,
            NULL
        };
        execvp(daemon_path, argv);
        _exit(127);
    }
    /* parent: reap first child immediately so it doesn't become a zombie */
    int st = 0;
    (void)waitpid(pid, &st, 0);
    return 0;
}

/* No pid to watch after the double fork: the poll timeout is the only signal. */
static int  mg_daemon_exited(mg_proc_t proc) { (void)proc; return 0; }
static void mg_proc_release(mg_proc_t proc)  { (void)proc; }

static void mg_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif

/* ----- spawn lock -----
 *
 * Without this guard two CLI invocations racing on a cold profile would
 * each pass the "socket not connectable" check and each spawn a daemon.
 * The second daemon's bind() then fails and the user sees a confusing
 * error even though the first daemon is healthy.
 *
 * The lock file lives next to the socket: <socket_path>.lock. We take an
 * exclusive non-blocking lock; if someone else holds it, that means another
 * spawn is in progress and we just wait on the socket. The OS releases the
 * lock when our fd/handle is closed, so we hold it for the duration of the
 * spawn-and-poll dance.
 */
#ifdef _WIN32
typedef HANDLE mg_lock_t;
#define MG_LOCK_INVALID INVALID_HANDLE_VALUE
#else
typedef int mg_lock_t;
#define MG_LOCK_INVALID (-1)
#endif

static int mg_lock_path(const char *socket_path, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s.lock", socket_path);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Try to take an exclusive lock. Returns:
 *   1  — lock acquired (caller must release via mg_unlock)
 *   0  — lock held by another process (try-fail)
 *  -1  — error opening/creating the lock file
 */
static int mg_try_lock(const char *lock_path, mg_lock_t *out) {
#ifdef _WIN32
    HANDLE h = CreateFileA(lock_path,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL,
                           OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = { 0 };
    if (!LockFileEx(h,
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, MAXDWORD, MAXDWORD, &ov)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) return 0;
        return -1;
    }
    *out = h;
    return 1;
#else
    int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        close(fd);
        if (saved == EWOULDBLOCK || saved == EAGAIN) return 0;
        return -1;
    }
    *out = fd;
    return 1;
#endif
}

static void mg_unlock(mg_lock_t lock) {
#ifdef _WIN32
    if (lock != MG_LOCK_INVALID) {
        OVERLAPPED ov = { 0 };
        UnlockFileEx(lock, 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle(lock);
    }
#else
    if (lock >= 0) {
        flock(lock, LOCK_UN);
        close(lock);
    }
#endif
}

mg_err_t mg_autostart_daemon(const char *socket_path, char *err, size_t err_cap) {
    if (!socket_path) return MG_ERR_INVALID_ARG;
    if (err && err_cap > 0) err[0] = '\0';

    char cli_dir[1024];
    if (mg_own_exe_dir(cli_dir, sizeof(cli_dir)) != 0) {
        if (err) snprintf(err, err_cap, "cannot resolve own exe dir");
        return MG_ERR_IO;
    }
    char daemon_path[1024];
    if (mg_path_join(daemon_path, sizeof(daemon_path), cli_dir,
                     "graftd" MG_EXE_SUFFIX) != 0) {
        if (err) snprintf(err, err_cap, "daemon path build failed");
        return MG_ERR_IO;
    }
    if (!mg_file_exists(daemon_path)) {
        if (err) snprintf(err, err_cap,
                          "graftd not found at %s — run cmake --build build",
                          daemon_path);
        return MG_ERR_IO;
    }

    char log_path[1024];
    if (mg_log_path(cli_dir, log_path, sizeof(log_path)) != 0) {
        if (err) snprintf(err, err_cap, "log path build failed");
        return MG_ERR_IO;
    }

    char config_path[1024];
    if (mg_resolve_config(cli_dir, config_path, sizeof(config_path)) != 0) {
        if (err) snprintf(err, err_cap,
                          "config not found ($GRAFT_CONFIG, ./config.yaml, "
                          "./config.example.yaml, %s/../config.example.yaml)",
                          cli_dir);
        return MG_ERR_IO;
    }

    /* Serialize concurrent spawn attempts via an exclusive pidfile lock. */
    char lock_path[1100];
    mg_lock_t lock = MG_LOCK_INVALID;
    int locked = 0;
    if (mg_lock_path(socket_path, lock_path, sizeof(lock_path)) == 0) {
        int lr = mg_try_lock(lock_path, &lock);
        if (lr == 1) {
            locked = 1;
        } else if (lr == 0) {
            /* Another CLI is mid-spawn. Wait briefly for its daemon to come
             * up; if it does, we're done — no second daemon needed. */
            int waited = 0;
            while (waited < MG_AUTOSTART_TIMEOUT_MS) {
                mg_sleep_ms(MG_AUTOSTART_POLL_MS);
                waited += MG_AUTOSTART_POLL_MS;
                int fd = -1;
                if (mg_daemon_socket_connect(socket_path, &fd) == MG_OK) {
                    mg_daemon_socket_close(fd);
                    return MG_OK;
                }
            }
            if (err) snprintf(err, err_cap,
                              "another daemon spawn is in progress but socket %s "
                              "did not become ready in %d ms",
                              socket_path, MG_AUTOSTART_TIMEOUT_MS);
            return MG_ERR_IO;
        }
        /* lr == -1: couldn't open lock file (e.g., dir doesn't exist yet).
         * Fall through without locking — best-effort. */
    }

    /* Re-check the socket while holding the lock: another CLI may have
     * spawned a daemon between our initial connect attempt and acquiring
     * the lock. */
    if (locked) {
        int fd = -1;
        if (mg_daemon_socket_connect(socket_path, &fd) == MG_OK) {
            mg_daemon_socket_close(fd);
            mg_unlock(lock);
            return MG_OK;
        }
    }

    char spawn_err[256] = { 0 };
    mg_proc_t proc = MG_PROC_NONE;
    if (mg_spawn_daemon(daemon_path, cli_dir, config_path, log_path,
                        &proc, spawn_err, sizeof(spawn_err)) != 0) {
        if (locked) mg_unlock(lock);
        if (err) snprintf(err, err_cap, "spawn failed: %s", spawn_err);
        return MG_ERR_IO;
    }

    /* poll until the socket accepts a connection or we time out */
    int elapsed = 0;
    int died = 0;
    while (elapsed < MG_AUTOSTART_TIMEOUT_MS) {
        mg_sleep_ms(MG_AUTOSTART_POLL_MS);
        elapsed += MG_AUTOSTART_POLL_MS;
        int fd = -1;
        if (mg_daemon_socket_connect(socket_path, &fd) == MG_OK) {
            mg_daemon_socket_close(fd);
            if (locked) mg_unlock(lock);
            mg_proc_release(proc);
            return MG_OK;
        }
        /* A daemon that has already exited will never bind the socket. Report
         * now instead of making the user sit through the whole timeout. */
        if (mg_daemon_exited(proc)) { died = 1; break; }
    }
    if (locked) mg_unlock(lock);
    mg_proc_release(proc);
    if (err) {
        /* "socket did not become ready" tells the user nothing they can act on.
         * The daemon already wrote the real reason to its log, so quote it. */
        char tail[900];
        mg_log_tail(log_path, tail, sizeof(tail));
        char what[128];
        if (died) snprintf(what, sizeof(what), "the daemon started and then exited");
        else snprintf(what, sizeof(what), "the daemon did not answer within %d ms",
                      MG_AUTOSTART_TIMEOUT_MS);
        if (tail[0]) {
            snprintf(err, err_cap, "%s. It said:\n%s\n(full log: %s)",
                     what, tail, log_path);
        } else {
            snprintf(err, err_cap, "%s and wrote nothing to %s (config: %s)",
                     what, log_path, config_path);
        }
    }
    return MG_ERR_IO;
}
