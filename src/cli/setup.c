#include "setup.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <direct.h>
#  include <windows.h>
#  define MG_PATH_SEP '\\'
#  define mg_mkdir(p) _mkdir(p)
#else
#  include <dirent.h>
#  include <unistd.h>
#  define MG_PATH_SEP '/'
#  define mg_mkdir(p) mkdir((p), 0700)
#endif

enum mg_setup_agent {
    MG_SETUP_CLAUDECODE,
    MG_SETUP_CODEX,
    MG_SETUP_OPENCODE
};


static int path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s%c%s", a, MG_PATH_SEP, b);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int parent_dir(char *path) {
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\') n--;
    if (n == 0) return -1;
    path[n - 1] = '\0';
    return 0;
}

static int file_exists(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
#endif
}

static int dir_exists(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
#endif
}

static int mkdir_p(const char *path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (i == n || buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            if (!dir_exists(buf)) {
                if (mg_mkdir(buf) != 0 && !dir_exists(buf)) {
                    buf[i] = saved;
                    return -1;
                }
            }
            buf[i] = saved;
        }
    }
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    char dir[1024];
    if (snprintf(dir, sizeof(dir), "%s", dst) >= (int)sizeof(dir)) return -1;
    if (parent_dir(dir) != 0 || mkdir_p(dir) != 0) return -1;

    FILE *fi = fopen(src, "rb");
    if (!fi) return -1;
    FILE *fo = fopen(dst, "wb");
    if (!fo) {
        fclose(fi);
        return -1;
    }
    char buf[64 * 1024];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(fi)) rc = -1;
    fclose(fi);
    if (fclose(fo) != 0) rc = -1;
    return rc;
}

static int copy_tree(const char *src, const char *dst) {
    if (!dir_exists(src)) return -1;
    if (mkdir_p(dst) != 0) return -1;

#ifdef _WIN32
    char pattern[1024];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", src) >= (int)sizeof(pattern)) return -1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int rc = 0;
    do {
        const char *name = fd.cFileName;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char s[1024], d[1024];
        if (path_join(s, sizeof(s), src, name) != 0 || path_join(d, sizeof(d), dst, name) != 0) {
            rc = -1;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (copy_tree(s, d) != 0) {
                rc = -1;
                break;
            }
        } else if (copy_file(s, d) != 0) {
            rc = -1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return rc;
#else
    DIR *dir = opendir(src);
    if (!dir) return -1;
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char s[1024], d[1024];
        if (path_join(s, sizeof(s), src, name) != 0 || path_join(d, sizeof(d), dst, name) != 0) {
            rc = -1;
            break;
        }
        if (dir_exists(s)) {
            if (copy_tree(s, d) != 0) {
                rc = -1;
                break;
            }
        } else if (copy_file(s, d) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(dir);
    return rc;
#endif
}

static int ends_with(const char *s, const char *suffix) {
    size_t n, m;
    if (!s || !suffix) return 0;
    n = strlen(s);
    m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int normalize_codex_skill_file(const char *path) {
    FILE *in = fopen(path, "rb");
    if (!in) return -1;
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        fclose(in);
        return -1;
    }
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    char line[8192];
    while (fgets(line, sizeof(line), in)) {
        if (!strncmp(line, "description: ", 13) && strncmp(line, "description: >-", 15)) {
            char *desc = line + 13;
            fputs("description: >-\n  ", out);
            fputs(desc, out);
            if (!strchr(desc, '\n')) fputc('\n', out);
        } else {
            fputs(line, out);
        }
    }
    int rc = ferror(in) ? -1 : 0;
    if (fclose(in) != 0) rc = -1;
    if (fclose(out) != 0) rc = -1;
    if (rc != 0) {
        remove(tmp);
        return -1;
    }
    if (remove(path) != 0) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

static int normalize_codex_skill_tree(const char *root) {
    if (!dir_exists(root)) return 0;

#ifdef _WIN32
    char pattern[1024];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", root) >= (int)sizeof(pattern)) return -1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int rc = 0;
    do {
        const char *name = fd.cFileName;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char p[1024];
        if (path_join(p, sizeof(p), root, name) != 0) { rc = -1; break; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (normalize_codex_skill_tree(p) != 0) { rc = -1; break; }
        } else if (ends_with(name, "SKILL.md")) {
            if (normalize_codex_skill_file(p) != 0) { rc = -1; break; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return rc;
#else
    DIR *dir = opendir(root);
    if (!dir) return -1;
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char p[1024];
        if (path_join(p, sizeof(p), root, name) != 0) { rc = -1; break; }
        if (dir_exists(p)) {
            if (normalize_codex_skill_tree(p) != 0) { rc = -1; break; }
        } else if (ends_with(name, "SKILL.md")) {
            if (normalize_codex_skill_file(p) != 0) { rc = -1; break; }
        }
    }
    closedir(dir);
    return rc;
#endif
}

static int user_home(char *out, size_t cap) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (!home || !*home) home = getenv("LOCALAPPDATA");
#else
    const char *home = getenv("HOME");
#endif
    if (!home || !*home) return -1;
    return (snprintf(out, cap, "%s", home) < (int)cap) ? 0 : -1;
}

static int own_exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return -1;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
#endif
    if (parent_dir(buf) != 0) return -1;
    return (snprintf(out, cap, "%s", buf) < (int)cap) ? 0 : -1;
}

static int cwd_path(char *out, size_t cap) {
#ifdef _WIN32
    return _getcwd(out, (int)cap) ? 0 : -1;
#else
    return getcwd(out, cap) ? 0 : -1;
#endif
}

static const char *agent_display_name(enum mg_setup_agent agent) {
    switch (agent) {
        case MG_SETUP_CLAUDECODE: return "Claude Code";
        case MG_SETUP_CODEX: return "Codex";
        case MG_SETUP_OPENCODE: return "OpenCode";
    }
    return "agent";
}


static int candidate_standard_dir(char *out, size_t cap, const char *base) {
    char tmp[1024];
    if (path_join(tmp, sizeof(tmp), base, "integrations") != 0) return -1;
    return path_join(out, cap, tmp, "standard");
}

static int find_standard_dir(char *out, size_t cap) {
    const char *env = getenv("GRAFT_INTEGRATIONS_DIR");
    if (env && *env) {
        if (path_join(out, cap, env, "standard") == 0 && dir_exists(out)) return 0;
        if (dir_exists(env)) {
            char entry[1024];
            if (path_join(entry, sizeof(entry), env, "skills") == 0 && dir_exists(entry)) {
                snprintf(out, cap, "%s", env);
                return 0;
            }
        }
    }

    char base[1024], cand[1024];
    if (own_exe_dir(base, sizeof(base)) == 0) {
        if (candidate_standard_dir(cand, sizeof(cand), base) == 0 && dir_exists(cand)) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
        char parent[1024];
        if (snprintf(parent, sizeof(parent), "%s", base) < (int)sizeof(parent)
            && parent_dir(parent) == 0) {
            if (candidate_standard_dir(cand, sizeof(cand), parent) == 0 && dir_exists(cand)) {
                snprintf(out, cap, "%s", cand);
                return 0;
            }
            char share[1024], graft[1024], integrations[1024];
            if (path_join(share, sizeof(share), parent, "share") == 0
                && path_join(graft, sizeof(graft), share, "graft") == 0
                && path_join(integrations, sizeof(integrations), graft, "integrations") == 0
                && path_join(cand, sizeof(cand), integrations, "standard") == 0
                && dir_exists(cand)) {
                snprintf(out, cap, "%s", cand);
                return 0;
            }
        }
    }

    if (cwd_path(base, sizeof(base)) == 0
        && candidate_standard_dir(cand, sizeof(cand), base) == 0
        && dir_exists(cand)) {
        snprintf(out, cap, "%s", cand);
        return 0;
    }
    return -1;
}



/* Installing an agent integration means one thing: copying the skills into the
 * agent's skill directory. No hooks, no settings.json surgery, no instruction
 * files - `/graft-init`, itself one of the installed skills, does the wiring
 * from inside the agent where it can ask the user what it needs. */

static int install_skills(const char *src, const char *dst_skills, const char *display) {
    char src_skills[1024];
    if (path_join(src_skills, sizeof(src_skills), src, "skills") != 0) return -1;
    if (!dir_exists(src_skills)) return -1;
    if (copy_tree(src_skills, dst_skills) != 0) return -1;
    if (normalize_codex_skill_tree(dst_skills) != 0) return -1;
    printf("  %-12s skills installed to %s\n", display, dst_skills);
    return 0;
}

/* Where each agent keeps its user-level skills, and the directory whose
 * presence means "this agent is installed on this machine". */
static int agent_paths(enum mg_setup_agent agent, const char *home,
                       char *agent_home, size_t home_cap,
                       char *skills, size_t skills_cap) {
    char base[1024];
    switch (agent) {
        case MG_SETUP_CLAUDECODE:
            if (path_join(agent_home, home_cap, home, ".claude") != 0) return -1;
            break;
        case MG_SETUP_CODEX:
            if (path_join(agent_home, home_cap, home, ".codex") != 0) return -1;
            break;
        case MG_SETUP_OPENCODE:
            if (path_join(base, sizeof(base), home, ".config") != 0) return -1;
            if (path_join(agent_home, home_cap, base, "opencode") != 0) return -1;
            break;
        default:
            return -1;
    }
    return path_join(skills, skills_cap, agent_home, "skills");
}

static int setup_agent(enum mg_setup_agent agent, const char *src, const char *home) {
    char agent_home[1024], skills[1024];
    if (agent_paths(agent, home, agent_home, sizeof(agent_home), skills, sizeof(skills)) != 0) {
        return -1;
    }
    return install_skills(src, skills, agent_display_name(agent));
}

static int parse_agent(const char *s, enum mg_setup_agent *agent) {
    if (!strcmp(s, "claudecode") || !strcmp(s, "claude-code") || !strcmp(s, "claude")) {
        *agent = MG_SETUP_CLAUDECODE;
        return 0;
    }
    if (!strcmp(s, "codex")) {
        *agent = MG_SETUP_CODEX;
        return 0;
    }
    if (!strcmp(s, "opencode") || !strcmp(s, "open-code") || !strcmp(s, "open_code")) {
        *agent = MG_SETUP_OPENCODE;
        return 0;
    }
    return -1;
}

static const enum mg_setup_agent MG_SETUP_ALL[] = {
    MG_SETUP_CLAUDECODE, MG_SETUP_CODEX, MG_SETUP_OPENCODE
};
#define MG_SETUP_N_AGENTS (sizeof(MG_SETUP_ALL) / sizeof(MG_SETUP_ALL[0]))

static void print_usage(FILE *f) {
    fprintf(f, "usage: graft setup [claudecode|codex|opencode]\n");
    fprintf(f, "       with no argument, every agent found on this machine is set up\n");
}

int mg_setup_cmd(int argc, char **argv) {
    if (argc > 3) {
        print_usage(stderr);
        return 2;
    }

    enum mg_setup_agent one;
    int explicit_target = (argc == 3);
    if (explicit_target && parse_agent(argv[2], &one) != 0) {
        fprintf(stderr, "unknown setup target: %s\n", argv[2]);
        print_usage(stderr);
        return 2;
    }

    char home[1024], src[1024];
    if (user_home(home, sizeof(home)) != 0) {
        fprintf(stderr, "setup failed: could not resolve user profile directory\n");
        return 1;
    }
    if (find_standard_dir(src, sizeof(src)) != 0) {
        fprintf(stderr,
                "setup failed: could not find integrations/standard (set GRAFT_INTEGRATIONS_DIR)\n");
        return 1;
    }

    if (explicit_target) {
        if (setup_agent(one, src, home) != 0) {
            fprintf(stderr, "setup failed: %s\n", strerror(errno ? errno : EINVAL));
            return 1;
        }
        printf("\nRestart %s, then run /graft-init inside it.\n", agent_display_name(one));
        return 0;
    }

    /* No target given: set up every agent that is actually installed here. */
    int done = 0;
    for (size_t i = 0; i < MG_SETUP_N_AGENTS; i++) {
        enum mg_setup_agent a = MG_SETUP_ALL[i];
        char agent_home[1024], skills[1024];
        if (agent_paths(a, home, agent_home, sizeof(agent_home), skills, sizeof(skills)) != 0) {
            continue;
        }
        if (!dir_exists(agent_home)) continue;
        if (install_skills(src, skills, agent_display_name(a)) != 0) {
            fprintf(stderr, "  %-12s FAILED: %s\n", agent_display_name(a),
                    strerror(errno ? errno : EINVAL));
            continue;
        }
        done++;
    }

    if (done == 0) {
        fprintf(stderr,
                "setup found no agent on this machine.\n"
                "Expected one of ~/.claude, ~/.codex or ~/.config/opencode.\n"
                "Install your agent first, or name it: graft setup claudecode\n");
        return 1;
    }

    printf("\nRestart your agent, then run /graft-init inside it.\n");
    return 0;
}
