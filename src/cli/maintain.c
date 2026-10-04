/* graft CLI — autonomous maintenance (issue #5).
 *
 *   graft maintain status
 *   graft maintain scan [--limit N] [--root DIR] [--no-sources]
 *   graft maintain apply-safe
 *   graft maintain resolve [<candidate-id>] --action A [--node ID] [--by ID]
 *                          [--note TEXT] [--actor NAME]
 *   graft maintain log [--limit N] [--node ID]
 *
 * Thin wrappers over the maintain_* daemon ops (src/maintain/maintain.c),
 * except scan: like `sources diff`, it first asks the daemon for the file
 * sources recorded for the project of DIR (default: the working directory),
 * re-hashes them here and sends the links that no longer match as evidence,
 * so provenance candidates reflect the files as they are now.
 */

#include "maintain.h"
#include "client.h"
#include "sources.h"
#include "graft/source.h"
#include "mpack.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MG_CLI_PATH 4096

static int maintain_usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  graft maintain status\n"
        "  graft maintain scan [--limit N] [--root DIR] [--no-sources]\n"
        "  graft maintain apply-safe\n"
        "  graft maintain resolve [<candidate-id>] --action ACTION [--node ID] [--by ID]\n"
        "                         [--note TEXT] [--actor NAME]\n"
        "  graft maintain log [--limit N] [--node ID]\n"
        "actions: keep keep_both stale retire restore supersede supersede_a supersede_b\n"
        "         merge refresh\n");
    return 2;
}

static void reject_arg(const char *sub, const char *arg) {
    fprintf(stderr, "graft maintain %s: unknown option or missing value: '%s'\n", sub, arg);
    exit(2);
}

static int is_option(const char *arg) {
    return arg[0] == '-' && arg[1] == '-';
}

static int parse_count(const char *sub, const char *flag, const char *s) {
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (!*s || errno == ERANGE || (end && *end) || v <= 0 || v > INT_MAX) {
        fprintf(stderr, "graft maintain %s: %s expects a positive integer, got '%s'\n", sub, flag, s);
        exit(2);
    }
    return (int)v;
}

/* Sends op with the encoded args and prints the reply like any command. */
static int call_and_print(const char *op, char *args, size_t args_len) {
    mpack_tree_t tree;
    void *resp = NULL;
    int rc = mg_cli_call(op, args, args_len, &tree, &resp);
    if (rc == 0) {
        mg_cli_print_value(mpack_tree_root(&tree), 0);
        printf("\n");
    }
    if (resp) {
        mpack_tree_destroy(&tree);
        free(resp);
    }
    return rc;
}

/* Closes an args writer; 0 ok, 1 after printing the encode failure. */
static int finish_args(mpack_writer_t *w, char **args) {
    if (mpack_writer_destroy(w) != mpack_ok) {
        fprintf(stderr, "request encode failed\n");
        free(*args);
        *args = NULL;
        return 1;
    }
    return 0;
}

static int cmd_simple(const char *sub, const char *op, int argc, char **argv) {
    char *args = NULL;
    size_t len = 0;
    mpack_writer_t w;
    int rc;
    if (argc > 3) reject_arg(sub, argv[3]);
    mpack_writer_init_growable(&w, &args, &len);
    mpack_start_map(&w, 0);
    mpack_finish_map(&w);
    if (finish_args(&w, &args) != 0) return 1;
    rc = call_and_print(op, args, len);
    free(args);
    return rc;
}

static int cmd_scan(int argc, char **argv) {
    const char *dir = ".";
    int limit = 0, no_sources = 0, rc;
    static char root[MG_CLI_PATH], project[MG_CLI_PATH];
    char *args = NULL;
    size_t len = 0;
    mpack_writer_t w;
    mpack_tree_t tree;
    void *resp = NULL;

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = parse_count("scan", "--limit", argv[++i]);
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--no-sources")) no_sources = 1;
        else reject_arg("scan", argv[i]);
    }
    if (!no_sources &&
        mg_source_resolve_project(dir, root, sizeof(root), project, sizeof(project)) != 0) {
        fprintf(stderr, "graft maintain scan: not a directory: %s\n", dir);
        return 2;
    }

    /* the project's recorded file sources, re-hashed here */
    if (!no_sources) {
        mpack_writer_init_growable(&w, &args, &len);
        mpack_start_map(&w, 2);
        mpack_write_cstr(&w, "project"); mpack_write_cstr(&w, project);
        mpack_write_cstr(&w, "kind");    mpack_write_cstr(&w, "file");
        mpack_finish_map(&w);
        if (finish_args(&w, &args) != 0) return 1;
        rc = mg_cli_call("sources_list", args, len, &tree, &resp);
        free(args);
        args = NULL;
        if (rc != 0) {
            if (resp) { mpack_tree_destroy(&tree); free(resp); }
            return rc;
        }
    }

    mpack_writer_init_growable(&w, &args, &len);
    mpack_build_map(&w);
    if (limit > 0) {
        mpack_write_cstr(&w, "limit");
        mpack_write_int(&w, limit);
    }
    if (!no_sources) {
        mpack_write_cstr(&w, "project");
        mpack_write_cstr(&w, project);
        mpack_write_cstr(&w, "sources");
        (void)mg_sources_write_stale_links(
            mpack_node_map_cstr_optional(mpack_tree_root(&tree), "result"), root, &w);
        mpack_tree_destroy(&tree);
        free(resp);
    }
    mpack_complete_map(&w);
    if (finish_args(&w, &args) != 0) return 1;
    rc = call_and_print("maintain_scan", args, len);
    free(args);
    return rc;
}

static int cmd_resolve(int argc, char **argv) {
    const char *cid = NULL, *action = NULL, *node = NULL, *by = NULL, *note = NULL, *actor = NULL;
    char *args = NULL;
    size_t len = 0;
    mpack_writer_t w;
    int rc;

    for (int i = 3; i < argc; i++) {
        if      (!strcmp(argv[i], "--action") && i + 1 < argc) action = argv[++i];
        else if (!strcmp(argv[i], "--node")   && i + 1 < argc) node = argv[++i];
        else if (!strcmp(argv[i], "--by")     && i + 1 < argc) by = argv[++i];
        else if (!strcmp(argv[i], "--note")   && i + 1 < argc) note = argv[++i];
        else if (!strcmp(argv[i], "--actor")  && i + 1 < argc) actor = argv[++i];
        else if (is_option(argv[i]) || cid) reject_arg("resolve", argv[i]);
        else cid = argv[i];
    }
    if (!action) {
        fprintf(stderr, "graft maintain resolve: --action is required\n");
        return 2;
    }
    if (!cid && !node) {
        fprintf(stderr, "graft maintain resolve: give a <candidate-id> or --node ID\n");
        return 2;
    }
    if (!actor) {
        const char *env = getenv("GRAFT_AUTHOR");
        actor = env && *env ? env : "agent";
    }
    mpack_writer_init_growable(&w, &args, &len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "action"); mpack_write_cstr(&w, action);
    mpack_write_cstr(&w, "actor");  mpack_write_cstr(&w, actor);
    if (cid)  { mpack_write_cstr(&w, "candidate_id"); mpack_write_cstr(&w, cid); }
    if (node) { mpack_write_cstr(&w, "node");         mpack_write_cstr(&w, node); }
    if (by)   { mpack_write_cstr(&w, "by");           mpack_write_cstr(&w, by); }
    if (note) { mpack_write_cstr(&w, "note");         mpack_write_cstr(&w, note); }
    mpack_complete_map(&w);
    if (finish_args(&w, &args) != 0) return 1;
    rc = call_and_print("maintain_resolve", args, len);
    free(args);
    return rc;
}

static int cmd_log(int argc, char **argv) {
    const char *node = NULL;
    int limit = 0, rc;
    char *args = NULL;
    size_t len = 0;
    mpack_writer_t w;

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = parse_count("log", "--limit", argv[++i]);
        else if (!strcmp(argv[i], "--node") && i + 1 < argc) node = argv[++i];
        else reject_arg("log", argv[i]);
    }
    mpack_writer_init_growable(&w, &args, &len);
    mpack_build_map(&w);
    if (limit > 0) { mpack_write_cstr(&w, "limit"); mpack_write_int(&w, limit); }
    if (node)      { mpack_write_cstr(&w, "node");  mpack_write_cstr(&w, node); }
    mpack_complete_map(&w);
    if (finish_args(&w, &args) != 0) return 1;
    rc = call_and_print("maintain_log", args, len);
    free(args);
    return rc;
}

int mg_maintain_cmd(int argc, char **argv) {
    if (argc < 3) return maintain_usage();
    if (!strcmp(argv[2], "status"))     return cmd_simple("status", "maintain_status", argc, argv);
    if (!strcmp(argv[2], "apply-safe")) return cmd_simple("apply-safe", "maintain_apply_safe", argc, argv);
    if (!strcmp(argv[2], "scan"))       return cmd_scan(argc, argv);
    if (!strcmp(argv[2], "resolve"))    return cmd_resolve(argc, argv);
    if (!strcmp(argv[2], "log"))        return cmd_log(argc, argv);
    return maintain_usage();
}
