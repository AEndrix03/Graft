#ifndef MEMGRAPH_CONFIG_H
#define MEMGRAPH_CONFIG_H

#include "memgraph/error.h"
#include <stdbool.h>

typedef struct {
  /* daemon */
  char *socket_path;
  char *db_path;

  /* embedding */
  char *embed_model_path;
  int   embed_threads;
  int   embed_ctx_size;

  /* verification (feature flags) */
  bool  cross_encoder_enabled;
  char *cross_encoder_model_path;
  bool  nli_enabled;

  /* gating thresholds */
  float strong_hit_min_ce;
  float weak_hit_min_vec;
  float min_lex_overlap;
  float strong_hit_min_lex;

  /* retrieval */
  int   retrieve_top_k;
  int   rrf_k_const;

  /* edges */
  float edge_keyword_min;
  float edge_semantic_min;
  int   edge_keyword_topk;
  int   edge_semantic_topk;
  float mmr_lambda;

  /* explore */
  int   explore_default_beam;
  int   explore_default_depth;
  float explore_decay_gamma;
  float explore_alpha;          /* peso semantic vs edge */
} mg_config_t;

mg_err_t mg_config_load(const char *path, mg_config_t *out);
void     mg_config_free(mg_config_t *cfg);

/* Default values quando una chiave manca */
void     mg_config_defaults(mg_config_t *cfg);

#endif
