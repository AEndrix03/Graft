#ifndef GRAFT_WORKERS_H
#define GRAFT_WORKERS_H

/* Lifetime tracking for per-connection worker threads.
 *
 * The daemon and the HTTP server run one detached thread per client, and
 * those threads use state their listener owns (the mg_ctx_t resources, the
 * HTTP server handle). The owner must not free that state while any worker
 * is still running, so a group counts the live workers and remembers the
 * socket each one serves:
 *
 *   accept loop:  mg_workers_enter(g, fd) before starting the thread; on
 *                 failure close fd and do not start it.
 *   worker:       mg_workers_leave(g, fd) after its last use of shared
 *                 state, then close fd.
 *   shutdown:     stop accepting, then mg_workers_drain(g, timeout_ms).
 *
 * Draining refuses new workers, shuts down every registered socket so a
 * worker blocked in recv()/send() on an idle or slow client returns at once,
 * and waits for the count to reach zero. A worker busy inside a request (an
 * embedding, a rerank) is waited for, up to the timeout. */

/* How long shutdown waits for in-flight requests before giving up. */
#define MG_WORKERS_DRAIN_TIMEOUT_MS 30000

typedef struct mg_workers mg_workers_t;

/* NULL on OOM. */
mg_workers_t *mg_workers_new(void);

/* Only once no worker is running (after a drain that returned 0). */
void mg_workers_free(mg_workers_t *g);

/* Register a worker serving fd. Returns 0, or -1 when the group is
 * draining (or on OOM): the caller then closes fd itself. */
int mg_workers_enter(mg_workers_t *g, int fd);

/* Unregister the worker serving fd. After this call the group never touches
 * fd again, so the worker may close it. */
void mg_workers_leave(mg_workers_t *g, int fd);

/* Number of registered workers. */
int mg_workers_active(mg_workers_t *g);

/* Refuse new workers, shut down their sockets and wait up to timeout_ms
 * for all of them to leave. Returns how many are still running: 0 means the
 * shared state can be freed, anything else means it must be leaked. */
int mg_workers_drain(mg_workers_t *g, int timeout_ms);

#endif
