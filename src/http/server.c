/* HTTP server: TCP accept loop in a background thread.
 *
 * Lifecycle:
 *   mg_http_start(ctx) — binds, listens, spawns worker. Returns a handle.
 *   mg_http_stop(srv)  — flips shutdown flag, closes the listening socket
 *                        (which makes accept() return), joins the worker.
 *
 * One thread per connection (via pthread_create + detach). Acceptable for a
 * local-first inspection tool; a real high-concurrency server would use a
 * thread pool or epoll/kqueue. We keep it boring.
 */

#include "memgraph/http.h"
#include "internal.h"
#include "memgraph/error.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  define MG_INVALID_SOCK INVALID_SOCKET
#  define MG_CLOSE_SOCK(s) closesocket((SOCKET)(s))
typedef SOCKET mg_sock_t;
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <errno.h>
#  define MG_INVALID_SOCK (-1)
#  define MG_CLOSE_SOCK(s) close((s))
typedef int mg_sock_t;
#endif

struct mg_http_server {
  mg_ctx_t  *ctx;
  int        listen_fd;
  pthread_t  thread;
  volatile int shutdown;
};

typedef struct {
  mg_http_server_t *srv;
  int               fd;
} client_arg_t;

/* Look up a route. Returns the handler or NULL. Also writes the success
 * status hint into *out_status (used for 201 on POST insert). */
static mg_http_handler_fn route(const char *method, const char *path) {
  if (!method || !path) return NULL;

  if (strcmp(method, "GET") == 0) {
    if (strcmp(path, "/v1/healthz")  == 0) return mg_http_handler_healthz;
    if (strcmp(path, "/v1/match")    == 0) return mg_http_handler_match;
    if (strcmp(path, "/v1/search")   == 0) return mg_http_handler_search;
    if (strcmp(path, "/v1/explore")  == 0) return mg_http_handler_explore;
    if (strcmp(path, "/v1/classify") == 0) return mg_http_handler_classify;
    if (strcmp(path, "/v1/view")     == 0) return mg_http_handler_view;
    if (strncmp(path, "/v1/nodes/", 10) == 0) return mg_http_handler_get;
  } else if (strcmp(method, "POST") == 0) {
    if (strcmp(path, "/v1/insert") == 0) return mg_http_handler_insert;
  } else if (strcmp(method, "DELETE") == 0) {
    if (strncmp(path, "/v1/nodes/", 10) == 0) return mg_http_handler_delete;
  }
  return NULL;
}

static void *handle_client(void *arg) {
  client_arg_t *ca = (client_arg_t *)arg;
  mg_ctx_t *ctx = ca->srv->ctx;
  int fd = ca->fd;
  mg_http_request_t req;
  mg_http_response_t resp;
  mg_http_handler_fn h;
  free(ca);

  mg_http_response_init(&resp);

  if (mg_http_parse_request(fd, &req) != MG_OK) {
    mg_http_error(&resp, 400, "bad request");
    goto send;
  }

  /* Static SPA bundle is served unauthenticated — it's just HTML/JS/CSS
   * and the SPA itself attaches the bearer token to its own /v1/* fetches.
   * Try this first so /, /assets/* etc. don't require a key. */
  if (mg_http_try_static(ctx, &req, &resp)) goto send;

  /* Auth check applies to everything except /v1/healthz so liveness probes
   * don't need credentials. */
  if (strcmp(req.path ? req.path : "", "/v1/healthz") != 0
      && !mg_http_auth_ok(ctx, &req)) {
    mg_http_error(&resp, 401, "unauthorized");
    goto send;
  }

  h = route(req.method, req.path);
  if (!h) {
    mg_http_error(&resp, 404, "not found");
    goto send;
  }
  h(ctx, &req, &resp);

send:
  (void)mg_http_send_response(fd, &resp);
  mg_http_response_free(&resp);
  mg_http_request_free(&req);
  MG_CLOSE_SOCK((mg_sock_t)fd);
  return NULL;
}

static void *server_thread(void *arg) {
  mg_http_server_t *srv = (mg_http_server_t *)arg;
  while (!srv->shutdown) {
    struct sockaddr_in client_addr;
#ifdef _WIN32
    int caddrlen = sizeof(client_addr);
#else
    socklen_t caddrlen = sizeof(client_addr);
#endif
    mg_sock_t cfd = accept((mg_sock_t)srv->listen_fd,
                           (struct sockaddr *)&client_addr, &caddrlen);
    if (cfd == MG_INVALID_SOCK) {
      if (srv->shutdown) break;
      continue;
    }

    client_arg_t *ca = (client_arg_t *)malloc(sizeof(*ca));
    if (!ca) { MG_CLOSE_SOCK(cfd); continue; }
    ca->srv = srv;
    ca->fd  = (int)cfd;

    pthread_t th;
    if (pthread_create(&th, NULL, handle_client, ca) != 0) {
      free(ca);
      MG_CLOSE_SOCK(cfd);
      continue;
    }
    pthread_detach(th);
  }
  return NULL;
}

mg_err_t mg_http_start(mg_ctx_t *ctx, mg_http_server_t **out) {
  mg_http_server_t *srv;
  struct sockaddr_in addr;
  mg_sock_t s;
  int yes = 1;

  if (!ctx || !ctx->config || !out) return MG_ERR_INVALID_ARG;
  *out = NULL;
  if (!ctx->config->http_enabled) return MG_OK;  /* no-op when disabled */

#ifdef _WIN32
  {
    static int wsa_inited = 0;
    if (!wsa_inited) {
      WSADATA wd;
      (void)WSAStartup(MAKEWORD(2, 2), &wd);
      wsa_inited = 1;
    }
  }
#endif

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == MG_INVALID_SOCK) return MG_ERR_IO;

  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)ctx->config->http_port);
  if (ctx->config->http_bind && *ctx->config->http_bind) {
    addr.sin_addr.s_addr = inet_addr(ctx->config->http_bind);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
      MG_CLOSE_SOCK(s);
      return MG_ERR_CONFIG;
    }
  } else {
    addr.sin_addr.s_addr = htonl(0x7F000001);  /* 127.0.0.1 — safe default */
  }

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "http: bind failed on %s:%d\n",
            ctx->config->http_bind, ctx->config->http_port);
    MG_CLOSE_SOCK(s);
    return MG_ERR_IO;
  }
  if (listen(s, 16) != 0) {
    MG_CLOSE_SOCK(s);
    return MG_ERR_IO;
  }

  srv = (mg_http_server_t *)calloc(1, sizeof(*srv));
  if (!srv) { MG_CLOSE_SOCK(s); return MG_ERR_OOM; }
  srv->ctx = ctx;
  srv->listen_fd = (int)s;

  if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
    MG_CLOSE_SOCK(s);
    free(srv);
    return MG_ERR_INTERNAL;
  }

  fprintf(stderr, "http: listening on %s:%d%s\n",
          ctx->config->http_bind, ctx->config->http_port,
          (ctx->config->http_api_key && *ctx->config->http_api_key)
            ? " (auth: bearer)" : " (auth: none)");

  *out = srv;
  return MG_OK;
}

void mg_http_stop(mg_http_server_t *srv) {
  if (!srv) return;
  srv->shutdown = 1;
  if (srv->listen_fd >= 0) {
    MG_CLOSE_SOCK((mg_sock_t)srv->listen_fd);
    srv->listen_fd = -1;
  }
  pthread_join(srv->thread, NULL);
  free(srv);
}
