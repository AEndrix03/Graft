#include "graft/config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void expect_int(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_config: %s\n", msg);
    g_failures++;
  }
}

static void expect_float(float actual, float expected, float tol, const char *msg) {
  float diff = actual - expected;
  if (diff < 0) diff = -diff;
  if (diff > tol) {
    fprintf(stderr, "test_config: %s (got %f, expected %f)\n", msg, (double)actual, (double)expected);
    g_failures++;
  }
}

static void write_config(const char *path, const char *content) {
  FILE *fp = fopen(path, "wb");
  if (!fp) { fprintf(stderr, "cannot write %s\n", path); g_failures++; return; }
  fputs(content, fp);
  fclose(fp);
}

/* --- test cases --- */

static void test_missing_file(void) {
  mg_config_t cfg;
  expect_int(mg_config_load("no_such_file_xyz.yaml", &cfg) != MG_OK,
             "missing file must return error");
  printf("ok missing file\n");
}

static void test_null_args(void) {
  mg_config_t cfg;
  expect_int(mg_config_load(NULL, &cfg)     != MG_OK, "NULL path must return error");
  expect_int(mg_config_load("x.yaml", NULL) != MG_OK, "NULL out must return error");
  printf("ok null args\n");
}

static void test_defaults(void) {
  /* mg_config_defaults() must fill every field with the documented default
   * without touching the filesystem.  Values come from src/config/config.c. */
  mg_config_t cfg;
  mg_config_defaults(&cfg);

  expect_int(cfg.socket_path && strcmp(cfg.socket_path, "/tmp/graft.sock") == 0,
             "default socket_path");
  expect_int(cfg.db_path && strcmp(cfg.db_path, "./graft.db") == 0,
             "default db_path");
  expect_int(cfg.embed_model_path &&
             strcmp(cfg.embed_model_path, "./models/bge-m3.gguf") == 0,
             "default embed_model_path");
  expect_int(cfg.embed_threads  == 4,    "default embed_threads");
  expect_int(cfg.embed_ctx_size == 8192, "default embed_ctx_size");
  expect_int(!cfg.hardware_accel,        "default hardware_accel=false");

  expect_int(!cfg.cross_encoder_enabled,           "default cross_encoder_enabled=false");
  expect_int(cfg.cross_encoder_model_path == NULL, "default cross_encoder_model_path=NULL");
  expect_int(!cfg.nli_enabled,                     "default nli_enabled=false");
  expect_int(cfg.nli_prompt_template != NULL,       "default nli_prompt_template!=NULL");

  expect_float(cfg.strong_hit_min_ce,  0.6f,   0.001f, "default strong_hit_min_ce");
  expect_float(cfg.strong_hit_min_lex, 0.15f,  0.001f, "default strong_hit_min_lex");
  expect_float(cfg.weak_hit_min_vec,   0.85f,  0.001f, "default weak_hit_min_vec");
  expect_float(cfg.min_lex_overlap,    0.05f,  0.001f, "default min_lex_overlap");
  expect_float(cfg.verify_lex_strong_min_vec,    0.7f,   0.001f, "default verify_lex_strong_min_vec");
  expect_float(cfg.verify_sem_strong_min_vec,    0.75f,  0.001f, "default verify_sem_strong_min_vec");
  expect_float(cfg.verify_sem_strong_lex_margin, 0.015f, 0.001f, "default verify_sem_strong_lex_margin");
  expect_int(!cfg.verify_use_fused_gate, "default verify_use_fused_gate=false");
  expect_float(cfg.verify_strong_min_fused, 0.7f, 0.001f, "default verify_strong_min_fused");
  expect_float(cfg.verify_weak_min_fused,   0.5f, 0.001f, "default verify_weak_min_fused");

  expect_int(cfg.retrieve_top_k      == 25, "default retrieve_top_k");
  expect_int(cfg.rrf_k_const         == 60, "default rrf_k_const");
  expect_int(cfg.query_fallback_top_k == 5, "default query_fallback_top_k");

  expect_int(!cfg.rerank_enabled,    "default rerank_enabled=false");
  expect_int(cfg.rerank_top_k == 25, "default rerank_top_k");
  expect_float(cfg.rerank_w_vec, 0.30f, 0.001f, "default rerank_w_vec");
  expect_float(cfg.rerank_w_lex, 0.25f, 0.001f, "default rerank_w_lex");
  expect_float(cfg.rerank_w_ce,  0.45f, 0.001f, "default rerank_w_ce");
  expect_float(cfg.rerank_w_nli, 0.0f,  0.001f, "default rerank_w_nli");

  expect_float(cfg.edge_keyword_min,  0.5f, 0.001f, "default edge_keyword_min");
  expect_float(cfg.edge_semantic_min, 0.6f, 0.001f, "default edge_semantic_min");
  expect_int(cfg.edge_keyword_topk  ==  5, "default edge_keyword_topk");
  expect_int(cfg.edge_semantic_topk == 20, "default edge_semantic_topk");
  expect_float(cfg.mmr_lambda, 0.7f, 0.001f, "default mmr_lambda");

  expect_int(cfg.explore_default_beam  == 4, "default explore_default_beam");
  expect_int(cfg.explore_default_depth == 3, "default explore_default_depth");
  expect_float(cfg.explore_decay_gamma, 0.85f, 0.001f, "default explore_decay_gamma");
  expect_float(cfg.explore_alpha,       0.5f,  0.001f, "default explore_alpha");

  expect_int(!cfg.http_enabled, "default http_enabled=false");
  expect_int(cfg.http_bind && strcmp(cfg.http_bind, "127.0.0.1") == 0, "default http_bind");
  expect_int(cfg.http_port == 9977,               "default http_port");
  expect_int(!cfg.http_allow_remote,              "default http_allow_remote=false");
  expect_int(!cfg.http_tls_terminated_externally, "default http_tls_terminated_externally=false");
  expect_int(cfg.http_auth_token      == NULL,    "default http_auth_token=NULL");
  expect_int(cfg.http_readonly_token  == NULL,    "default http_readonly_token=NULL");
  expect_int(!cfg.http_view_anonymize,            "default http_view_anonymize=false");
  expect_int(cfg.http_view_keyword_scope == NULL, "default http_view_keyword_scope=NULL");
  expect_int( cfg.http_ep_match,    "default http_ep_match=true");
  expect_int( cfg.http_ep_search,   "default http_ep_search=true");
  expect_int( cfg.http_ep_explore,  "default http_ep_explore=true");
  expect_int( cfg.http_ep_classify, "default http_ep_classify=true");
  expect_int( cfg.http_ep_insert,   "default http_ep_insert=true");
  expect_int(!cfg.http_ep_delete,   "default http_ep_delete=false");
  expect_int( cfg.http_ep_view,     "default http_ep_view=true");
  expect_int(cfg.http_viewer_path &&
             strcmp(cfg.http_viewer_path, "viewer/dist") == 0,
             "default http_viewer_path");

  mg_config_free(&cfg);
  printf("ok defaults\n");
}

static void test_overrides(void) {
  /* Every config key must be parseable and override its default. */
  const char *path = "test_config_overrides.yaml";
  write_config(path,
    "daemon:\n"
    "  socket_path: \"./custom.sock\"\n"
    "  db_path: \"./custom.db\"\n"
    "embedding:\n"
    "  model_path: \"./models/custom.gguf\"\n"
    "  threads: 2\n"
    "  ctx_size: 4096\n"
    "  hardware_accel: true\n"
    "verification:\n"
    "  cross_encoder_enabled: true\n"
    "  cross_encoder_model_path: \"./models/ce.gguf\"\n"
    "  nli_enabled: yes\n"
    "  nli_prompt_template: \"P: {d} H: {q}\"\n"
    "  lex_strong_min_vec: 0.65\n"
    "  sem_strong_min_vec: 0.72\n"
    "  sem_strong_lex_margin: 0.02\n"
    "  use_fused_gate: true\n"
    "  strong_min_fused: 0.75\n"
    "  weak_min_fused: 0.45\n"
    "cache:\n"
    "  weak_hit_min_vec: 0.90\n"
    "  strong_hit_min_ce: 0.55\n"
    "  strong_hit_min_lex: 0.20\n"
    "  min_lex_overlap: 0.08\n"
    "retrieval:\n"
    "  top_k: 30\n"
    "  rrf_k_const: 50\n"
    "  query_fallback_top_k: 10\n"
    "rerank:\n"
    "  enabled: true\n"
    "  top_k: 15\n"
    "  w_vec: 0.40\n"
    "  w_lex: 0.20\n"
    "  w_ce: 0.30\n"
    "  w_nli: 0.10\n"
    "edges:\n"
    "  edge_keyword_min: 0.55\n"
    "  edge_semantic_min: 0.65\n"
    "  edge_keyword_topk: 8\n"
    "  edge_semantic_topk: 25\n"
    "  mmr_lambda: 0.80\n"
    "explore:\n"
    "  default_beam: 6\n"
    "  default_depth: 7\n"
    "  decay_gamma: 0.90\n"
    "  alpha: 0.60\n"
    "http:\n"
    "  enabled: true\n"
    "  bind: \"0.0.0.0\"\n"
    "  port: 8080\n"
    "  allow_remote: true\n"
    "  tls_terminated_externally: true\n"
    "  auth_token: \"secret123\"\n"
    "  readonly_token: \"readtoken\"\n"
    "  view_anonymize: true\n"
    "  view_keyword_scope: \"project-x\"\n"
    "  endpoint_match: false\n"
    "  endpoint_search: false\n"
    "  endpoint_explore: false\n"
    "  endpoint_classify: false\n"
    "  endpoint_insert: false\n"
    "  endpoint_delete: true\n"
    "  endpoint_view: false\n"
    "  viewer_path: \"./dist\"\n"
  );

  mg_config_t cfg;
  expect_int(mg_config_load(path, &cfg) == MG_OK, "load overrides");

  expect_int(strcmp(cfg.socket_path, "./custom.sock") == 0, "socket_path override");
  expect_int(strcmp(cfg.db_path,     "./custom.db")   == 0, "db_path override");
  expect_int(cfg.embed_model_path &&
             strcmp(cfg.embed_model_path, "./models/custom.gguf") == 0,
             "embed_model_path override");
  expect_int(cfg.embed_threads  == 2,    "embed_threads override");
  expect_int(cfg.embed_ctx_size == 4096, "embed_ctx_size override");
  expect_int(cfg.hardware_accel,         "hardware_accel override");
  expect_int(cfg.cross_encoder_enabled,  "cross_encoder_enabled override");
  expect_int(cfg.cross_encoder_model_path &&
             strcmp(cfg.cross_encoder_model_path, "./models/ce.gguf") == 0,
             "cross_encoder_model_path override");
  expect_int(cfg.nli_enabled, "nli_enabled override (yes)");
  expect_int(cfg.nli_prompt_template && strlen(cfg.nli_prompt_template) > 0,
             "nli_prompt_template override");
  expect_float(cfg.verify_lex_strong_min_vec,    0.65f, 0.001f, "lex_strong_min_vec override");
  expect_float(cfg.verify_sem_strong_min_vec,    0.72f, 0.001f, "sem_strong_min_vec override");
  expect_float(cfg.verify_sem_strong_lex_margin, 0.02f, 0.001f, "sem_strong_lex_margin override");
  expect_int(cfg.verify_use_fused_gate, "use_fused_gate override");
  expect_float(cfg.verify_strong_min_fused, 0.75f, 0.001f, "strong_min_fused override");
  expect_float(cfg.verify_weak_min_fused,   0.45f, 0.001f, "weak_min_fused override");
  expect_float(cfg.weak_hit_min_vec,   0.90f, 0.001f, "weak_hit_min_vec override");
  expect_float(cfg.strong_hit_min_ce,  0.55f, 0.001f, "strong_hit_min_ce override");
  expect_float(cfg.strong_hit_min_lex, 0.20f, 0.001f, "strong_hit_min_lex override");
  expect_float(cfg.min_lex_overlap,    0.08f, 0.001f, "min_lex_overlap override");
  expect_int(cfg.retrieve_top_k      == 30, "retrieve_top_k override");
  expect_int(cfg.rrf_k_const         == 50, "rrf_k_const override");
  expect_int(cfg.query_fallback_top_k == 10, "query_fallback_top_k override");
  expect_int(cfg.rerank_enabled,      "rerank_enabled override");
  expect_int(cfg.rerank_top_k == 15,  "rerank_top_k override");
  expect_float(cfg.rerank_w_vec, 0.40f, 0.001f, "rerank_w_vec override");
  expect_float(cfg.rerank_w_lex, 0.20f, 0.001f, "rerank_w_lex override");
  expect_float(cfg.rerank_w_ce,  0.30f, 0.001f, "rerank_w_ce override");
  expect_float(cfg.rerank_w_nli, 0.10f, 0.001f, "rerank_w_nli override");
  expect_float(cfg.edge_keyword_min,  0.55f, 0.001f, "edge_keyword_min override");
  expect_float(cfg.edge_semantic_min, 0.65f, 0.001f, "edge_semantic_min override");
  expect_int(cfg.edge_keyword_topk  ==  8, "edge_keyword_topk override");
  expect_int(cfg.edge_semantic_topk == 25, "edge_semantic_topk override");
  expect_float(cfg.mmr_lambda, 0.80f, 0.001f, "mmr_lambda override");
  expect_int(cfg.explore_default_beam  == 6, "explore_default_beam override");
  expect_int(cfg.explore_default_depth == 7, "explore_default_depth override");
  expect_float(cfg.explore_decay_gamma, 0.90f, 0.001f, "explore_decay_gamma override");
  expect_float(cfg.explore_alpha,       0.60f, 0.001f, "explore_alpha override");
  expect_int(cfg.http_enabled, "http_enabled override");
  expect_int(strcmp(cfg.http_bind, "0.0.0.0") == 0, "http_bind override");
  expect_int(cfg.http_port == 8080,              "http_port override");
  expect_int(cfg.http_allow_remote,              "http_allow_remote override");
  expect_int(cfg.http_tls_terminated_externally, "http_tls_terminated_externally override");
  expect_int(cfg.http_auth_token &&
             strcmp(cfg.http_auth_token, "secret123") == 0, "http_auth_token override");
  expect_int(cfg.http_readonly_token &&
             strcmp(cfg.http_readonly_token, "readtoken") == 0, "http_readonly_token override");
  expect_int(cfg.http_view_anonymize, "http_view_anonymize override");
  expect_int(cfg.http_view_keyword_scope &&
             strcmp(cfg.http_view_keyword_scope, "project-x") == 0,
             "http_view_keyword_scope override");
  expect_int(!cfg.http_ep_match,    "endpoint_match disabled");
  expect_int(!cfg.http_ep_search,   "endpoint_search disabled");
  expect_int(!cfg.http_ep_explore,  "endpoint_explore disabled");
  expect_int(!cfg.http_ep_classify, "endpoint_classify disabled");
  expect_int(!cfg.http_ep_insert,   "endpoint_insert disabled");
  expect_int( cfg.http_ep_delete,   "endpoint_delete enabled");
  expect_int(!cfg.http_ep_view,     "endpoint_view disabled");
  expect_int(cfg.http_viewer_path &&
             strcmp(cfg.http_viewer_path, "./dist") == 0, "http_viewer_path override");

  mg_config_free(&cfg);
  remove(path);
  printf("ok overrides\n");
}

static void test_partial_yaml(void) {
  /* Only some sections present: unmentioned fields keep their defaults. */
  const char *path = "test_config_partial.yaml";
  write_config(path,
    "embedding:\n"
    "  threads: 8\n"
  );

  mg_config_t cfg;
  expect_int(mg_config_load(path, &cfg) == MG_OK, "partial yaml loads");
  expect_int(cfg.embed_threads == 8,     "partial: embed_threads set");
  expect_int(cfg.embed_ctx_size == 8192, "partial: ctx_size keeps default");
  expect_int(cfg.retrieve_top_k == 25,   "partial: retrieve_top_k keeps default");
  expect_int(!cfg.http_enabled,          "partial: http_enabled keeps default");
  expect_int(!cfg.rerank_enabled,        "partial: rerank_enabled keeps default");

  mg_config_free(&cfg);
  remove(path);
  printf("ok partial yaml\n");
}

static void test_unknown_keys_ignored(void) {
  /* Unknown section/key must not produce an error. */
  const char *path = "test_config_unknown.yaml";
  write_config(path,
    "unknown_section:\n"
    "  some_key: \"value\"\n"
    "embedding:\n"
    "  threads: 3\n"
    "  future_key: \"ignored\"\n"
  );

  mg_config_t cfg;
  expect_int(mg_config_load(path, &cfg) == MG_OK,
             "unknown keys: load must not fail");
  expect_int(cfg.embed_threads == 3,
             "unknown keys: known key in same section still parsed");

  mg_config_free(&cfg);
  remove(path);
  printf("ok unknown keys ignored\n");
}

static void test_double_free_safe(void) {
  /* mg_config_free on zeroed struct and on NULL must not crash. */
  mg_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  mg_config_free(&cfg);
  mg_config_free(NULL);
  printf("ok double free safe\n");
}

int main(void) {
  test_missing_file();
  test_null_args();
  test_defaults();
  test_overrides();
  test_partial_yaml();
  test_unknown_keys_ignored();
  test_double_free_safe();
  if (g_failures == 0) {
    printf("test_config: PASS\n");
    return 0;
  }
  fprintf(stderr, "test_config: %d failures\n", g_failures);
  return 1;
}
