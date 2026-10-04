/* graft CLI — provenance freshness (issue #4).
 *
 *   graft sources diff [--root DIR] [--changed-only]
 *   graft sources refresh <node-id-hex> [--root DIR]
 *
 * Both resolve the project of DIR (default: the working directory) the same
 * way `insert --source file:` does, ask the daemon for the file sources it
 * recorded for that project (op sources_list, a plain SQLite read: no
 * embedding), and re-hash the files here, where the paths mean something.
 * Nothing is read from the files but their bytes for BLAKE3.
 *
 * diff reports every recorded file source as unchanged / changed / removed
 * (or unavailable when it exists but cannot be read), with the nodes it
 * supports; a node is "changed" when the file no longer matches the
 * fingerprint recorded on its link. refresh stores the current fingerprints
 * on one node's links (op sources_refresh) after an agent revalidated it,
 * leaving its content alone.
 */

#include "sources.h"
#include "client.h"
#include "graft/source.h"
#include "mpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MG_CLI_PATH 4096

typedef struct {
    const char *p;
    size_t      n;
} str_ref_t;

static void reject_arg(const char *sub, const char *arg) {
    fprintf(stderr, "graft sources %s: unknown option or missing value: '%s'\n", sub, arg);
    exit(2);
}

static int sources_usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  graft sources diff [--root DIR] [--changed-only]\n"
        "  graft sources refresh <hex_id> [--root DIR]\n");
    return 2;
}

static str_ref_t node_ref(mpack_node_t n) {
    str_ref_t r = { NULL, 0 };
    if (mpack_node_type(n) == mpack_type_str) {
        r.p = mpack_node_str(n);
        r.n = mpack_node_strlen(n);
    }
    return r;
}

static str_ref_t field(mpack_node_t map, const char *key) {
    return node_ref(mpack_node_map_cstr_optional(map, key));
}

static int ref_eq(str_ref_t r, const char *s) {
    return r.p && r.n == strlen(s) && memcmp(r.p, s, r.n) == 0;
}

static void write_ref(mpack_writer_t *w, str_ref_t r) {
    if (r.p) mpack_write_str(w, r.p, (uint32_t)r.n);
    else     mpack_write_nil(w);
}

/* State of root/locator against a recorded fingerprint; current gets the
 * file's fingerprint when it could be read. */
static const char *file_state(const char *root, str_ref_t locator, str_ref_t recorded,
                              char current[MG_SOURCE_FP_HEX + 1]) {
    char path[MG_CLI_PATH];
    size_t rn = strlen(root);
    int rc;
    current[0] = '\0';
    if (!locator.p || rn + 1 + locator.n + 1 > sizeof(path)) return "unavailable";
    memcpy(path, root, rn);
    if (rn == 0 || root[rn - 1] != '/') path[rn++] = '/';
    memcpy(path + rn, locator.p, locator.n);
    path[rn + locator.n] = '\0';
    rc = mg_source_hash_file(path, current);
    if (rc == 1) return "removed";
    if (rc != 0) return "unavailable";
    return ref_eq(recorded, current) ? "unchanged" : "changed";
}

/* Sends {op, args} and parses the reply into *tree (backed by *resp, which
 * the caller frees after mpack_tree_destroy). A daemon error is printed as
 * the usual envelope. Returns 0 ok, 1 transport failure, 3 daemon error. */
static int call_daemon(const char *op, const char *args, size_t args_len,
                       mpack_tree_t *tree, void **resp) {
    char *req = NULL;
    size_t req_len = 0, resp_len = 0;
    mpack_writer_t w;
    mpack_writer_init_growable(&w, &req, &req_len);
    mpack_start_map(&w, 2);
    mpack_write_cstr(&w, "op");
    mpack_write_cstr(&w, op);
    mpack_write_cstr(&w, "args");
    mpack_write_object_bytes(&w, args, args_len);
    mpack_finish_map(&w);
    if (mpack_writer_destroy(&w) != mpack_ok) {
        fprintf(stderr, "request encode failed\n");
        free(req);
        return 1;
    }
    int rc = mg_cli_exchange(req, req_len, resp, &resp_len);
    free(req);
    if (rc != 0) return 1;

    mpack_tree_init_data(tree, (const char *)*resp, resp_len);
    mpack_tree_parse(tree);
    if (mpack_tree_error(tree) != mpack_ok) {
        fprintf(stderr, "response decode error\n");
        return 1;
    }
    mpack_node_t root = mpack_tree_root(tree);
    mpack_node_t st = mpack_node_map_cstr_optional(root, "status");
    if (!mpack_node_is_missing(st) && !mpack_node_is_nil(st) && mpack_node_int(st) != 0) {
        mg_cli_print_value(root, 0);
        printf("\n");
        return 3;
    }
    return 0;
}

/* Prints a locally built {status:0, result:...} envelope. */
static int print_built(char *buf, size_t len) {
    mpack_tree_t tree;
    int rc = 0;
    mpack_tree_init_data(&tree, buf, len);
    mpack_tree_parse(&tree);
    if (mpack_tree_error(&tree) != mpack_ok) {
        fprintf(stderr, "output encode error\n");
        rc = 1;
    } else {
        mg_cli_print_value(mpack_tree_root(&tree), 0);
        printf("\n");
    }
    mpack_tree_destroy(&tree);
    return rc;
}

static int resolve(const char *sub, const char *dir, char *root, char *project) {
    if (mg_source_resolve_project(dir, root, MG_CLI_PATH, project, MG_CLI_PATH) != 0) {
        fprintf(stderr, "graft sources %s: not a directory: %s\n", sub, dir);
        return 2;
    }
    return 0;
}

static int contains_ref(const str_ref_t *set, size_t n, str_ref_t r) {
    for (size_t i = 0; i < n; i++) {
        if (set[i].n == r.n && memcmp(set[i].p, r.p, r.n) == 0) return 1;
    }
    return 0;
}

static int cmd_diff(int argc, char **argv) {
    const char *dir = ".";
    int changed_only = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--changed-only")) changed_only = 1;
        else reject_arg("diff", argv[i]);
    }
    static char root[MG_CLI_PATH], project[MG_CLI_PATH];
    int rc = resolve("diff", dir, root, project);
    if (rc != 0) return rc;

    char *args = NULL;
    size_t args_len = 0;
    mpack_writer_t w;
    mpack_writer_init_growable(&w, &args, &args_len);
    mpack_start_map(&w, 2);
    mpack_write_cstr(&w, "project"); mpack_write_cstr(&w, project);
    mpack_write_cstr(&w, "kind");    mpack_write_cstr(&w, "file");
    mpack_finish_map(&w);
    if (mpack_writer_destroy(&w) != mpack_ok) { free(args); return 1; }

    mpack_tree_t tree;
    void *resp = NULL;
    rc = call_daemon("sources_list", args, args_len, &tree, &resp);
    free(args);
    if (rc != 0) {
        if (resp) { mpack_tree_destroy(&tree); free(resp); }
        return rc;
    }

    mpack_node_t result = mpack_node_map_cstr_optional(mpack_tree_root(&tree), "result");
    char *out = NULL;
    size_t out_len = 0;
    rc = mg_sources_diff_report(result, root, project, changed_only, &out, &out_len);
    if (rc == 0) rc = print_built(out, out_len);
    free(out);
    mpack_tree_destroy(&tree);
    free(resp);
    return rc;
}

int mg_sources_diff_report(mpack_node_t list_result, const char *root, const char *project,
                           int changed_only, char **out, size_t *out_len) {
    mpack_writer_t w;
    mpack_node_t list = mpack_node_map_cstr_optional(list_result, "sources");
    size_t n = mpack_node_type(list) == mpack_type_array ? mpack_node_array_length(list) : 0;

    *out = NULL;
    *out_len = 0;

    /* pass 1: hash each file once, classify sources and links */
    typedef struct { char fp[MG_SOURCE_FP_HEX + 1]; const char *state; } src_state_t;
    src_state_t *st = (src_state_t *)calloc(n ? n : 1, sizeof(*st));
    size_t n_links = 0;
    for (size_t i = 0; i < n; i++) {
        n_links += mpack_node_array_length(mpack_node_map_cstr(mpack_node_array_at(list, i), "nodes"));
    }
    str_ref_t *stale = (str_ref_t *)calloc(n_links ? n_links : 1, sizeof(*stale));
    if (!st || !stale) {
        free(st); free(stale);
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    size_t n_stale = 0;
    long long c_unchanged = 0, c_changed = 0, c_removed = 0, c_unavailable = 0;
    for (size_t i = 0; i < n; i++) {
        mpack_node_t src = mpack_node_array_at(list, i);
        mpack_node_t nodes = mpack_node_map_cstr(src, "nodes");
        size_t nn = mpack_node_array_length(nodes);
        str_ref_t first = nn ? field(mpack_node_array_at(nodes, 0), "fingerprint") : (str_ref_t){ NULL, 0 };
        const char *s = file_state(root, field(src, "locator"), first, st[i].fp);
        if (!strcmp(s, "unchanged") || !strcmp(s, "changed")) {
            s = "unchanged";
            for (size_t j = 0; j < nn; j++) {
                if (!ref_eq(field(mpack_node_array_at(nodes, j), "fingerprint"), st[i].fp)) s = "changed";
            }
        }
        st[i].state = s;
        if      (!strcmp(s, "unchanged")) c_unchanged++;
        else if (!strcmp(s, "changed"))   c_changed++;
        else if (!strcmp(s, "removed"))   c_removed++;
        else                              c_unavailable++;
        for (size_t j = 0; j < nn; j++) {
            mpack_node_t node = mpack_node_array_at(nodes, j);
            str_ref_t id = field(node, "id_hex");
            int stale_link = strcmp(s, "unchanged") != 0 &&
                             !(st[i].fp[0] && ref_eq(field(node, "fingerprint"), st[i].fp));
            if (stale_link && id.p && !contains_ref(stale, n_stale, id)) stale[n_stale++] = id;
        }
    }

    /* pass 2: the report */
    mpack_writer_init_growable(&w, out, out_len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "status"); mpack_write_int(&w, 0);
    mpack_write_cstr(&w, "result");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "project"); mpack_write_cstr(&w, project);
    mpack_write_cstr(&w, "root");    mpack_write_cstr(&w, root);
    mpack_write_cstr(&w, "summary");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "sources");     mpack_write_int(&w, (int64_t)n);
    mpack_write_cstr(&w, "unchanged");   mpack_write_int(&w, c_unchanged);
    mpack_write_cstr(&w, "changed");     mpack_write_int(&w, c_changed);
    mpack_write_cstr(&w, "removed");     mpack_write_int(&w, c_removed);
    mpack_write_cstr(&w, "unavailable"); mpack_write_int(&w, c_unavailable);
    mpack_write_cstr(&w, "nodes_to_revalidate"); mpack_write_int(&w, (int64_t)n_stale);
    mpack_complete_map(&w);
    mpack_write_cstr(&w, "sources");
    mpack_build_array(&w);
    for (size_t i = 0; i < n; i++) {
        if (changed_only && !strcmp(st[i].state, "unchanged")) continue;
        mpack_node_t src = mpack_node_array_at(list, i);
        mpack_node_t nodes = mpack_node_map_cstr(src, "nodes");
        mpack_build_map(&w);
        mpack_write_cstr(&w, "kind");    mpack_write_cstr(&w, "file");
        mpack_write_cstr(&w, "locator"); write_ref(&w, field(src, "locator"));
        mpack_write_cstr(&w, "state");   mpack_write_cstr(&w, st[i].state);
        mpack_write_cstr(&w, "fingerprint");
        if (st[i].fp[0]) mpack_write_cstr(&w, st[i].fp);
        else             mpack_write_nil(&w);
        mpack_write_cstr(&w, "nodes");
        mpack_build_array(&w);
        for (size_t j = 0; j < mpack_node_array_length(nodes); j++) {
            mpack_node_t node = mpack_node_array_at(nodes, j);
            str_ref_t rec = field(node, "fingerprint");
            const char *ns = st[i].state;
            if (!strcmp(ns, "unchanged") || !strcmp(ns, "changed")) {
                ns = ref_eq(rec, st[i].fp) ? "unchanged" : "changed";
            }
            mpack_build_map(&w);
            mpack_write_cstr(&w, "id_hex"); write_ref(&w, field(node, "id_hex"));
            mpack_write_cstr(&w, "title");  write_ref(&w, field(node, "title"));
            mpack_write_cstr(&w, "state");  mpack_write_cstr(&w, ns);
            mpack_write_cstr(&w, "recorded_fingerprint"); write_ref(&w, rec);
            mpack_complete_map(&w);
        }
        mpack_complete_array(&w);
        mpack_complete_map(&w);
    }
    mpack_complete_array(&w);
    mpack_complete_map(&w);
    mpack_complete_map(&w);
    free(st);
    free(stale);
    if (mpack_writer_destroy(&w) != mpack_ok) {
        free(*out);
        *out = NULL;
        *out_len = 0;
        return 1;
    }
    return 0;
}

static int cmd_refresh(int argc, char **argv) {
    const char *dir = ".";
    const char *id = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) dir = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] == '-') reject_arg("refresh", argv[i]);
        else if (!id) id = argv[i];
        else reject_arg("refresh", argv[i]);
    }
    if (!id) {
        fprintf(stderr, "graft sources refresh: missing <hex_id>\n");
        return 2;
    }
    static char root[MG_CLI_PATH], project[MG_CLI_PATH];
    int rc = resolve("refresh", dir, root, project);
    if (rc != 0) return rc;

    char *args = NULL;
    size_t args_len = 0;
    mpack_writer_t w;
    mpack_writer_init_growable(&w, &args, &args_len);
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "id_hex"); mpack_write_cstr(&w, id);
    mpack_finish_map(&w);
    if (mpack_writer_destroy(&w) != mpack_ok) { free(args); return 1; }

    mpack_tree_t tree;
    void *resp = NULL;
    rc = call_daemon("sources_list", args, args_len, &tree, &resp);
    free(args);
    if (rc != 0) {
        if (resp) { mpack_tree_destroy(&tree); free(resp); }
        return rc;
    }
    mpack_node_t result = mpack_node_map_cstr_optional(mpack_tree_root(&tree), "result");
    mpack_node_t list = mpack_node_map_cstr_optional(result, "sources");
    size_t n = mpack_node_type(list) == mpack_type_array ? mpack_node_array_length(list) : 0;

    typedef struct { char fp[MG_SOURCE_FP_HEX + 1]; const char *state; } src_state_t;
    src_state_t *st = (src_state_t *)calloc(n ? n : 1, sizeof(*st));
    if (!st) { mpack_tree_destroy(&tree); free(resp); return 1; }
    size_t n_updates = 0;
    for (size_t i = 0; i < n; i++) {
        mpack_node_t src = mpack_node_array_at(list, i);
        mpack_node_t node = mpack_node_array_at(mpack_node_map_cstr(src, "nodes"), 0);
        if (!ref_eq(field(src, "kind"), "file")) { st[i].state = "ignored"; continue; }
        if (!ref_eq(field(src, "project"), project)) { st[i].state = "skipped"; continue; }
        st[i].state = file_state(root, field(src, "locator"), field(node, "fingerprint"), st[i].fp);
        if (st[i].fp[0]) n_updates++;
    }

    int64_t updated = 0;
    if (n_updates > 0) {
        mpack_writer_init_growable(&w, &args, &args_len);
        mpack_start_map(&w, 2);
        mpack_write_cstr(&w, "id_hex"); mpack_write_cstr(&w, id);
        mpack_write_cstr(&w, "updates");
        mpack_start_array(&w, (uint32_t)n_updates);
        for (size_t i = 0; i < n; i++) {
            if (!st[i].fp[0]) continue;
            mpack_node_t src = mpack_node_array_at(list, i);
            mpack_start_map(&w, 4);
            mpack_write_cstr(&w, "project");     mpack_write_cstr(&w, project);
            mpack_write_cstr(&w, "kind");        mpack_write_cstr(&w, "file");
            mpack_write_cstr(&w, "locator");     write_ref(&w, field(src, "locator"));
            mpack_write_cstr(&w, "fingerprint"); mpack_write_cstr(&w, st[i].fp);
            mpack_finish_map(&w);
        }
        mpack_finish_array(&w);
        mpack_finish_map(&w);
        if (mpack_writer_destroy(&w) != mpack_ok) rc = 1;

        mpack_tree_t rtree;
        void *rresp = NULL;
        if (rc == 0) rc = call_daemon("sources_refresh", args, args_len, &rtree, &rresp);
        free(args);
        if (rc == 0) {
            mpack_node_t rr = mpack_node_map_cstr_optional(mpack_tree_root(&rtree), "result");
            mpack_node_t un = mpack_node_map_cstr_optional(rr, "updated");
            if (!mpack_node_is_missing(un) && !mpack_node_is_nil(un)) updated = mpack_node_i64(un);
        }
        if (rresp) { mpack_tree_destroy(&rtree); free(rresp); }
        if (rc != 0) {
            free(st);
            mpack_tree_destroy(&tree);
            free(resp);
            return rc;
        }
    }

    char *out = NULL;
    size_t out_len = 0;
    mpack_writer_init_growable(&w, &out, &out_len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "status"); mpack_write_int(&w, 0);
    mpack_write_cstr(&w, "result");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "id_hex");  mpack_write_cstr(&w, id);
    mpack_write_cstr(&w, "project"); mpack_write_cstr(&w, project);
    mpack_write_cstr(&w, "updated"); mpack_write_int(&w, updated);
    mpack_write_cstr(&w, "sources");
    mpack_build_array(&w);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(st[i].state, "ignored")) continue;
        mpack_node_t src = mpack_node_array_at(list, i);
        mpack_node_t node = mpack_node_array_at(mpack_node_map_cstr(src, "nodes"), 0);
        mpack_build_map(&w);
        mpack_write_cstr(&w, "locator"); write_ref(&w, field(src, "locator"));
        mpack_write_cstr(&w, "state");
        /* refreshed links are unchanged from now on; report what was found */
        mpack_write_cstr(&w, st[i].state);
        mpack_write_cstr(&w, "refreshed"); mpack_write_bool(&w, st[i].fp[0] != '\0');
        mpack_write_cstr(&w, "previous_fingerprint"); write_ref(&w, field(node, "fingerprint"));
        mpack_write_cstr(&w, "fingerprint");
        if (st[i].fp[0]) mpack_write_cstr(&w, st[i].fp);
        else             mpack_write_nil(&w);
        if (!strcmp(st[i].state, "skipped")) {
            mpack_write_cstr(&w, "project"); write_ref(&w, field(src, "project"));
        }
        mpack_complete_map(&w);
    }
    mpack_complete_array(&w);
    mpack_complete_map(&w);
    mpack_complete_map(&w);
    free(st);
    rc = mpack_writer_destroy(&w) == mpack_ok ? print_built(out, out_len) : 1;
    free(out);
    mpack_tree_destroy(&tree);
    free(resp);
    return rc;
}

int mg_sources_cmd(int argc, char **argv) {
    if (argc < 3) return sources_usage();
    if (!strcmp(argv[2], "diff"))    return cmd_diff(argc, argv);
    if (!strcmp(argv[2], "refresh")) return cmd_refresh(argc, argv);
    return sources_usage();
}

const char *mg_sources_file_state(const char *project, const char *locator,
                                  const char *fingerprint) {
    static int resolved = 0;
    static char root[MG_CLI_PATH], cwd_project[MG_CLI_PATH];
    char current[MG_SOURCE_FP_HEX + 1];
    str_ref_t loc, rec;
    if (!resolved) {
        resolved = mg_source_resolve_project(".", root, sizeof(root),
                                             cwd_project, sizeof(cwd_project)) == 0 ? 1 : -1;
    }
    if (resolved < 0 || !project || !locator || strcmp(project, cwd_project) != 0) return NULL;
    loc.p = locator; loc.n = strlen(locator);
    rec.p = fingerprint; rec.n = fingerprint ? strlen(fingerprint) : 0;
    return file_state(root, loc, rec, current);
}
