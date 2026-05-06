#include "memgraph/verify.h"

int mg_ce_try_enable(mg_verify_ctx_t *ctx) {
  (void)ctx;
  return -1;
}

int mg_ce_score_pair(mg_verify_ctx_t *ctx, const char *query, const char *candidate, float *out) {
  (void)ctx;
  (void)query;
  (void)candidate;
  if (out) {
    *out = -1.0f;
  }
  return -1;
}

void mg_ce_shutdown(mg_verify_ctx_t *ctx) {
  (void)ctx;
}
