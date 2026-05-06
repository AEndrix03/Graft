/* memgraph CLI — profile management.
 *
 * A profile tenant-isolates the graph: each profile gets its own SQLite
 * DB file and its own daemon listening on its own socket.
 *
 *   <MEMGRAPH_HOME>/profiles/<name>/memgraph.db   — the per-profile DB
 *
 * Socket path:
 *   POSIX  : /tmp/memgraph-<name>.sock
 *   Windows: <MEMGRAPH_HOME>\sockets\<name>.sock
 *
 * The CLI is the only component that knows about profiles. Before
 * connecting to (or auto-starting) the daemon, main.c sets MEMGRAPH_SOCKET
 * and MEMGRAPH_DB_PATH in its own env, which the daemon honors as
 * overrides on top of the YAML config.
 *
 * Export/import use a plain file copy (the file IS a SQLite DB carrying
 * the full graph). Both operations refuse when a daemon is running on
 * the affected profile, to avoid copying mid-write WAL state.
 */

#include "profile.h"
#include "../daemon/internal.h"
#include "memgraph/error.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define MG_PATH_SEP '\\'
#  define mg_mkdir(p) _mkdir(p)
#  define mg_unlink(p) _unlink(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <errno.h>
#  define MG_PATH_SEP '/'
#  define mg_mkdir(p) mkdir((p), 0755)
#  define mg_unlink(p) unlink(p)
#endif

/* ---------- helpers ---------- */

static int file_exists(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
#else
    struct stat st;
    return (stat(p, &st) == 0) ? 1 : 0;
#endif
}

static int dir_exists(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    return (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
#endif
}

/* mkdir -p — create each intermediate component. Returns 0 on success. */
static int mkdir_p(const char *path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (i == n || buf[i] == MG_PATH_SEP) {
            char saved = buf[i];
            buf[i] = '\0';
            if (!dir_exists(buf)) {
                if (mg_mkdir(buf) != 0) {
                    /* race-tolerant: another caller may have created it */
                    if (!dir_exists(buf)) {
                        buf[i] = saved;
                        return -1;
                    }
                }
            }
            buf[i] = saved;
        }
    }
    return 0;
}

static int rmdir_recursive(const char *path) {
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rmdir_recursive(child);
            else
                DeleteFileA(child);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            struct stat st;
            if (stat(child, &st) == 0) {
                if (S_ISDIR(st.st_mode)) rmdir_recursive(child);
                else                     unlink(child);
            }
        }
        closedir(d);
    }
    return rmdir(path);
#endif
}

static int copy_file(const char *src, const char *dst) {
    FILE *fi = fopen(src, "rb");
    if (!fi) return -1;
    FILE *fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); return -1; }
    char buf[64 * 1024];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) { rc = -1; break; }
    }
    if (ferror(fi)) rc = -1;
    fclose(fi);
    if (fclose(fo) != 0) rc = -1;
    return rc;
}

/* SQLite header magic — first 16 bytes of any SQLite DB file. */
static int looks_like_sqlite(const char *path) {
    static const char magic[] = "SQLite format 3";
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[16] = { 0 };
    size_t n = fread(buf, 1, 16, f);
    fclose(f);
    return (n >= 16 && memcmp(buf, magic, 15) == 0 && buf[15] == 0) ? 1 : 0;
}

/* Probe whether a daemon is currently listening on `socket_path`. */
static int daemon_running(const char *socket_path) {
    int fd = -1;
    if (mg_daemon_socket_connect(socket_path, &fd) == MG_OK) {
        mg_daemon_socket_close(fd);
        return 1;
    }
    return 0;
}

/* ---------- name validation ---------- */

int mg_profile_name_valid(const char *name) {
    if (!name) return -1;
    size_t n = strlen(name);
    if (n == 0 || n > 64) return -1;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return -1;
    }
    return 0;
}

/* ---------- path resolution ---------- */

int mg_profile_home(char *out, size_t cap) {
    const char *env = getenv("MEMGRAPH_HOME");
    if (env && *env) {
        if (snprintf(out, cap, "%s", env) >= (int)cap) return -1;
    } else {
#ifdef _WIN32
        const char *base = getenv("USERPROFILE");
        if (!base || !*base) base = getenv("LOCALAPPDATA");
        if (!base || !*base) return -1;
        if (snprintf(out, cap, "%s\\.lmemorygraph", base) >= (int)cap) return -1;
#else
        const char *home = getenv("HOME");
        if (!home || !*home) return -1;
        if (snprintf(out, cap, "%s/.lmemorygraph", home) >= (int)cap) return -1;
#endif
    }
    if (mkdir_p(out) != 0) return -1;
    return 0;
}

int mg_profile_active(char *out, size_t cap) {
    const char *env = getenv("MEMGRAPH_PROFILE");
    if (env && *env && mg_profile_name_valid(env) == 0) {
        if (snprintf(out, cap, "%s", env) >= (int)cap) return -1;
        return 0;
    }
    if (snprintf(out, cap, "%s", MG_PROFILE_DEFAULT) >= (int)cap) return -1;
    return 0;
}

int mg_profile_dir(const char *name, char *out, size_t cap, int create) {
    char home[1024];
    if (mg_profile_home(home, sizeof(home)) != 0) return -1;
    if (snprintf(out, cap, "%s%cprofiles%c%s", home, MG_PATH_SEP, MG_PATH_SEP, name) >= (int)cap)
        return -1;
    if (create && mkdir_p(out) != 0) return -1;
    return 0;
}

int mg_profile_db_path(const char *name, char *out, size_t cap, int create) {
    char dir[1024];
    if (mg_profile_dir(name, dir, sizeof(dir), create) != 0) return -1;
    if (snprintf(out, cap, "%s%cmemgraph.db", dir, MG_PATH_SEP) >= (int)cap) return -1;
    return 0;
}

int mg_profile_socket_path(const char *name, char *out, size_t cap, int create) {
#ifdef _WIN32
    char home[1024];
    if (mg_profile_home(home, sizeof(home)) != 0) return -1;
    char dir[1024];
    if (snprintf(dir, sizeof(dir), "%s\\sockets", home) >= (int)sizeof(dir)) return -1;
    if (create && mkdir_p(dir) != 0) return -1;
    if (snprintf(out, cap, "%s\\%s.sock", dir, name) >= (int)cap) return -1;
    (void)create;
    return 0;
#else
    (void)create;
    if (snprintf(out, cap, "/tmp/memgraph-%s.sock", name) >= (int)cap) return -1;
    return 0;
#endif
}

int mg_profile_exists(const char *name) {
    if (mg_profile_name_valid(name) != 0) return 0;
    char dir[1024];
    if (mg_profile_dir(name, dir, sizeof(dir), 0) != 0) return 0;
    return dir_exists(dir);
}

/* ---------- subcommands ---------- */

static int cmd_list(void) {
    char home[1024];
    if (mg_profile_home(home, sizeof(home)) != 0) {
        fprintf(stderr, "cannot resolve MEMGRAPH_HOME\n");
        return 1;
    }
    char active[128];
    mg_profile_active(active, sizeof(active));

    char profiles_dir[1024];
    snprintf(profiles_dir, sizeof(profiles_dir), "%s%cprofiles", home, MG_PATH_SEP);
    if (!dir_exists(profiles_dir)) (void)mkdir_p(profiles_dir);

    printf("{\n  \"home\": \"%s\",\n  \"active\": \"%s\",\n  \"profiles\": [",
           home, active);
    int first = 1;

#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", profiles_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            printf("%s\n    \"%s\"", first ? "" : ",", fd.cFileName);
            first = 0;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(profiles_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", profiles_dir, de->d_name);
            if (!dir_exists(child)) continue;
            printf("%s\n    \"%s\"", first ? "" : ",", de->d_name);
            first = 0;
        }
        closedir(d);
    }
#endif
    printf("%s]\n}\n", first ? "" : "\n  ");
    return 0;
}

static int cmd_current(void) {
    char active[128];
    mg_profile_active(active, sizeof(active));
    printf("{\n  \"active\": \"%s\"\n}\n", active);
    return 0;
}

static int cmd_add(const char *name) {
    if (mg_profile_name_valid(name) != 0) {
        fprintf(stderr, "invalid profile name (allowed: [a-zA-Z0-9_-]{1,64})\n");
        return 2;
    }
    if (mg_profile_exists(name)) {
        fprintf(stderr, "profile '%s' already exists\n", name);
        return 1;
    }
    char dir[1024];
    if (mg_profile_dir(name, dir, sizeof(dir), 1) != 0) {
        fprintf(stderr, "failed to create profile dir\n");
        return 1;
    }
    printf("{\n  \"created\": \"%s\",\n  \"dir\": \"%s\"\n}\n", name, dir);
    return 0;
}

static int cmd_remove(const char *name, int yes) {
    if (mg_profile_name_valid(name) != 0) {
        fprintf(stderr, "invalid profile name\n");
        return 2;
    }
    if (!strcmp(name, MG_PROFILE_DEFAULT)) {
        fprintf(stderr, "profile 'default' cannot be removed\n");
        return 1;
    }
    if (!mg_profile_exists(name)) {
        fprintf(stderr, "profile '%s' does not exist\n", name);
        return 1;
    }
    char sock[1024];
    if (mg_profile_socket_path(name, sock, sizeof(sock), 0) == 0
        && daemon_running(sock)) {
        fprintf(stderr, "a daemon is currently running for profile '%s' — stop it first\n",
                name);
        return 1;
    }
    if (!yes) {
        fprintf(stderr, "About to permanently delete profile '%s' and ALL its data.\n", name);
        fprintf(stderr, "Type the profile name again to confirm: ");
        fflush(stderr);
        char buf[128] = { 0 };
        if (!fgets(buf, sizeof(buf), stdin)) return 1;
        size_t l = strlen(buf);
        while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
        if (strcmp(buf, name) != 0) {
            fprintf(stderr, "confirmation did not match — aborted\n");
            return 1;
        }
    }
    char dir[1024];
    if (mg_profile_dir(name, dir, sizeof(dir), 0) != 0) return 1;
    if (rmdir_recursive(dir) != 0) {
        fprintf(stderr, "failed to remove %s\n", dir);
        return 1;
    }
    printf("{\n  \"removed\": \"%s\"\n}\n", name);
    return 0;
}

static int detect_shell_syntax(const char *hint, char *out, size_t cap) {
    /* Returns the env-export syntax for the current shell. Honors --shell flag
     * if given; otherwise sniffs from $SHELL / OS. */
    if (hint) {
        if (!strcmp(hint, "bash") || !strcmp(hint, "zsh") || !strcmp(hint, "sh")) {
            snprintf(out, cap, "posix"); return 0;
        }
        if (!strcmp(hint, "fish")) { snprintf(out, cap, "fish"); return 0; }
        if (!strcmp(hint, "powershell") || !strcmp(hint, "pwsh")) {
            snprintf(out, cap, "powershell"); return 0;
        }
        if (!strcmp(hint, "cmd")) { snprintf(out, cap, "cmd"); return 0; }
    }
    /* On Windows the binary may still be invoked from a POSIX-ish shell
     * (Git Bash, MSYS2, WSL, Cygwin). Trust $SHELL when present — it tells
     * us what the user actually typed in. Only fall back to PowerShell when
     * there's no signal, which is the typical native-Windows case. */
    const char *sh = getenv("SHELL");
    if (sh && *sh) {
        if (strstr(sh, "fish"))                                       snprintf(out, cap, "fish");
        else if (strstr(sh, "bash") || strstr(sh, "zsh") || strstr(sh, "sh")) snprintf(out, cap, "posix");
        else                                                          snprintf(out, cap, "posix");
        return 0;
    }
#ifdef _WIN32
    snprintf(out, cap, "powershell");
#else
    snprintf(out, cap, "posix");
#endif
    return 0;
}

static int cmd_set(const char *name, const char *shell_hint) {
    if (mg_profile_name_valid(name) != 0) {
        fprintf(stderr, "invalid profile name\n");
        return 2;
    }
    if (!mg_profile_exists(name) && strcmp(name, MG_PROFILE_DEFAULT) != 0) {
        fprintf(stderr, "profile '%s' does not exist (run: memgraph profile add %s)\n",
                name, name);
        return 1;
    }
    /* Print the export line for the detected shell. The CLI cannot mutate
     * the parent shell's env directly, so the user pipes this into eval /
     * Invoke-Expression. To make it persistent, the user adds the printed
     * line to their shell rc file themselves. */
    char syntax[16];
    detect_shell_syntax(shell_hint, syntax, sizeof(syntax));
    if (!strcmp(syntax, "fish")) {
        printf("set -x MEMGRAPH_PROFILE %s\n", name);
    } else if (!strcmp(syntax, "powershell")) {
        printf("$env:MEMGRAPH_PROFILE = '%s'\n", name);
    } else if (!strcmp(syntax, "cmd")) {
        printf("set MEMGRAPH_PROFILE=%s\n", name);
    } else {
        printf("export MEMGRAPH_PROFILE=%s\n", name);
    }
    fprintf(stderr,
        "Apply to the current shell:\n"
        "  bash/zsh/fish:  eval \"$(memgraph profile set %s)\"\n"
        "  PowerShell:     memgraph profile set %s | Out-String | Invoke-Expression\n",
        name, name);
    return 0;
}

static int cmd_export(const char *name, const char *path) {
    if (mg_profile_name_valid(name) != 0) {
        fprintf(stderr, "invalid profile name\n");
        return 2;
    }
    if (!path || !*path) {
        fprintf(stderr, "--path is required\n");
        return 2;
    }
    if (!mg_profile_exists(name)) {
        fprintf(stderr, "profile '%s' does not exist\n", name);
        return 1;
    }
    char sock[1024];
    if (mg_profile_socket_path(name, sock, sizeof(sock), 0) == 0
        && daemon_running(sock)) {
        fprintf(stderr,
            "a daemon is currently running for profile '%s' — stop it first to "
            "guarantee a consistent export\n", name);
        return 1;
    }
    char db[1024];
    if (mg_profile_db_path(name, db, sizeof(db), 0) != 0) return 1;
    if (!file_exists(db)) {
        fprintf(stderr, "profile '%s' has no DB yet (nothing to export)\n", name);
        return 1;
    }
    if (copy_file(db, path) != 0) {
        fprintf(stderr, "copy failed: %s -> %s\n", db, path);
        return 1;
    }
    printf("{\n  \"exported\": \"%s\",\n  \"from\": \"%s\",\n  \"to\": \"%s\"\n}\n",
           name, db, path);
    return 0;
}

static int cmd_import(const char *name, const char *path, int force) {
    if (mg_profile_name_valid(name) != 0) {
        fprintf(stderr, "invalid profile name\n");
        return 2;
    }
    if (!path || !*path || !file_exists(path)) {
        fprintf(stderr, "--file does not exist: %s\n", path ? path : "(none)");
        return 1;
    }
    if (!looks_like_sqlite(path)) {
        fprintf(stderr,
            "file does not look like a memgraph profile (missing SQLite "
            "header). Aborting to avoid corrupting the profile.\n");
        return 1;
    }
    if (mg_profile_exists(name)) {
        char sock[1024];
        if (mg_profile_socket_path(name, sock, sizeof(sock), 0) == 0
            && daemon_running(sock)) {
            fprintf(stderr,
                "profile '%s' is currently in use by a running daemon — stop it first\n",
                name);
            return 1;
        }
        if (!force) {
            fprintf(stderr,
                "profile '%s' already exists. Pass --force to overwrite its DB.\n",
                name);
            return 1;
        }
    }
    char db[1024];
    if (mg_profile_db_path(name, db, sizeof(db), 1) != 0) return 1;
    if (copy_file(path, db) != 0) {
        fprintf(stderr, "copy failed: %s -> %s\n", path, db);
        return 1;
    }
    printf("{\n  \"imported\": \"%s\",\n  \"from\": \"%s\",\n  \"to\": \"%s\"\n}\n",
           name, path, db);
    return 0;
}

/* ---------- dispatcher ---------- */

static int profile_usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  memgraph profile list\n"
        "  memgraph profile current\n"
        "  memgraph profile add    <name>\n"
        "  memgraph profile remove <name> [--yes]\n"
        "  memgraph profile set    <name> [--shell bash|zsh|fish|powershell|cmd]\n"
        "  memgraph profile export <name> --path <file>\n"
        "  memgraph profile import --name <name> --file <file> [--force]\n");
    return 2;
}

int mg_profile_cmd(int argc, char **argv) {
    if (argc < 3) return profile_usage();
    const char *sub = argv[2];

    if (!strcmp(sub, "list"))    return cmd_list();
    if (!strcmp(sub, "current")) return cmd_current();

    if (!strcmp(sub, "add")) {
        if (argc < 4) return profile_usage();
        return cmd_add(argv[3]);
    }
    if (!strcmp(sub, "remove") || !strcmp(sub, "rm")) {
        if (argc < 4) return profile_usage();
        const char *name = argv[3];
        int yes = 0;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--yes") || !strcmp(argv[i], "-y")) yes = 1;
        }
        return cmd_remove(name, yes);
    }
    if (!strcmp(sub, "set")) {
        if (argc < 4) return profile_usage();
        const char *name = argv[3];
        const char *shell = NULL;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--shell") && i + 1 < argc) shell = argv[++i];
        }
        return cmd_set(name, shell);
    }
    if (!strcmp(sub, "export")) {
        if (argc < 4) return profile_usage();
        const char *name = argv[3];
        const char *path = NULL;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        }
        return cmd_export(name, path);
    }
    if (!strcmp(sub, "import")) {
        const char *name = NULL, *path = NULL;
        int force = 0;
        for (int i = 3; i < argc; i++) {
            if      (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
            else if (!strcmp(argv[i], "--file") && i + 1 < argc) path = argv[++i];
            else if (!strcmp(argv[i], "--force")) force = 1;
        }
        if (!name || !path) return profile_usage();
        return cmd_import(name, path, force);
    }
    return profile_usage();
}
