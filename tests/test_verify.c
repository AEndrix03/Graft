#include "graft/config.h"
#include "graft/verify.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int expect_int(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_verify: %s\n", msg);
    return 1;
  }
  return 0;
}

int main(void) {
  mg_config_t cfg;
  mg_verify_ctx_t *ctx = NULL;
  mg_verify_signals_t sig;
  int failures = 0;
  float high = mg_text_trigram_jaccard("ciao mondo", "ciao mondo!");
  float low = mg_text_trigram_jaccard("ciao mondo", "goodbye world");

  failures += expect_int(high > 0.6f, "high jaccard");
  failures += expect_int(low < 0.1f, "low jaccard");

  mg_config_defaults(&cfg);
  failures += expect_int(mg_verify_init(&cfg, &ctx) == MG_OK, "verify init");
  if (ctx) {
    failures += expect_int(mg_verify_score(ctx, "alpha beta", "alpha beta", 0.75f, 0.2f, &sig) == MG_OK,
                           "strong score call");
    failures += expect_int(sig.hit_level == MG_HIT_STRONG, "strong gate");
    failures += expect_int(isnan(sig.s_ce), "ce disabled");

    failures += expect_int(mg_verify_score(ctx, "alpha beta", "alpha gamma", 0.9f, 0.06f, &sig) == MG_OK,
                           "weak score call");
    failures += expect_int(sig.hit_level == MG_HIT_WEAK, "weak gate");

    failures += expect_int(mg_verify_score(ctx,
                                           "perche @Valid non si applica ricorsivamente in un attributo dto",
                                           "Spring Boot @Valid cascade on nested DTOs needs @Valid on the field plus @Validated on the controller",
                                           0.807f, 0.067f, &sig) == MG_OK,
                           "cross-language semantic score call");
    failures += expect_int(sig.hit_level == MG_HIT_STRONG, "cross-language semantic strong gate");

    failures += expect_int(mg_verify_score(ctx, "alpha beta", "delta gamma", 0.5f, 0.01f, &sig) == MG_OK,
                           "none score call");
    failures += expect_int(sig.hit_level == MG_HIT_NONE, "none gate");
  }

  mg_verify_shutdown(ctx);
  ctx = NULL;

  cfg.cross_encoder_enabled = true;
  free(cfg.cross_encoder_model_path);
  cfg.cross_encoder_model_path = NULL;
  failures += expect_int(mg_verify_init(&cfg, &ctx) == MG_OK,
                         "verify init with unavailable ce");
  if (ctx) {
    failures += expect_int(mg_verify_score(ctx, "alpha beta", "alpha beta", 0.75f, 0.2f, &sig) == MG_OK,
                           "score call with unavailable ce");
    failures += expect_int(sig.hit_level == MG_HIT_STRONG, "fallback gate with unavailable ce");
    failures += expect_int(isnan(sig.s_ce), "unavailable ce leaves signal disabled");
  }

  mg_verify_shutdown(ctx);
  ctx = NULL;
  mg_config_free(&cfg);

  /* Fused-gate mode: hit level decided by mg_score_fuse + thresholds, not by
   * the boolean lex/sem rules. With CE/NLI disabled the fused score collapses
   * to a weighted avg of s_vec and s_lex, which is enough to exercise the
   * branch end-to-end. */
  mg_config_defaults(&cfg);
  cfg.verify_use_fused_gate = true;
  cfg.verify_strong_min_fused = 0.70f;
  cfg.verify_weak_min_fused = 0.50f;
  /* Use only s_vec + s_lex (matches the default rerank weights). */
  cfg.rerank_w_vec = 0.30f;
  cfg.rerank_w_lex = 0.25f;
  cfg.rerank_w_ce  = 0.45f;
  cfg.rerank_w_nli = 0.0f;
  failures += expect_int(mg_verify_init(&cfg, &ctx) == MG_OK, "verify init fused-gate");
  if (ctx) {
    /* fused = (0.30*0.80 + 0.25*0.40) / (0.30+0.25) = 0.34/0.55 = 0.6181... */
    failures += expect_int(mg_verify_score(ctx, "q", "c", 0.80f, 0.40f, &sig) == MG_OK,
                           "fused-gate score call (above weak floor)");
    failures += expect_int(sig.hit_level == MG_HIT_WEAK,
                           "fused-gate yields WEAK at ~0.62 between 0.50 and 0.70");

    /* fused = (0.30*0.95 + 0.25*0.85) / 0.55 = (0.285+0.2125)/0.55 = 0.9045... */
    failures += expect_int(mg_verify_score(ctx, "q", "c", 0.95f, 0.85f, &sig) == MG_OK,
                           "fused-gate strong call");
    failures += expect_int(sig.hit_level == MG_HIT_STRONG,
                           "fused-gate yields STRONG when fused >= strong_min_fused");

    /* fused = (0.30*0.30 + 0.25*0.05) / 0.55 = (0.09+0.0125)/0.55 = 0.1863... */
    failures += expect_int(mg_verify_score(ctx, "q", "c", 0.30f, 0.05f, &sig) == MG_OK,
                           "fused-gate weak floor call");
    failures += expect_int(sig.hit_level == MG_HIT_NONE,
                           "fused-gate yields NONE below weak_min_fused");
  }

  mg_verify_shutdown(ctx);
  mg_config_free(&cfg);
  return failures == 0 ? 0 : 1;
}
