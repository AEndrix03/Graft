/* graft view — open the 3D viewer in the browser.
 *
 * On the first invocation, the viewer SPA is not built yet, so this command
 * auto-builds it (npm install + npm run build) before opening the browser.
 * Subsequent invocations skip straight to opening the URL.
 *
 * Viewer-source resolution order:
 *   1. $GRAFT_VIEWER_DIR explicit override.
 *   2. <install_root>/viewer/ derived from argv[0] (user install layout).
 *   3. ./viewer/ relative to cwd (developer / source-tree layout).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "view.h"

#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
#define PATH_SEP_S "\\"
#else
#include <unistd.h>
#define PATH_SEP '/'
#define PATH_SEP_S "/"
#endif

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Strip the trailing filename component from `path` in place. */
static void strip_last(char *path) {
    char *p = strrchr(path, PATH_SEP);
#ifdef _WIN32
    /* on Windows accept both separators */
    char *q = strrchr(path, '/');
    if (q && (!p || q > p)) p = q;
#endif
    if (p) *p = '\0';
}

/* Locate the viewer source directory. Writes the absolute (or cwd-relative)
 * path to `out`. Returns 0 on success, -1 if no candidate exists. */
static int resolve_viewer_dir(const char *argv0, char *out, size_t outsz) {
    const char *env_override = getenv("GRAFT_VIEWER_DIR");
    if (env_override && *env_override) {
        snprintf(out, outsz, "%s", env_override);
        return path_exists(out) ? 0 : -1;
    }

    char base[1024] = "";
#ifdef _WIN32
    if (GetModuleFileNameA(NULL, base, (DWORD)sizeof(base)) == 0) base[0] = '\0';
#else
    if (argv0 && argv0[0] == '/') {
        snprintf(base, sizeof(base), "%s", argv0);
    }
#endif
    if (base[0]) {
        /* base = <root>/bin/graft[.exe] → strip filename, then "bin" */
        strip_last(base);
        strip_last(base);
        char candidate[1100];
        snprintf(candidate, sizeof(candidate), "%s%cviewer", base, PATH_SEP);
        if (path_exists(candidate)) {
            snprintf(out, outsz, "%s", candidate);
            return 0;
        }
    }

    /* Dev / source-tree fallback */
    if (path_exists("viewer")) {
        snprintf(out, outsz, "viewer");
        return 0;
    }
    return -1;
}

static int run_in_dir(const char *dir, const char *what, const char *label) {
    char cmd[2048];
#ifdef _WIN32
    /* `cd /d` handles drive changes; outer quotes preserve spaces in `dir`. */
    snprintf(cmd, sizeof(cmd), "cmd /c \"cd /d \"%s\" && %s\"", dir, what);
#else
    snprintf(cmd, sizeof(cmd), "cd '%s' && %s", dir, what);
#endif
    fprintf(stderr, "  %s...\n", label);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  %s failed (exit %d).\n", label, rc);
    }
    return rc;
}

static int ensure_viewer_built(const char *viewer_dir) {
    char dist_index[1200];
    snprintf(dist_index, sizeof(dist_index),
             "%s%sdist%sindex.html", viewer_dir, PATH_SEP_S, PATH_SEP_S);
    if (path_exists(dist_index)) return 0;

    char package_json[1200];
    snprintf(package_json, sizeof(package_json),
             "%s%spackage.json", viewer_dir, PATH_SEP_S);
    if (!path_exists(package_json)) {
        fprintf(stderr, "Viewer source at %s is missing package.json — skipping auto-build.\n", viewer_dir);
        return -1;
    }

    fprintf(stderr, "Building viewer SPA at %s (first run, ~30s)...\n", viewer_dir);

    /* npm install — idempotent; skip if node_modules already populated. */
    char node_modules[1200];
    snprintf(node_modules, sizeof(node_modules),
             "%s%snode_modules", viewer_dir, PATH_SEP_S);
    if (!path_exists(node_modules)) {
        if (run_in_dir(viewer_dir, "npm install", "npm install") != 0) {
            fprintf(stderr, "Hint: ensure Node.js + npm are on PATH, then retry.\n");
            return -1;
        }
    }

    if (run_in_dir(viewer_dir, "npm run build", "npm run build") != 0) return -1;

    if (!path_exists(dist_index)) {
        fprintf(stderr, "Build finished but %s still missing.\n", dist_index);
        return -1;
    }
    fprintf(stderr, "  viewer built.\n");
    return 0;
}

int mg_view_cmd(int argc, char **argv) {
    int port = 9977;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    }

    char viewer_dir[1024] = "";
    if (resolve_viewer_dir(argv[0], viewer_dir, sizeof(viewer_dir)) == 0) {
        if (ensure_viewer_built(viewer_dir) != 0) {
            fprintf(stderr,
                    "Continuing anyway — graftd may still serve a pre-built bundle elsewhere.\n");
        }
    } else {
        fprintf(stderr,
                "Note: viewer source not found locally; relying on whatever graftd serves at viewer_path.\n");
    }

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    fprintf(stderr, "Opening %s — requires `http.enabled: true` in config.yaml.\n", url);

    char open_cmd[256];
#ifdef _WIN32
    snprintf(open_cmd, sizeof(open_cmd), "start \"\" \"%s\"", url);
#elif defined(__APPLE__)
    snprintf(open_cmd, sizeof(open_cmd), "open '%s'", url);
#else
    snprintf(open_cmd, sizeof(open_cmd), "xdg-open '%s'", url);
#endif
    return system(open_cmd);
}
