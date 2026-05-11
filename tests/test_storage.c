#include "graft/storage.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int same_id(const mg_node_id_t a, const mg_node_id_t b) {
  return memcmp(a, b, MG_NODE_ID_BYTES) == 0;
}

static void make_embedding(mg_embedding_t e) {
  float norm = 0.0f;
  size_t i;
  for (i = 0; i < MG_EMBEDDING_DIM; ++i) {
    e[i] = (float)((i % 17) + 1);
    norm += e[i] * e[i];
  }
  norm = sqrtf(norm);
  for (i = 0; i < MG_EMBEDDING_DIM; ++i) {
    e[i] /= norm;
  }
}

int main(void) {
  const char *path = "./test_storage.db";
  mg_storage_t *s = NULL;
  mg_node_t node;
  mg_node_t got;
  mg_embedding_t emb;
  mg_node_score_t scores[4];
  mg_node_id_t id_by_hash;
  int count = 0;
  mg_err_t err;

  remove(path);
  remove("./test_storage.db-wal");
  remove("./test_storage.db-shm");

  err = mg_storage_open(path, &s);
  if (err != MG_OK) {
    fprintf(stderr, "open: %s\n", mg_strerror(err));
    return 1;
  }
  err = mg_storage_apply_schema(s);
  if (err != MG_OK) {
    fprintf(stderr, "schema: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  memset(&node, 0, sizeof(node));
  mg_uuidv7(node.id);
  mg_blake3((const uint8_t *)"title/body", strlen("title/body"), node.content_hash);
  node.title = (char *)"title";
  node.body  = (char *)"body text";
  node.author = NULL;
  node.created_at = 1;
  node.expires_at = 0;
  node.last_access = 1;
  node.access_count = 0;
  node.state = MG_NODE_ACTIVE;
  make_embedding(emb);

  err = mg_storage_insert_node_with_edges(s, &node, emb, NULL, 0, NULL, 0, NULL);
  if (err != MG_OK) {
    fprintf(stderr, "insert: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_get_node(s, node.id, &got);
  if (err != MG_OK || strcmp(got.title, "title") != 0 || strcmp(got.body, "body text") != 0) {
    fprintf(stderr, "get_node failed\n");
    mg_storage_close(s);
    return 1;
  }
  mg_node_free(&got);

  err = mg_storage_node_id_by_hash(s, node.content_hash, id_by_hash);
  if (err != MG_OK || !same_id(node.id, id_by_hash)) {
    fprintf(stderr, "hash lookup failed\n");
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_vector_topk(s, emb, 4, scores, &count);
  if (err != MG_OK || count != 1 || !same_id(scores[0].id, node.id) || fabsf(scores[0].score - 1.0f) > 0.001f) {
    fprintf(stderr, "topk failed count=%d score=%f\n", count, count > 0 ? scores[0].score : 0.0f);
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_insert_node_with_edges(s, &node, emb, NULL, 0, NULL, 0, NULL);
  if (err != MG_ERR_DUPLICATE) {
    fprintf(stderr, "duplicate check failed: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_node_t title_hit;
  mg_node_t body_hit;
  memset(&title_hit, 0, sizeof(title_hit));
  memset(&body_hit, 0, sizeof(body_hit));

  mg_uuidv7(title_hit.id);
  mg_blake3((const uint8_t *)"alpha-title/beta-body", strlen("alpha-title/beta-body"), title_hit.content_hash);
  title_hit.title = (char *)"alpha title only";
  title_hit.body = (char *)"beta body only";
  title_hit.created_at = 2;
  title_hit.expires_at = 0;
  title_hit.last_access = 2;
  title_hit.state = MG_NODE_ACTIVE;

  mg_uuidv7(body_hit.id);
  mg_blake3((const uint8_t *)"beta-title/alpha-body", strlen("beta-title/alpha-body"), body_hit.content_hash);
  body_hit.title = (char *)"beta title only";
  body_hit.body = (char *)"alpha body only";
  body_hit.created_at = 3;
  body_hit.expires_at = 0;
  body_hit.last_access = 3;
  body_hit.state = MG_NODE_ACTIVE;

  err = mg_storage_insert_node_with_edges(s, &title_hit, emb, NULL, 0, NULL, 0, NULL);
  if (err != MG_OK) {
    fprintf(stderr, "insert title_hit: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_keyword_id_t graph_kw = 0;
  err = mg_storage_upsert_keyword(s, "graph", NULL, &graph_kw);
  if (err != MG_OK || graph_kw <= 0) {
    fprintf(stderr, "upsert graph keyword: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_edge_t one_way_edges[2];
  memset(one_way_edges, 0, sizeof(one_way_edges));
  memcpy(one_way_edges[0].src, body_hit.id, MG_NODE_ID_BYTES);
  memcpy(one_way_edges[0].dst, title_hit.id, MG_NODE_ID_BYTES);
  one_way_edges[0].kind = MG_EDGE_SEMANTIC;
  one_way_edges[0].keyword_id = 0;
  one_way_edges[0].weight = 0.75f;
  memcpy(one_way_edges[1].src, body_hit.id, MG_NODE_ID_BYTES);
  memcpy(one_way_edges[1].dst, title_hit.id, MG_NODE_ID_BYTES);
  one_way_edges[1].kind = MG_EDGE_KEYWORD;
  one_way_edges[1].keyword_id = graph_kw;
  one_way_edges[1].weight = 0.8f;

  err = mg_storage_insert_node_with_edges(s, &body_hit, emb, NULL, 0, one_way_edges, 2, NULL);
  if (err != MG_OK) {
    fprintf(stderr, "insert body_hit: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_fts_search(s, "alpha", 4, true, false, scores, &count);
  if (err != MG_OK || count != 1 || !same_id(scores[0].id, title_hit.id)) {
    fprintf(stderr, "title-only fts failed count=%d err=%s\n", count, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_fts_search(s, "alpha", 4, false, true, scores, &count);
  if (err != MG_OK || count != 1 || !same_id(scores[0].id, body_hit.id)) {
    fprintf(stderr, "body-only fts failed count=%d err=%s\n", count, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_fts_search(s, "alpha", 4, true, true, scores, &count);
  if (err != MG_OK || count != 2) {
    fprintf(stderr, "combined fts failed count=%d err=%s\n", count, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_edge_t neighbors[4];
  int n_neighbors = 0;
  err = mg_storage_neighbors(s, title_hit.id, MG_EDGE_SEMANTIC, NULL, 0, neighbors, 4, &n_neighbors);
  if (err != MG_OK || n_neighbors != 1 || !same_id(neighbors[0].src, title_hit.id) || !same_id(neighbors[0].dst, body_hit.id)) {
    fprintf(stderr, "incoming semantic neighbor failed count=%d err=%s\n", n_neighbors, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_neighbors(s, title_hit.id, MG_EDGE_KEYWORD, &graph_kw, 1, neighbors, 4, &n_neighbors);
  if (err != MG_OK || n_neighbors != 1 || neighbors[0].keyword_id != graph_kw ||
      !same_id(neighbors[0].src, title_hit.id) || !same_id(neighbors[0].dst, body_hit.id)) {
    fprintf(stderr, "incoming keyword neighbor failed count=%d err=%s\n", n_neighbors, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_neighbors(s, body_hit.id, MG_EDGE_SEMANTIC, NULL, 0, neighbors, 4, &n_neighbors);
  if (err != MG_OK || n_neighbors != 1 || !same_id(neighbors[0].src, body_hit.id) || !same_id(neighbors[0].dst, title_hit.id)) {
    fprintf(stderr, "outgoing semantic neighbor failed count=%d err=%s\n", n_neighbors, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_node_t expired_neighbor;
  mg_edge_t expired_neighbor_edge;
  memset(&expired_neighbor, 0, sizeof(expired_neighbor));
  memset(&expired_neighbor_edge, 0, sizeof(expired_neighbor_edge));
  mg_uuidv7(expired_neighbor.id);
  mg_blake3((const uint8_t *)"expired-neighbor", strlen("expired-neighbor"), expired_neighbor.content_hash);
  expired_neighbor.title = (char *)"expired neighbor";
  expired_neighbor.body = (char *)"expired neighbor body";
  expired_neighbor.created_at = 4;
  expired_neighbor.expires_at = 1;
  expired_neighbor.last_access = 4;
  expired_neighbor.state = MG_NODE_ACTIVE;
  memcpy(expired_neighbor_edge.src, title_hit.id, MG_NODE_ID_BYTES);
  memcpy(expired_neighbor_edge.dst, expired_neighbor.id, MG_NODE_ID_BYTES);
  expired_neighbor_edge.kind = MG_EDGE_SEMANTIC;
  expired_neighbor_edge.weight = 1.0f;

  err = mg_storage_insert_node_with_edges(s, &expired_neighbor, emb, NULL, 0, &expired_neighbor_edge, 1, NULL);
  if (err != MG_OK) {
    fprintf(stderr, "insert expired_neighbor: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }
  err = mg_storage_neighbors(s, title_hit.id, MG_EDGE_SEMANTIC, NULL, 0, neighbors, 4, &n_neighbors);
  if (err != MG_OK || n_neighbors != 1 || !same_id(neighbors[0].dst, body_hit.id)) {
    fprintf(stderr, "expired neighbor leaked count=%d err=%s\n", n_neighbors, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_node_t expired_vec;
  memset(&expired_vec, 0, sizeof(expired_vec));
  mg_uuidv7(expired_vec.id);
  mg_blake3((const uint8_t *)"expired-vector", strlen("expired-vector"), expired_vec.content_hash);
  expired_vec.title = (char *)"expired vector";
  expired_vec.body = (char *)"expired vector body";
  expired_vec.created_at = 4;
  expired_vec.expires_at = 1;
  expired_vec.last_access = 4;
  expired_vec.state = MG_NODE_ACTIVE;

  err = mg_storage_insert_node_with_edges(s, &expired_vec, emb, NULL, 0, NULL, 0, NULL);
  if (err != MG_OK) {
    fprintf(stderr, "insert expired_vec: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_vector_topk(s, emb, 8, scores, &count);
  if (err != MG_OK) {
    fprintf(stderr, "expired vector topk failed: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }
  for (int i = 0; i < count; ++i) {
    if (same_id(scores[i].id, expired_vec.id)) {
      fprintf(stderr, "expired vector result leaked\n");
      mg_storage_close(s);
      return 1;
    }
  }
  err = mg_storage_get_node(s, expired_vec.id, &got);
  if (err != MG_ERR_NOT_FOUND) {
    if (err == MG_OK) mg_node_free(&got);
    fprintf(stderr, "expired vector node was not pruned: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_node_t expired_fts;
  memset(&expired_fts, 0, sizeof(expired_fts));
  mg_uuidv7(expired_fts.id);
  mg_blake3((const uint8_t *)"expired-fts", strlen("expired-fts"), expired_fts.content_hash);
  expired_fts.title = (char *)"gamma expired only";
  expired_fts.body = (char *)"gamma expired body";
  expired_fts.created_at = 5;
  expired_fts.expires_at = 1;
  expired_fts.last_access = 5;
  expired_fts.state = MG_NODE_ACTIVE;

  err = mg_storage_insert_node_with_edges(s, &expired_fts, emb, NULL, 0, NULL, 0, NULL);
  if (err != MG_OK) {
    fprintf(stderr, "insert expired_fts: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  err = mg_storage_fts_search(s, "gamma", 4, true, true, scores, &count);
  if (err != MG_OK || count != 0) {
    fprintf(stderr, "expired fts leaked count=%d err=%s\n", count, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_node_t expired_cleanup;
  int64_t deleted = 0;
  memset(&expired_cleanup, 0, sizeof(expired_cleanup));
  mg_uuidv7(expired_cleanup.id);
  mg_blake3((const uint8_t *)"expired-cleanup", strlen("expired-cleanup"), expired_cleanup.content_hash);
  expired_cleanup.title = (char *)"expired cleanup";
  expired_cleanup.body = (char *)"expired cleanup body";
  expired_cleanup.created_at = 6;
  expired_cleanup.expires_at = 1;
  expired_cleanup.last_access = 6;
  expired_cleanup.state = MG_NODE_ACTIVE;

  err = mg_storage_insert_node_with_edges(s, &expired_cleanup, emb, NULL, 0, NULL, 0, NULL);
  if (err != MG_OK) {
    fprintf(stderr, "insert expired_cleanup: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }
  err = mg_storage_prune_expired(s, &deleted);
  if (err != MG_OK || deleted != 1) {
    fprintf(stderr, "prune expired failed deleted=%lld err=%s\n",
            (long long)deleted, mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  mg_storage_close(s);
  remove(path);
  remove("./test_storage.db-wal");
  remove("./test_storage.db-shm");
  return 0;
}
