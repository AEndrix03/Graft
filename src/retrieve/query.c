/* mg_op_query: cache lookup with multi-signal gating.
 *
 * Decision flow:
 *   1. Embed query text -> q.
 *   2. vector_topk(q, 1) -> top1.
 *   3. Sanity floor: if no candidate or s_vec < 0.3 -> MISS + fallback retrieve.
 *   4. Get candidate node, compute s_lex via trigram Jaccard.
 *   5. mg_verify_score() fills s_jaccard, s_ce, hit_level.
 *   6. STRONG -> touch_access, return summary+detail.
 *      WEAK   -> return summary only (no detail).
 *      NONE   -> MISS + fallback retrieve.
 *
 * Side effects:
 *   - On STRONG hit: mg_storage_touch_access(top1.id).
 *   - On any non-empty top1 (HIT or MISS): record_sample(kind=1, s_vec).
 *   - signals_only=true suppresses BOTH side effects (read-only inspection).
 *
 * Result map (handler writes a single mpack VALUE):
 *
 *  HIT (STRONG | WEAK):
 *   { "hit": "STRONG"|"WEAK", "id_hex", "summary",
 *     "detail": string|nil,
 *     "signals": { s_vec, s_lex, s_jaccard, s_ce|nil } }
 *
 *  MISS:
 *   { "hit": "MISS",
 *     "fallback_retrieve": { results, distinct_keywords },
 *     "signals": { ... } }
 */

#include "internal.h"
#include "memgraph/storage.h"
#include "memgraph/embed.h"
#include "memgraph/verify.h"
#include "memgraph/config.h"
#include "memgraph/types.h"
#include "memgraph/error.h"

#include <stdlib.h>
#include <string.h>

#define MG_QUERY_VEC_SANITY_FLOOR 0.3f
#define MG_QUERY_SAMPLE_KIND      1   /* "query_top1" */

static const char *hit_label(mg_hit_t h) {
    switch (h) {
        case MG_HIT_STRONG: return "STRONG";
        case MG_HIT_WEAK:   return "WEAK";
        default:            return "NONE";
    }
}

static void write_signals_map(mpack_writer_t *w,
                              const mg_verify_signals_t *sig) {
    mpack_build_map(w);
    mpack_write_cstr(w, "s_vec");     mpack_write_float(w, sig->s_vec);
    mpack_write_cstr(w, "s_lex");     mpack_write_float(w, sig->s_lex);
    mpack_write_cstr(w, "s_jaccard"); mpack_write_float(w, sig->s_jaccard);
    mpack_write_cstr(w, "s_ce");
    if (sig->s_ce >= 0.0f) mpack_write_float(w, sig->s_ce);
    else                   mpack_write_nil(w);
    mpack_complete_map(w);
}

static void write_miss(mg_ctx_t *ctx,
                       const char *text,
                       const mg_embedding_t q,
                       const mg_verify_signals_t *sig,
                       mpack_writer_t *result) {
    mpack_build_map(result);

    mpack_write_cstr(result, "hit");
    mpack_write_cstr(result, "MISS");

    mpack_write_cstr(result, "fallback_retrieve");
    /* Run RRF; if it fails internally we still want a map placeholder.
     * Use a tighter cap than the standalone retrieve op: a MISS fallback is a
     * "by the way" hint, not the answer, and a wide list of neighbors can blow
     * the agent's context on broad queries (e.g. single-keyword lookups). */
    int fallback_k = ctx->config->query_fallback_top_k;
    if (fallback_k <= 0) fallback_k = 5;
    if (fallback_k > ctx->config->retrieve_top_k && ctx->config->retrieve_top_k > 0)
        fallback_k = ctx->config->retrieve_top_k;
    if (mg_retrieve_run_rrf(ctx, text, q,
                            fallback_k, result) != MG_OK) {
        /* run_rrf may have left writer in error state. Best-effort: write nil. */
        mpack_write_nil(result);
    }

    mpack_write_cstr(result, "signals");
    write_signals_map(result, sig);

    mpack_complete_map(result);
}

mg_err_t mg_op_query(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
    if (!ctx || !ctx->storage || !ctx->embed || !ctx->config || !result)
        return MG_ERR_INVALID_ARG;

    char *text = mg_retrieve_node_str_dup(mpack_node_map_cstr(args, "text"));
    if (!text) return MG_ERR_INVALID_ARG;

    bool signals_only = false;
    mpack_node_t so_node = mpack_node_map_cstr_optional(args, "signals_only");
    if (!mpack_node_is_missing(so_node) && !mpack_node_is_nil(so_node)) {
        signals_only = mpack_node_bool(so_node);
    }

    mg_embedding_t q;
    mg_err_t e = mg_embed_text(ctx->embed, text, q);
    if (e != MG_OK) { free(text); return e; }

    mg_node_score_t top1;
    int n_top1 = 0;
    e = mg_storage_vector_topk(ctx->storage, q, 1, &top1, &n_top1);
    if (e != MG_OK) { free(text); return e; }

    /* No candidate at all: pure MISS, no signals available. */
    if (n_top1 == 0) {
        mg_verify_signals_t empty = {0};
        empty.s_ce = -1.0f;
        empty.hit_level = MG_HIT_NONE;
        write_miss(ctx, text, q, &empty, result);
        free(text);
        return MG_OK;
    }

    /* Sanity floor before paying for verify: definite MISS. */
    if (top1.score < MG_QUERY_VEC_SANITY_FLOOR) {
        if (!signals_only) {
            (void)mg_storage_record_sample(ctx->storage,
                                           MG_QUERY_SAMPLE_KIND,
                                           top1.score);
        }
        mg_verify_signals_t sig = {0};
        sig.s_vec = top1.score;
        sig.s_ce = -1.0f;
        sig.hit_level = MG_HIT_NONE;
        write_miss(ctx, text, q, &sig, result);
        free(text);
        return MG_OK;
    }

    mg_node_t cand = {0};
    e = mg_storage_get_node(ctx->storage, top1.id, &cand);
    if (e != MG_OK) { free(text); return e; }

    /* MVP s_lex proxy: trigram Jaccard between query text and candidate
     * summary (BM25 scores aren't directly comparable across queries
     * without per-query normalization, and Jaccard is bounded in [0,1]). */
    float s_lex = mg_text_trigram_jaccard(text,
                                          cand.summary ? cand.summary : "");

    mg_verify_signals_t sig = {0};
    sig.s_ce = -1.0f;
    e = mg_verify_score((mg_verify_ctx_t *)ctx->verify,
                         text, cand.summary ? cand.summary : "",
                         top1.score, s_lex, &sig);
    if (e != MG_OK) {
        mg_node_free(&cand);
        free(text);
        return e;
    }

    if (!signals_only) {
        (void)mg_storage_record_sample(ctx->storage,
                                       MG_QUERY_SAMPLE_KIND,
                                       top1.score);
    }

    if (sig.hit_level == MG_HIT_STRONG || sig.hit_level == MG_HIT_WEAK) {
        if (!signals_only && sig.hit_level == MG_HIT_STRONG) {
            (void)mg_storage_touch_access(ctx->storage, top1.id);
        }

        char id_hex[2 * MG_NODE_ID_BYTES + 1];
        mg_retrieve_hex_encode(top1.id, MG_NODE_ID_BYTES, id_hex);

        mpack_build_map(result);

        mpack_write_cstr(result, "hit");
        mpack_write_cstr(result, hit_label(sig.hit_level));

        mpack_write_cstr(result, "id_hex");
        mpack_write_cstr(result, id_hex);

        mpack_write_cstr(result, "summary");
        mpack_write_cstr(result, cand.summary ? cand.summary : "");

        mpack_write_cstr(result, "detail");
        if (sig.hit_level == MG_HIT_STRONG) {
            mpack_write_cstr(result, cand.detail ? cand.detail : "");
        } else {
            mpack_write_nil(result);
        }

        mpack_write_cstr(result, "signals");
        write_signals_map(result, &sig);

        mpack_complete_map(result);

        mg_node_free(&cand);
        free(text);
        return MG_OK;
    }

    /* MG_HIT_NONE: MISS with fallback. */
    mg_node_free(&cand);
    write_miss(ctx, text, q, &sig, result);
    free(text);
    return MG_OK;
}
