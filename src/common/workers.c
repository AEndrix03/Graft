/* Worker group: see graft/workers.h. */

#include "graft/workers.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <sys/socket.h>
#endif

struct mg_workers {
  pthread_mutex_t mu;
  pthread_cond_t  idle;      /* signalled when the count drops to zero */
  int            *fds;       /* sockets of the running workers */
  int             n;
  int             cap;
  int             draining;
};

mg_workers_t *mg_workers_new(void) {
  mg_workers_t *g = (mg_workers_t *)calloc(1, sizeof(*g));
  if (!g) return NULL;
  pthread_mutex_init(&g->mu, NULL);
  pthread_cond_init(&g->idle, NULL);
  return g;
}

void mg_workers_free(mg_workers_t *g) {
  if (!g) return;
  pthread_cond_destroy(&g->idle);
  pthread_mutex_destroy(&g->mu);
  free(g->fds);
  free(g);
}

int mg_workers_enter(mg_workers_t *g, int fd) {
  int rc = -1;
  pthread_mutex_lock(&g->mu);
  if (!g->draining) {
    if (g->n == g->cap) {
      int cap = g->cap ? g->cap * 2 : 16;
      int *fds = (int *)realloc(g->fds, (size_t)cap * sizeof(*fds));
      if (fds) { g->fds = fds; g->cap = cap; }
    }
    if (g->n < g->cap) {
      g->fds[g->n++] = fd;
      rc = 0;
    }
  }
  pthread_mutex_unlock(&g->mu);
  return rc;
}

void mg_workers_leave(mg_workers_t *g, int fd) {
  pthread_mutex_lock(&g->mu);
  for (int i = 0; i < g->n; i++) {
    if (g->fds[i] == fd) {
      g->fds[i] = g->fds[--g->n];
      break;
    }
  }
  if (g->n == 0) pthread_cond_broadcast(&g->idle);
  pthread_mutex_unlock(&g->mu);
}

int mg_workers_active(mg_workers_t *g) {
  pthread_mutex_lock(&g->mu);
  int n = g->n;
  pthread_mutex_unlock(&g->mu);
  return n;
}

/* Make a worker blocked on fd (an idle client, a half-sent request, a slow
 * reader) return from recv()/send() now, and fail any later call on it.
 * shutdown(), not close(): the fd stays valid until its worker has left the
 * group, so its number cannot be reused under a worker still using it. On
 * Linux and macOS that wakes a blocked recv(); Windows only fails the calls
 * made afterwards, so a recv() already blocked is cancelled with CancelIoEx
 * (Winsock runs blocking calls as overlapped I/O on the socket handle). */
static void wake_sock(int fd) {
  if (fd < 0) return;
#ifdef _WIN32
  (void)shutdown((SOCKET)fd, SD_BOTH);
  (void)CancelIoEx((HANDLE)(SOCKET)fd, NULL);
#else
  (void)shutdown(fd, SHUT_RDWR);
#endif
}

static void add_ms(struct timespec *ts, int ms) {
  ts->tv_sec  += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

int mg_workers_drain(mg_workers_t *g, int timeout_ms) {
  struct timespec now, deadline, slice;
  clock_gettime(CLOCK_REALTIME, &now);
  deadline = now;
  add_ms(&deadline, timeout_ms);

  pthread_mutex_lock(&g->mu);
  g->draining = 1;
  while (g->n > 0) {
    /* Re-wake every 100 ms: a worker caught between two calls when the
     * previous round ran may have blocked again since. */
    for (int i = 0; i < g->n; i++) wake_sock(g->fds[i]);
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
    slice = now;
    add_ms(&slice, 100);
    if (slice.tv_sec > deadline.tv_sec ||
        (slice.tv_sec == deadline.tv_sec && slice.tv_nsec > deadline.tv_nsec)) {
      slice = deadline;
    }
    (void)pthread_cond_timedwait(&g->idle, &g->mu, &slice);
  }
  int left = g->n;
  pthread_mutex_unlock(&g->mu);
  return left;
}
