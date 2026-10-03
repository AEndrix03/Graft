/* Shutdown must wait for the per-connection worker threads before freeing
 * the state they use (#8). Covers the worker group shared by the daemon and
 * the HTTP server, then stops a live HTTP server while one client idles on
 * an open connection and another is stuck halfway through its request:
 * mg_http_stop() must wake both, wait for their threads and return promptly,
 * not free the server under them and not hang. */

#include "graft/config.h"
#include "graft/http.h"
#include "graft/workers.h"
#include "../src/http/internal.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  define MG_CLOSE_SOCK(s) closesocket(s)
typedef SOCKET mg_sock_t;
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  define INVALID_SOCKET (-1)
#  define MG_CLOSE_SOCK(s) close(s)
typedef int mg_sock_t;
#endif

static int g_failures = 0;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_workers_drain: %s\n", msg);
    g_failures++;
  }
}

static void sleep_ms(int ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
#endif
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---- worker group ---------------------------------------------------- */

typedef struct {
  mg_workers_t *g;
  volatile int  done;
} slow_arg_t;

/* Stands in for a request that is busy (an embedding, say) when shutdown
 * starts: it has no socket to be woken through, so the drain must wait. */
static void *slow_worker(void *vp) {
  slow_arg_t *a = (slow_arg_t *)vp;
  sleep_ms(300);
  a->done = 1;
  mg_workers_leave(a->g, -1);
  return NULL;
}

static void test_group(void) {
  mg_workers_t *g = mg_workers_new();
  slow_arg_t a;
  pthread_t th;
  long t0;
  int left;

  expect(g != NULL, "mg_workers_new");
  if (!g) return;

  /* An in-flight worker is waited for. */
  a.g = g;
  a.done = 0;
  expect(mg_workers_enter(g, -1) == 0, "enter before the drain");
  expect(mg_workers_active(g) == 1, "one active worker");
  pthread_create(&th, NULL, slow_worker, &a);
  left = mg_workers_drain(g, 10000);
  expect(left == 0, "drain waits for the in-flight worker");
  expect(a.done, "the worker finished before the drain returned");
  pthread_join(th, NULL);

  /* A draining group takes no new workers. */
  expect(mg_workers_enter(g, -1) != 0, "enter is refused once draining");
  mg_workers_free(g);

  /* A worker that never finishes cannot hang shutdown: the drain gives up
   * at the deadline and reports it, so the caller keeps the state alive. */
  g = mg_workers_new();
  expect(mg_workers_enter(g, -1) == 0, "enter a stuck worker");
  t0 = now_ms();
  left = mg_workers_drain(g, 200);
  expect(left == 1, "drain reports the stuck worker");
  expect(now_ms() - t0 < 5000, "drain honours its timeout");
  mg_workers_leave(g, -1);
  mg_workers_free(g);
}

/* ---- HTTP server ------------------------------------------------------ */

static mg_sock_t connect_to(int port) {
  struct sockaddr_in sa;
  mg_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return s;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  sa.sin_addr.s_addr = htonl(0x7F000001UL);
  if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    MG_CLOSE_SOCK(s);
    return INVALID_SOCKET;
  }
  return s;
}

static void test_http_stop(void) {
  mg_config_t cfg;
  mg_ctx_t ctx;
  mg_http_server_t *srv = NULL;
  mg_sock_t idle, partial;
  const char *half = "GET /v1/healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  int port;
  long t0;
  bool stopped;

  mg_config_defaults(&cfg);
  cfg.http_enabled = true;
  cfg.http_port = 0;
  memset(&ctx, 0, sizeof(ctx));
  ctx.config = &cfg;

  expect(mg_http_start(&ctx, &srv) == MG_OK && srv, "http server starts");
  if (!srv) { mg_config_free(&cfg); return; }
  port = mg_http_server_port(srv);
  expect(port > 0, "bound port is known");

  idle = connect_to(port);
  partial = connect_to(port);
  expect(idle != INVALID_SOCKET && partial != INVALID_SOCKET, "clients connect");
  if (partial != INVALID_SOCKET) {
    (void)send(partial, half, (int)strlen(half), 0);
  }

  /* Both workers must be parked in recv() before the stop. */
  t0 = now_ms();
  while (mg_http_server_active_clients(srv) < 2 && now_ms() - t0 < 10000) {
    sleep_ms(10);
  }
  expect(mg_http_server_active_clients(srv) == 2, "two client threads running");

  t0 = now_ms();
  stopped = mg_http_stop(srv);
  expect(stopped, "stop drained every client thread before freeing srv");
  expect(now_ms() - t0 < 10000, "stop does not wait on idle clients");

  if (idle != INVALID_SOCKET) MG_CLOSE_SOCK(idle);
  if (partial != INVALID_SOCKET) MG_CLOSE_SOCK(partial);
  mg_config_free(&cfg);
}

int main(void) {
#ifndef _WIN32
  /* A worker answering a shut-down socket must not kill the test. */
  signal(SIGPIPE, SIG_IGN);
#endif
  test_group();
  test_http_stop();
  if (g_failures) {
    fprintf(stderr, "test_workers_drain: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok workers drain\n");
  return 0;
}
