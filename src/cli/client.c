/* Shared CLI plumbing: response printing and the request/response exchange
 * with the daemon (see client.h). */

#include "client.h"
#include "autostart.h"
#include "../daemon/internal.h"
#include "graft/wire.h"
#include "graft/error.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

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

void mg_cli_print_value(mpack_node_t n, int indent) {
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
                mg_cli_print_value(mpack_node_array_at(n, i), indent + 1);
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
                mg_cli_print_value(mpack_node_map_key_at(n, i), indent + 1);
                fputs(": ", stdout);
                mg_cli_print_value(mpack_node_map_value_at(n, i), indent + 1);
                fputs(i + 1 < len ? ",\n" : "\n", stdout);
            }
            put_indent(indent); fputc('}', stdout); return;
        }
        default:
            fputs("null", stdout); return;
    }
}

int mg_cli_exchange(const char *req, size_t req_len, void **resp, size_t *resp_len) {
    const char *sock_path = getenv("GRAFT_SOCKET");
    if (!sock_path || !*sock_path) sock_path = "/tmp/graft.sock";

    *resp = NULL;
    *resp_len = 0;

    int fd = -1;
    if (mg_daemon_socket_connect(sock_path, &fd) != MG_OK) {
        /* Daemon down — try to spawn it next to this binary, then retry once.
         * This pays a one-time cost (~1-2s) on the first command of a session
         * and saves the user from having to start the daemon manually. */
        /* Roomy: the message quotes the tail of the daemon log. */
        char ae[2048] = { 0 };
        if (mg_autostart_daemon(sock_path, ae, sizeof(ae)) != MG_OK) {
            fprintf(stderr, "connect failed: %s\nauto-start: %s\n", sock_path, ae);
            mg_daemon_socket_shutdown();
            return 1;
        }
        if (mg_daemon_socket_connect(sock_path, &fd) != MG_OK) {
            fprintf(stderr, "connect failed after auto-start: %s\n", sock_path);
            mg_daemon_socket_shutdown();
            return 1;
        }
    }

    if (mg_wire_write_frame(fd, req, req_len) != MG_OK) {
        fprintf(stderr, "send failed\n");
        mg_daemon_socket_close(fd);
        mg_daemon_socket_shutdown();
        return 1;
    }

    if (mg_wire_read_frame(fd, resp, resp_len) != MG_OK) {
        fprintf(stderr, "recv failed\n");
        mg_daemon_socket_close(fd);
        mg_daemon_socket_shutdown();
        return 1;
    }
    mg_daemon_socket_close(fd);
    mg_daemon_socket_shutdown();
    return 0;
}
