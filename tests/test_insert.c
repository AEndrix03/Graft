#include "memgraph/config.h"
#include "memgraph/storage.h"

#include <stdlib.h>
#include <string.h>

mg_err_t mg_insert_build_edges_from_embedding(
  mg_storage_t *s,
  const mg_config_t *cfg,
  const mg_node_id_t src,
  const mg_embedding_t q,
  const mg_keyword_id_t *kw_ids,
  size_t n_kw,
  mg_edge_t **out_edges,
  size_t *out_n_edges,
  size_t *out_n_kw_edges,
  size_t *out_n_sem_edges
);

static void fill_id(mg_node_id_t id, unsigned char value) {
  memset(id, value, MG_NODE_ID_BYTES);
}

static void fill_hash(mg_hash_t hash, unsigned char value) {
  memset(hash, value, MG_HASH_BYTES);
}

static void fill_embedding(mg_embedding_t emb, float x, float y) {
  memset(emb, 0, sizeof(mg_embedding_t));
  emb[0] = x;
  emb[1] = y;
}

static int insert_seed(
  mg_storage_t *s,
  unsigned char id_byte,
  unsigned char hash_byte,
  const char *title,
  mg_keyword_id_t kw_id,
  const mg_embedding_t emb
) {
  mg_node_t node;
  memset(&node, 0, sizeof(node));
  fill_id(node.id, id_byte);
  fill_hash(node.content_hash, hash_byte);
  node.title = (char *)title;
  node.body  = (char *)"body";
  node.author = NULL;
  node.created_at = 1;
  node.expires_at = 0;
  node.last_access = 1;
  node.access_count = 0;
  node.state = MG_NODE_ACTIVE;
  return mg_storage_insert_node_with_edges(s, &node, emb, &kw_id, 1u, NULL, 0u, NULL) == MG_OK ? 0 : 1;
}

int main(void) {
  mg_storage_t *storage = NULL;
  mg_config_t cfg;
  mg_config_defaults(&cfg);
  cfg.edge_keyword_min = 0.5f;
  cfg.edge_semantic_min = 0.6f;
  cfg.edge_keyword_topk = 5;
  cfg.edge_semantic_topk = 3;
  cfg.mmr_lambda = 0.7f;

  if (mg_storage_open(":memory:", &storage) != MG_OK) {
    mg_config_free(&cfg);
    return 1;
  }
  if (mg_storage_apply_schema(storage) != MG_OK) {
    mg_storage_close(storage);
    mg_config_free(&cfg);
    return 1;
  }

  mg_keyword_id_t kw_id = 0;
  if (mg_storage_upsert_keyword(storage, "systems", NULL, &kw_id) != MG_OK || kw_id <= 0) {
    mg_storage_close(storage);
    mg_config_free(&cfg);
    return 1;
  }

  mg_embedding_t q;
  mg_embedding_t e1;
  mg_embedding_t e2;
  mg_embedding_t e3;
  fill_embedding(q, 1.0f, 0.0f);
  fill_embedding(e1, 0.9f, 0.1f);
  fill_embedding(e2, 0.7f, 0.3f);
  fill_embedding(e3, 0.4f, 0.6f);

  if (insert_seed(storage, 1, 11, "seed one", kw_id, e1) ||
      insert_seed(storage, 2, 22, "seed two", kw_id, e2) ||
      insert_seed(storage, 3, 33, "seed three", kw_id, e3)) {
    mg_storage_close(storage);
    mg_config_free(&cfg);
    return 1;
  }

  mg_hash_t lookup_hash;
  mg_node_id_t found_id;
  fill_hash(lookup_hash, 22);
  if (mg_storage_node_id_by_hash(storage, lookup_hash, found_id) != MG_OK || found_id[0] != 2) {
    mg_storage_close(storage);
    mg_config_free(&cfg);
    return 1;
  }

  mg_node_id_t src;
  fill_id(src, 9);
  mg_edge_t *edges = NULL;
  size_t n_edges = 0u;
  size_t n_kw_edges = 0u;
  size_t n_sem_edges = 0u;
  if (mg_insert_build_edges_from_embedding(
        storage, &cfg, src, q, &kw_id, 1u, &edges, &n_edges, &n_kw_edges, &n_sem_edges) != MG_OK) {
    mg_storage_close(storage);
    mg_config_free(&cfg);
    return 1;
  }

  if (n_edges == 0u || n_kw_edges == 0u || n_sem_edges == 0u || n_edges != n_kw_edges + n_sem_edges) {
    free(edges);
    mg_storage_close(storage);
    mg_config_free(&cfg);
    return 1;
  }

  for (size_t i = 0u; i < n_edges; ++i) {
    if (memcmp(edges[i].src, src, MG_NODE_ID_BYTES) != 0) {
      free(edges);
      mg_storage_close(storage);
      mg_config_free(&cfg);
      return 1;
    }
    if (edges[i].kind == MG_EDGE_KEYWORD && edges[i].weight < cfg.edge_keyword_min) {
      free(edges);
      mg_storage_close(storage);
      mg_config_free(&cfg);
      return 1;
    }
    if (edges[i].kind == MG_EDGE_SEMANTIC && edges[i].weight < cfg.edge_semantic_min) {
      free(edges);
      mg_storage_close(storage);
      mg_config_free(&cfg);
      return 1;
    }
  }

  float pct[6];
  if (mg_storage_distribution_percentiles(storage, 0, pct) != MG_OK || pct[1] <= 0.0f) {
    free(edges);
    mg_storage_close(storage);
    mg_config_free(&cfg);
    return 1;
  }

  free(edges);
  mg_storage_close(storage);
  mg_config_free(&cfg);
  return 0;
}
