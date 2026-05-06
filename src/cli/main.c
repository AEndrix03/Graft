/* memgraph — thin CLI client.
 *
 *   memgraph insert     --summary "..." --detail "..." --keyword foo --keyword bar
 *   memgraph query      "testo della domanda"
 *   memgraph retrieve   "testo" [--top-k 25]
 *   memgraph explore    "testo" --keyword k1 --depth 3 [--beam 4]
 *   memgraph get        <hex_id>
 *   memgraph stats
 *   memgraph classify   --summary "..."
 *   memgraph consolidate
 *
 * Connects to the daemon socket (default /tmp/memgraph.sock, override via
 * env MEMGRAPH_SOCKET), sends a single request frame, prints the parsed
 * response in mpack's JSON-ish format and exits.
 */

#include "../daemon/internal.h"
#include "memgraph/wire.h"
#include "memgraph/error.h"
#include "mpack.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* JSON-ish pretty-printer for mpack nodes.
 * mpack's own print helpers are gated on MPACK_DEBUG (off in release),
 * so we roll our own — small, no dependencies on the build flag. */
static void print_value(mpack_node_t n, int indent);

static void put_indent(int indent) {
    for (int i = 0; i < indent; i++) fputs("  ", stdout);
}

static void print_str(mpack_node_t n) {
    const char *p = mpack_node_str(n);
    size_t      l = mpack_node_strlen(n);
    fputc('"', stdout);
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n",  stdout); break;
            case '\r': fputs("\\r",  stdout); break;
            case '\t': fputs("\\t",  stdout); break;
            default:
                if (c < 0x20) printf("\\u%04x", c);
                else          fputc((int)c, stdout);
        }
    }
    fputc('"', stdout);
}

static void print_value(mpack_node_t n, int indent) {
    mpack_type_t t = mpack_node_type(n);
    switch (t) {
        case mpack_type_nil:    fputs("null",  stdout); return;
        case mpack_type_bool:
            fputs(mpack_node_bool(n) ? "true" : "false", stdout); return;
        case mpack_type_int:
            printf("%" PRId64, (int64_t)mpack_node_i64(n)); return;
        case mpack_type_uint:
            printf("%" PRIu64, (uint64_t)mpack_node_u64(n)); return;
        case mpack_type_float:
            printf("%g", (double)mpack_node_float(n)); return;
        case mpack_type_double:
            printf("%g", mpack_node_double(n)); return;
        case mpack_type_str:
            print_str(n); return;
        case mpack_type_bin: {
            size_t bl = mpack_node_bin_size(n);
            printf("\"<bin:%zu bytes>\"", bl); return;
        }
        case mpack_type_array: {
            size_t len = mpack_node_array_length(n);
            if (len == 0) { fputs("[]", stdout); return; }
            fputs("[\n", stdout);
            for (size_t i = 0; i < len; i++) {
                put_indent(indent + 1);
                print_value(mpack_node_array_at(n, i), indent + 1);
                fputs(i + 1 < len ? ",\n" : "\n", stdout);
            }
            put_indent(indent); fputc(']', stdout); return;
        }
        case mpack_type_map: {
            size_t len = mpack_node_map_count(n);
            if (len == 0) { fputs("{}", stdout); return; }
            fputs("{\n", stdout);
            for (size_t i = 0; i < len; i++) {
                put_indent(indent + 1);
                print_value(mpack_node_map_key_at(n, i), indent + 1);
                fputs(": ", stdout);
                print_value(mpack_node_map_value_at(n, i), indent + 1);
                fputs(i + 1 < len ? ",\n" : "\n", stdout);
            }
            put_indent(indent); fputc('}', stdout); return;
        }
        default:
            fputs("null", stdout); return;
    }
}

#define MG_CLI_MAX_KEYWORDS 64

static int usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  memgraph insert --summary S --detail D [--keyword K]...\n"
        "  memgraph query <text>\n"
        "  memgraph retrieve <text> [--top-k N]\n"
        "  memgraph explore <text> [--keyword K]... [--depth N] [--beam N]\n"
        "  memgraph get <hex_id>\n"
        "  memgraph classify --summary S\n"
        "  memgraph stats\n"
        "  memgraph consolidate\n");
    return 2;
}

/* -------------- per-op argument writers -------------- */

static int build_insert(int argc, char **argv, mpack_writer_t *w) {
    const char *summary = NULL, *detail = NULL;
    const char *kws[MG_CLI_MAX_KEYWORDS];
    int n_kws = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--summary") && i + 1 < argc) summary = argv[++i];
        else if (!strcmp(argv[i], "--detail") && i + 1 < argc) detail = argv[++i];
        else if (!strcmp(argv[i], "--keyword") && i + 1 < argc
                 && n_kws < MG_CLI_MAX_KEYWORDS) {
            kws[n_kws++] = argv[++i];
        }
    }
    mpack_start_map(w, 3);
    mpack_write_cstr(w, "summary"); mpack_write_cstr(w, summary ? summary : "");
    mpack_write_cstr(w, "detail");  mpack_write_cstr(w, detail  ? detail  : "");
    mpack_write_cstr(w, "keywords");
    mpack_start_array(w, (uint32_t)n_kws);
    for (int i = 0; i < n_kws; i++) mpack_write_cstr(w, kws[i]);
    mpack_finish_array(w);
    mpack_finish_map(w);
    return 0;
}

static int build_query(int argc, char **argv, mpack_writer_t *w) {
    const char *text = (argc >= 3) ? argv[2] : "";
    mpack_start_map(w, 1);
    mpack_write_cstr(w, "text"); mpack_write_cstr(w, text);
    mpack_finish_map(w);
    return 0;
}

static int build_retrieve(int argc, char **argv, mpack_writer_t *w) {
    const char *text = NULL;
    int top_k = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--top-k") && i + 1 < argc) top_k = atoi(argv[++i]);
        else if (!text) text = argv[i];
    }
    int n = 1 + (top_k > 0 ? 1 : 0);
    mpack_start_map(w, (uint32_t)n);
    mpack_write_cstr(w, "text"); mpack_write_cstr(w, text ? text : "");
    if (top_k > 0) {
        mpack_write_cstr(w, "top_k");
        mpack_write_int(w, top_k);
    }
    mpack_finish_map(w);
    return 0;
}

static int build_explore(int argc, char **argv, mpack_writer_t *w) {
    const char *text = NULL;
    const char *kws[MG_CLI_MAX_KEYWORDS];
    int n_kws = 0;
    int depth = 0, beam = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--keyword") && i + 1 < argc
            && n_kws < MG_CLI_MAX_KEYWORDS) {
            kws[n_kws++] = argv[++i];
        } else if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
            depth = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--beam") && i + 1 < argc) {
            beam = atoi(argv[++i]);
        } else if (!text) {
            text = argv[i];
        }
    }
    int n = 1
          + (n_kws > 0 ? 1 : 0)
          + (depth > 0 ? 1 : 0)
          + (beam  > 0 ? 1 : 0);
    mpack_start_map(w, (uint32_t)n);
    mpack_write_cstr(w, "text"); mpack_write_cstr(w, text ? text : "");
    if (n_kws > 0) {
        mpack_write_cstr(w, "keywords");
        mpack_start_array(w, (uint32_t)n_kws);
        for (int i = 0; i < n_kws; i++) mpack_write_cstr(w, kws[i]);
        mpack_finish_array(w);
    }
    if (depth > 0) { mpack_write_cstr(w, "depth");      mpack_write_int(w, depth); }
    if (beam  > 0) { mpack_write_cstr(w, "beam_width"); mpack_write_int(w, beam);  }
    mpack_finish_map(w);
    return 0;
}

static int build_get(int argc, char **argv, mpack_writer_t *w) {
    const char *id = (argc >= 3) ? argv[2] : "";
    mpack_start_map(w, 1);
    mpack_write_cstr(w, "id_hex"); mpack_write_cstr(w, id);
    mpack_finish_map(w);
    return 0;
}

static int build_classify(int argc, char **argv, mpack_writer_t *w) {
    const char *summary = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--summary") && i + 1 < argc) summary = argv[++i];
    }
    mpack_start_map(w, 1);
    mpack_write_cstr(w, "summary"); mpack_write_cstr(w, summary ? summary : "");
    mpack_finish_map(w);
    return 0;
}

static int build_empty(mpack_writer_t *w) {
    mpack_start_map(w, 0);
    mpack_finish_map(w);
    return 0;
}

/* -------------- main -------------- */

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];

    /* ---- build request ---- */
    char  *req     = NULL;
    size_t req_len = 0;
    mpack_writer_t w;
    mpack_writer_init_growable(&w, &req, &req_len);

    mpack_start_map(&w, 2);
    mpack_write_cstr(&w, "op");
    mpack_write_cstr(&w, cmd);
    mpack_write_cstr(&w, "args");

    int build_rc = -1;
    if      (!strcmp(cmd, "insert"))      build_rc = build_insert  (argc, argv, &w);
    else if (!strcmp(cmd, "query"))       build_rc = build_query   (argc, argv, &w);
    else if (!strcmp(cmd, "retrieve"))    build_rc = build_retrieve(argc, argv, &w);
    else if (!strcmp(cmd, "explore"))     build_rc = build_explore (argc, argv, &w);
    else if (!strcmp(cmd, "get"))         build_rc = build_get     (argc, argv, &w);
    else if (!strcmp(cmd, "classify"))    build_rc = build_classify(argc, argv, &w);
    else if (!strcmp(cmd, "stats"))       build_rc = build_empty   (&w);
    else if (!strcmp(cmd, "consolidate")) build_rc = build_empty   (&w);
    else {
        (void)mpack_writer_destroy(&w);
        free(req);
        return usage();
    }
    (void)build_rc;

    mpack_finish_map(&w);
    mpack_error_t we = mpack_writer_destroy(&w);
    if (we != mpack_ok) {
        fprintf(stderr, "request encode failed (mpack error %d)\n", (int)we);
        free(req);
        return 1;
    }

    /* ---- connect & exchange ---- */
    const char *sock_path = getenv("MEMGRAPH_SOCKET");
    if (!sock_path || !*sock_path) sock_path = "/tmp/memgraph.sock";

    int fd = -1;
    if (mg_daemon_socket_connect(sock_path, &fd) != MG_OK) {
        fprintf(stderr, "connect failed: %s\n", sock_path);
        free(req);
        mg_daemon_socket_shutdown();
        return 1;
    }

    if (mg_wire_write_frame(fd, req, req_len) != MG_OK) {
        fprintf(stderr, "send failed\n");
        mg_daemon_socket_close(fd);
        free(req);
        mg_daemon_socket_shutdown();
        return 1;
    }
    free(req);

    void  *resp     = NULL;
    size_t resp_len = 0;
    if (mg_wire_read_frame(fd, &resp, &resp_len) != MG_OK) {
        fprintf(stderr, "recv failed\n");
        mg_daemon_socket_close(fd);
        mg_daemon_socket_shutdown();
        return 1;
    }
    mg_daemon_socket_close(fd);
    mg_daemon_socket_shutdown();

    /* ---- parse and print ---- */
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, (const char *)resp, resp_len);
    mpack_tree_parse(&tree);
    int rc = 0;
    if (mpack_tree_error(&tree) != mpack_ok) {
        fprintf(stderr, "response decode error\n");
        rc = 1;
    } else {
        mpack_node_t root = mpack_tree_root(&tree);
        print_value(root, 0);
        printf("\n");
        /* Propagate non-zero status to exit code so scripts can check it. */
        mpack_node_t st = mpack_node_map_cstr_optional(root, "status");
        if (!mpack_node_is_missing(st) && !mpack_node_is_nil(st)) {
            int s = (int)mpack_node_int(st);
            if (s != 0) rc = 3;
        }
    }
    mpack_tree_destroy(&tree);
    free(resp);
    return rc;
}
