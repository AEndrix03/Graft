#ifndef GRAFT_CLI_CLIENT_H
#define GRAFT_CLI_CLIENT_H

#include "mpack.h"

#include <stddef.h>

/* JSON-ish pretty-printer for mpack nodes: what every daemon command prints
 * on stdout. mpack's own print helpers are gated on MPACK_DEBUG (off in
 * release), so we roll our own. */
void mg_cli_print_value(mpack_node_t n, int indent);

/* Sends one request frame to the daemon at GRAFT_SOCKET (default
 * /tmp/graft.sock), auto-starting it when it is down, and returns the
 * response frame in *resp (free() it). Returns 0, or 1 after printing the
 * failure on stderr. */
int mg_cli_exchange(const char *req, size_t req_len, void **resp, size_t *resp_len);

/* Sends {op, args} (args: one encoded mpack map) and parses the reply into
 * *tree, backed by *resp (free() it after mpack_tree_destroy). A daemon
 * error is printed as the usual envelope. Returns 0 ok, 1 transport
 * failure, 3 daemon error. */
int mg_cli_call(const char *op, const char *args, size_t args_len,
                mpack_tree_t *tree, void **resp);

/* Prints a locally built {status, result} envelope like a daemon reply. */
int mg_cli_print_built(char *buf, size_t len);

#endif
