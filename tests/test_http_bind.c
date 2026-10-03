/* HTTP bind addresses: the loopback forms documented as valid local binds
 * (127.0.0.1, localhost, ::1) must start the server, and a non-loopback
 * address must be refused unless http.allow_remote is set. Port 0 lets the
 * OS pick a free port, so the test never collides with a running daemon. */

#include "graft/config.h"
#include "graft/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_http_bind: %s\n", msg);
    g_failures++;
  }
}

static void set_bind(mg_config_t *cfg, const char *addr) {
  free(cfg->http_bind);
  cfg->http_bind = (char *)malloc(strlen(addr) + 1u);
  strcpy(cfg->http_bind, addr);
}

static mg_err_t try_bind(mg_ctx_t *ctx, const char *addr) {
  mg_http_server_t *srv = NULL;
  mg_err_t err;
  set_bind(ctx->config, addr);
  err = mg_http_start(ctx, &srv);
  mg_http_stop(srv);
  return err;
}

int main(void) {
  mg_config_t cfg;
  mg_ctx_t ctx;
  mg_err_t err;

  mg_config_defaults(&cfg);
  cfg.http_enabled = true;
  cfg.http_port = 0;
  memset(&ctx, 0, sizeof(ctx));
  ctx.config = &cfg;

  expect(try_bind(&ctx, "127.0.0.1") == MG_OK, "127.0.0.1 binds");
  expect(try_bind(&ctx, "localhost") == MG_OK, "localhost binds");

  /* A host without IPv6 cannot bind ::1 (MG_ERR_IO); what must never happen
   * again is rejecting the address itself as invalid configuration. */
  err = try_bind(&ctx, "::1");
  expect(err == MG_OK || err == MG_ERR_IO, "::1 is accepted as a bind address");
  if (err != MG_OK) printf("skip ::1 (no IPv6 loopback on this host)\n");

  cfg.http_allow_remote = false;
  expect(try_bind(&ctx, "0.0.0.0") == MG_ERR_CONFIG,
         "a non-loopback bind is refused without allow_remote");
  expect(try_bind(&ctx, "::") == MG_ERR_CONFIG,
         "the IPv6 wildcard is refused without allow_remote");

  mg_config_free(&cfg);
  if (g_failures) {
    fprintf(stderr, "test_http_bind: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok http bind\n");
  return 0;
}
