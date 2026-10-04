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

#endif
