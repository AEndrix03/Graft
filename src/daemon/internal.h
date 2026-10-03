#ifndef MG_DAEMON_INTERNAL_H
#define MG_DAEMON_INTERNAL_H

#include "graft/error.h"
#include <stddef.h>

/* Cross-platform AF_UNIX socket helpers shared by the daemon and the CLI. */

void     mg_daemon_socket_init(void);     /* WSAStartup on Windows; no-op elsewhere */
void     mg_daemon_socket_shutdown(void); /* WSACleanup on Windows; no-op elsewhere */

int      mg_daemon_socket_listen(const char *path);
int      mg_daemon_socket_accept(int listen_fd);
/* Wait up to timeout_ms for listen_fd to have a pending connection.
 * 1 = ready, 0 = timed out or interrupted by a signal, -1 = error. */
int      mg_daemon_socket_poll(int listen_fd, int timeout_ms);
mg_err_t mg_daemon_socket_connect(const char *path, int *out_fd);
void     mg_daemon_socket_close(int fd);

#endif
