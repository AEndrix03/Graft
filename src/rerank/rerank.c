/* Second-stage reranker on top of RRF retrieval.
 *
 * The reranker re-scores the top-N RRF candidates using a weighted fusion of
 * the available signals (s_vec, s_lex, s_ce). Signals that are NaN are
 * dropped from numerator AND denominator so weights stay normalized.
 *
 * The module SHARES the cross-encoder runtime owned by a mg_verify_ctx_t —
 * it does NOT load a second copy of the model.
 */

#include "internal.h"

#include "graft/rerank.h"
#include "graft/verify.h"
#include "graft/verify_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Weighted fusion. Inputs are assumed already clamped/bounded [0,1] for
 * non-NaN values (PR0 guarantees this for s_vec and s_lex; CE is a
 * sigmoid/softmax output, also in [0,1]).
 * Returns NaN when no signal contributes (all NaN or all weights zero). */
static float mg_rerank_fuse(float s_vec, float s_lex, float s_ce,
                            float w_vec, float w_lex, float w_ce) {
    float fused = 0.0f, w_sum = 0.0f;
    if (!isnan(s_vec) && w_vec > 0.0f) { fused += w_vec * s_vec; w_sum += w_vec; }
    if (!isnan(s_lex) && w_lex > 0.0f) { fused += w_lex * s_lex; w_sum += w_lex; }
    if (!isnan(s_ce)  && w_ce  > 0.0f) { fused += w_ce  * s_ce ; w_sum += w_ce ; }
    return w_sum > 0.0f ? fused / w_sum : NAN;
}

mg_err_t mg_rerank_init(const mg_config_t *cfg,
                        struct mg_verify_ctx *verify_ctx_for_ce,
                        mg_rerank_ctx_t **out) {
    if (!cfg || !out) return MG_ERR_INVALID_ARG;
    *out = NULL;

    if (!cfg->rerank_enabled) {
        /* Disabled by config: NULL ctx + MG_OK is the documented sentinel. */
        return MG_OK;
    }

    mg_rerank_ctx_t *r = (mg_rerank_ctx_t *)calloc(1, sizeof(*r));
    if (!r) return MG_ERR_OOM;

    /* Shallow copy: we only read scalar weight/top_k fields. */
    r->cfg = *cfg;
    r->verify_ctx_for_ce = verify_ctx_for_ce;

    /* Clamp negative weights to zero. */
    if (r->cfg.rerank_w_vec < 0.0f) r->cfg.rerank_w_vec = 0.0f;
    if (r->cfg.rerank_w_lex < 0.0f) r->cfg.rerank_w_lex = 0.0f;
    if (r->cfg.rerank_w_ce  < 0.0f) r->cfg.rerank_w_ce  = 0.0f;

    /* If all weights are 0, the fuse function would always return NaN —
     * treat as a config error and disable. */
    if (r->cfg.rerank_w_vec == 0.0f &&
        r->cfg.rerank_w_lex == 0.0f &&
        r->cfg.rerank_w_ce  == 0.0f) {
        fprintf(stderr,
                "rerank: all fusion weights are zero; rerank disabled\n");
        free(r);
        return MG_OK;
    }

    r->enabled = true;
    *out = r;
    return MG_OK;
}

void mg_rerank_shutdown(mg_rerank_ctx_t *r) {
    if (!r) return;
    /* verify_ctx_for_ce is borrowed — do NOT shut it down here. */
    free(r);
}

bool mg_rerank_enabled(const mg_rerank_ctx_t *r) {
    return r != NULL && r->enabled;
}

mg_err_t mg_rerank_batch(mg_rerank_ctx_t *r,
                         const char *query_text,
                         const mg_rerank_input_t *cands, int n,
                         mg_rerank_output_t *out) {
    if (!r || !query_text || !cands || !out || n < 0) {
        return MG_ERR_INVALID_ARG;
    }
    if (!r->enabled) return MG_ERR_INVALID_ARG;

    /* CE is sequential here — each candidate runs through one
     * mg_ce_score_pair forward pass. True multi-pair batch decoding (packing
     * multiple sequences into a single llama batch) is a future optimization;
     * this loop IS the rerank hotspot when CE is on. */
    bool ce_available = r->verify_ctx_for_ce != NULL &&
                        r->verify_ctx_for_ce->ce_runtime_enabled;

    for (int i = 0; i < n; i++) {
        out[i].s_ce = NAN;
        if (ce_available && cands[i].candidate_text) {
            float ce = NAN;
            if (mg_ce_score_pair(r->verify_ctx_for_ce,
                                 query_text,
                                 cands[i].candidate_text,
                                 &ce) == 0) {
                out[i].s_ce = ce;
            }
        }
        out[i].fused = mg_rerank_fuse(cands[i].s_vec,
                                      cands[i].s_lex,
                                      out[i].s_ce,
                                      r->cfg.rerank_w_vec,
                                      r->cfg.rerank_w_lex,
                                      r->cfg.rerank_w_ce);
    }
    return MG_OK;
}
