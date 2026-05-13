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

#include "graft/http.h"
#include "internal.h"
#include "graft/error.h"

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

/* Constant-time string comparison. Returns 1 if both strings are non-NULL,
 * have the same length, and every byte matches. Defends against timing
 * oracles that would otherwise leak the prefix of the configured token. */
static int mg_const_time_eq(const char *a, const char *b) {
  size_t la, lb, i;
  unsigned char diff = 0;
  if (!a || !b) return 0;
  la = strlen(a);
  lb = strlen(b);
  diff |= (unsigned char)((la ^ lb) | ((la ^ lb) >> 8));
  /* Walk min(la, lb) bytes regardless of mismatch so the loop's runtime
   * depends only on the input lengths, not on where the first mismatch is. */
  {
    size_t n = la < lb ? la : lb;
    for (i = 0; i < n; ++i) {
      diff |= (unsigned char)(a[i] ^ b[i]);
    }
  }
  return diff == 0;
}

/* Resolve the effective auth token. Env GRAFT_HTTP_AUTH_TOKEN wins over the
 * config field so secrets can stay out of YAML. Returns NULL/empty when no
 * token is configured at all (auth disabled). */
static const char *mg_http_effective_token(const mg_ctx_t *ctx) {
  const char *env = getenv("GRAFT_HTTP_AUTH_TOKEN");
  if (env && *env) return env;
  if (ctx->config->http_auth_token && *ctx->config->http_auth_token) {
    return ctx->config->http_auth_token;
  }
  return NULL;
}

/* Check Authorization header for "Bearer <token>" matching the configured
 * value. Returns 1 if auth is satisfied (or not required), 0 otherwise. */
static int mg_http_auth_ok(const mg_ctx_t *ctx, const mg_http_request_t *req) {
  const char *want = mg_http_effective_token(ctx);
  const char *got;
  if (!want) return 1;  /* auth disabled */
  got = mg_http_header_get(req, "Authorization");
  if (!got) return 0;
  /* Skip the "Bearer " prefix (case-insensitive). */
#ifdef _WIN32
  if (_strnicmp(got, "Bearer ", 7) != 0) return 0;
#else
  if (strncasecmp(got, "Bearer ", 7) != 0) return 0;
#endif
  got += 7;
  while (*got == ' ' || *got == '\t') ++got;
  return mg_const_time_eq(got, want);
}

/* DNS-rebinding defense: only accept Host: headers naming the loopback
 * interface (or the explicitly allow-listed external bind). Block attacker
 * pages that point a public DNS name at our local server.
 *
 * Accepts: "127.0.0.1", "127.0.0.1:9977", "[::1]", "[::1]:9977", "localhost",
 * "localhost:9977". When http_allow_remote is true, defers to whatever
 * value http_bind names (caller-trusted). */
static int mg_host_header_ok(const mg_ctx_t *ctx, const char *host) {
  if (!host || !*host) return 0;

  /* Strip an optional :port suffix for the comparison, but only outside of
   * the bracketed-v6 form to keep "[::1]:9977" intact. */
  char hbuf[256];
  size_t n = strlen(host);
  if (n >= sizeof(hbuf)) return 0;
  memcpy(hbuf, host, n + 1);

  char *trim = hbuf;
  while (*trim == ' ' || *trim == '\t') trim++;
  size_t tn = strlen(trim);
  while (tn > 0 && (trim[tn - 1] == ' ' || trim[tn - 1] == '\t')) trim[--tn] = '\0';

  /* For non-bracketed forms, drop ":port". For bracketed ([::1]:port),
   * keep everything up to and including the closing bracket. */
  if (trim[0] == '[') {
    char *rb = strchr(trim, ']');
    if (rb) *(rb + 1) = '\0';
  } else {
    char *colon = strrchr(trim, ':');
    if (colon) *colon = '\0';
  }

  if (strcmp(trim, "127.0.0.1") == 0) return 1;
  if (strcmp(trim, "localhost") == 0) return 1;
  if (strcmp(trim, "[::1]") == 0) return 1;
  if (strcmp(trim, "::1") == 0) return 1;

  /* If the operator opted into remote bind, also accept the configured
   * bind address as a valid Host. */
  if (ctx && ctx->config && ctx->config->http_allow_remote
      && ctx->config->http_bind && *ctx->config->http_bind
      && strcmp(trim, ctx->config->http_bind) == 0) {
    return 1;
  }

  return 0;
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

  /* DNS rebinding guard — must precede any routing (static or API). */
  {
    const char *host = mg_http_header_get(&req, "Host");
    if (!mg_host_header_ok(ctx, host)) {
      mg_http_error(&resp, 403, "forbidden host header");
      goto send;
    }
  }

  /* Static SPA bundle is served by the local-first daemon. Public production
   * deployments should place the OAuth gateway in front of /v1/* instead.
   * Static paths are NOT auth-gated so the viewer HTML can boot; data fetches
   * to /v1/* are gated below. */
  if (mg_http_try_static(ctx, &req, &resp)) goto send;

  /* Bearer token gate for the API surface. When `http.auth_token` (or env
   * GRAFT_HTTP_AUTH_TOKEN) is set, every /v1/* request must carry
   * `Authorization: Bearer <token>`. Constant-time compare avoids timing
   * oracles. 401 + WWW-Authenticate signals the auth scheme to clients. */
  if (req.path && strncmp(req.path, "/v1/", 4) == 0) {
    if (!mg_http_auth_ok(ctx, &req)) {
      mg_http_error(&resp, 401, "unauthorized");
      /* Advertise the bearer scheme so well-behaved clients prompt for a
       * token. extra_headers stores borrowed string literals — safe to
       * assign without strdup. */
      if (resp.n_extra_headers < 8) {
        resp.extra_headers[resp.n_extra_headers * 2]     = (char *)"WWW-Authenticate";
        resp.extra_headers[resp.n_extra_headers * 2 + 1] = (char *)"Bearer realm=\"graft\"";
        resp.n_extra_headers++;
      }
      goto send;
    }
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

  /* Refuse non-loopback binds unless the operator has explicitly opted in
   * via `http.allow_remote: true`. The HTTP server ships with no auth, so
   * binding to a public interface would expose unauthenticated access to
   * the entire graph. localhost / 127.0.0.0/8 / ::1 are always allowed. */
  {
    const char *bind_str = (ctx->config->http_bind && *ctx->config->http_bind)
                           ? ctx->config->http_bind : "127.0.0.1";
    unsigned long ip = ntohl(addr.sin_addr.s_addr);
    int is_loopback = ((ip & 0xFF000000UL) == 0x7F000000UL);  /* 127.0.0.0/8 */
    int is_named_local = (strcmp(bind_str, "localhost") == 0);
    if (!is_loopback && !is_named_local && !ctx->config->http_allow_remote) {
      fprintf(stderr,
              "http: refusing to bind non-loopback address %s — set "
              "`http.allow_remote: true` in config.yaml to override "
              "(WARNING: server has no auth).\n",
              bind_str);
      MG_CLOSE_SOCK(s);
      return MG_ERR_CONFIG;
    }
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

  /* Operator-visible banner. The HTTP server has no native auth: anything
   * that reaches bind:port can call /v1/*. Surface the exact attack surface
   * so misconfiguration is loud, not silent. */
  {
    const char *bind_str = (ctx->config->http_bind && *ctx->config->http_bind)
                           ? ctx->config->http_bind : "127.0.0.1";
    const char *auth_state = mg_http_effective_token(ctx)
                             ? "bearer token required"
                             : "NO AUTH";
    fprintf(stderr, "\n");
    fprintf(stderr, "================ graft HTTP API ================\n");
    fprintf(stderr, "  listening on http://%s:%d  (plaintext, %s)\n",
            bind_str, ctx->config->http_port, auth_state);
    if (ctx->config->http_allow_remote && !mg_http_effective_token(ctx)) {
      fprintf(stderr, "  ALLOW_REMOTE: enabled with NO AUTH — bind is NOT loopback.\n");
      fprintf(stderr, "  Put an authenticating reverse proxy in front OR set\n");
      fprintf(stderr, "  http.auth_token (or env GRAFT_HTTP_AUTH_TOKEN), or the\n");
      fprintf(stderr, "  entire memory graph is exposed to the network.\n");
    } else if (ctx->config->http_allow_remote) {
      fprintf(stderr, "  ALLOW_REMOTE: enabled — connections must carry the\n");
      fprintf(stderr, "  bearer token. Still plaintext: terminate TLS upstream.\n");
    }
    fprintf(stderr, "  enabled endpoints (/v1/* gated by auth when token set):\n");
    if (ctx->config->http_ep_match)    fprintf(stderr, "    GET    /v1/match     (read)\n");
    if (ctx->config->http_ep_search)   fprintf(stderr, "    GET    /v1/search    (read)\n");
    if (ctx->config->http_ep_explore)  fprintf(stderr, "    GET    /v1/explore   (read)\n");
    if (ctx->config->http_ep_classify) fprintf(stderr, "    GET    /v1/classify  (read)\n");
    if (ctx->config->http_ep_view)     fprintf(stderr, "    GET    /v1/view      (read: FULL GRAPH DUMP)\n");
    if (ctx->config->http_ep_insert)   fprintf(stderr, "    POST   /v1/insert    (WRITE)\n");
    if (ctx->config->http_ep_delete)   fprintf(stderr, "    DELETE /v1/nodes/*   (WRITE)\n");
    fprintf(stderr, "  Disable any endpoint you do not need in config.yaml.\n");
    fprintf(stderr, "================================================\n\n");
  }

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
