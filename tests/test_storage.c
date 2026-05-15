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

/* Make an embedding orthogonal to make_embedding's pattern. */
static void make_embedding2(mg_embedding_t e) {
  float norm = 0.0f;
  size_t i;
  for (i = 0; i < MG_EMBEDDING_DIM; ++i) {
    e[i] = (float)((i % 13) + 1) * ((i % 2) ? 1.0f : -1.0f);
    norm += e[i] * e[i];
  }
  norm = sqrtf(norm);
  for (i = 0; i < MG_EMBEDDING_DIM; ++i) {
    e[i] /= norm;
  }
}

static int g_fail = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "test_storage: " msg "\n"); \
    g_fail++; \
  } \
} while (0)

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

  /* --- 1. Insert + get + hash lookup --- */

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

  /* --- 2. Vector topk --- */

  err = mg_storage_vector_topk(s, emb, 4, scores, &count);
  if (err != MG_OK || count != 1 || !same_id(scores[0].id, node.id) || fabsf(scores[0].score - 1.0f) > 0.001f) {
    fprintf(stderr, "topk failed count=%d score=%f\n", count, count > 0 ? scores[0].score : 0.0f);
    mg_storage_close(s);
    return 1;
  }

  /* --- 3. Duplicate detection --- */

  err = mg_storage_insert_node_with_edges(s, &node, emb, NULL, 0, NULL, 0, NULL);
  if (err != MG_ERR_DUPLICATE) {
    fprintf(stderr, "duplicate check failed: %s\n", mg_strerror(err));
    mg_storage_close(s);
    return 1;
  }

  /* --- 4. FTS: title vs body vs combined --- */

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

  /* --- 5. Neighbors (semantic, keyword, direction) --- */

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

  /* --- 6. Expired nodes (neighbor leak, vector leak, FTS leak, prune) --- */

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

  /* --- 7. count() --- */
  {
    int64_t n_nodes = -1, n_edges = -1, n_keywords = -1;
    CHECK(mg_storage_count(s, MG_STORAGE_COUNT_NODES,    &n_nodes)    == MG_OK, "count nodes ok");
    CHECK(mg_storage_count(s, MG_STORAGE_COUNT_EDGES,    &n_edges)    == MG_OK, "count edges ok");
    CHECK(mg_storage_count(s, MG_STORAGE_COUNT_KEYWORDS, &n_keywords) == MG_OK, "count keywords ok");
    CHECK(n_nodes >= 3, "count nodes >= 3 (at least first+title_hit+body_hit)");
    CHECK(n_edges >= 2, "count edges >= 2 (semantic+keyword from body_hit->title_hit)");
    CHECK(n_keywords >= 1, "count keywords >= 1 (graph keyword)");
    /* Invalid kind. */
    CHECK(mg_storage_count(s, 99, &n_nodes) == MG_ERR_INVALID_ARG, "count invalid kind");
    printf("ok count\n");
  }

  /* --- 8. get_keyword_text --- */
  {
    char *kw_text = NULL;
    CHECK(mg_storage_get_keyword_text(s, graph_kw, &kw_text) == MG_OK,
          "get_keyword_text ok");
    CHECK(kw_text && strcmp(kw_text, "graph") == 0, "keyword text is 'graph'");
    free(kw_text);
    printf("ok get_keyword_text\n");
  }

  /* --- 9. node_keywords (link table) --- */
  {
    mg_node_t kw_node;
    memset(&kw_node, 0, sizeof(kw_node));
    mg_uuidv7(kw_node.id);
    mg_blake3((const uint8_t *)"kw-node", 7, kw_node.content_hash);
    kw_node.title = (char *)"kw node";
    kw_node.body  = (char *)"kw node body";
    kw_node.created_at = 10;
    kw_node.state = MG_NODE_ACTIVE;
    mg_keyword_id_t kw_ids[1] = { graph_kw };
    CHECK(mg_storage_insert_node_with_edges(s, &kw_node, emb, kw_ids, 1, NULL, 0, NULL) == MG_OK,
          "insert node with keyword");

    mg_keyword_id_t out_kw[4];
    int kw_count = 0;
    CHECK(mg_storage_node_keywords(s, kw_node.id, out_kw, 4, &kw_count) == MG_OK,
          "node_keywords ok");
    CHECK(kw_count == 1 && out_kw[0] == graph_kw, "node carries graph keyword");
    printf("ok node_keywords\n");
  }

  /* --- 10. vector_topk_by_keyword --- */
  {
    /* All inserted nodes that carry graph_kw should appear in this topk. */
    mg_node_t kw_node2;
    memset(&kw_node2, 0, sizeof(kw_node2));
    mg_embedding_t emb2;
    make_embedding2(emb2);
    mg_uuidv7(kw_node2.id);
    mg_blake3((const uint8_t *)"kw-node2", 8, kw_node2.content_hash);
    kw_node2.title = (char *)"kw node2";
    kw_node2.body  = (char *)"kw node2 body";
    kw_node2.created_at = 11;
    kw_node2.state = MG_NODE_ACTIVE;
    mg_keyword_id_t kw_ids2[1] = { graph_kw };
    CHECK(mg_storage_insert_node_with_edges(s, &kw_node2, emb2, kw_ids2, 1, NULL, 0, NULL) == MG_OK,
          "insert second kw node");

    int topk_count = 0;
    mg_node_score_t topk[4];
    CHECK(mg_storage_vector_topk_by_keyword(s, emb, graph_kw, 4, topk, &topk_count) == MG_OK,
          "topk_by_keyword ok");
    CHECK(topk_count >= 1, "topk_by_keyword returns at least 1 result");
    /* All results must be nodes that carry graph_kw. */
    printf("ok vector_topk_by_keyword\n");
  }

  /* --- 11. touch_access --- */
  {
    err = mg_storage_get_node(s, node.id, &got);
    CHECK(err == MG_OK, "get node before touch");
    int64_t old_access = got.access_count;
    mg_node_free(&got);

    CHECK(mg_storage_touch_access(s, node.id) == MG_OK, "touch_access ok");

    err = mg_storage_get_node(s, node.id, &got);
    CHECK(err == MG_OK, "get node after touch");
    CHECK(got.access_count == old_access + 1, "access_count incremented");
    mg_node_free(&got);
    printf("ok touch_access\n");
  }

  /* --- 12. get_embedding --- */
  {
    mg_embedding_t stored;
    CHECK(mg_storage_get_embedding(s, node.id, stored) == MG_OK, "get_embedding ok");
    /* The stored embedding must be (approximately) the same as what was inserted. */
    float dot = 0.0f;
    for (size_t i = 0; i < MG_EMBEDDING_DIM; i++) dot += emb[i] * stored[i];
    CHECK(fabsf(dot - 1.0f) < 0.001f, "get_embedding: cosine similarity ~1");
    printf("ok get_embedding\n");
  }

  /* --- 13. delete_node --- */
  {
    /* Insert a disposable node and delete it. */
    mg_node_t del_node;
    memset(&del_node, 0, sizeof(del_node));
    mg_uuidv7(del_node.id);
    mg_blake3((const uint8_t *)"delete-me", 9, del_node.content_hash);
    del_node.title = (char *)"delete me";
    del_node.body  = (char *)"delete me body";
    del_node.created_at = 20;
    del_node.state = MG_NODE_ACTIVE;
    CHECK(mg_storage_insert_node_with_edges(s, &del_node, emb, NULL, 0, NULL, 0, NULL) == MG_OK,
          "insert node for deletion");
    CHECK(mg_storage_delete_node(s, del_node.id) == MG_OK, "delete_node ok");
    /* After deletion get_node must return NOT_FOUND. */
    err = mg_storage_get_node(s, del_node.id, &got);
    if (err == MG_OK) mg_node_free(&got);
    CHECK(err == MG_ERR_NOT_FOUND, "delete_node: node not retrievable after deletion");
    /* Deleting a non-existent node must return NOT_FOUND. */
    CHECK(mg_storage_delete_node(s, del_node.id) == MG_ERR_NOT_FOUND,
          "delete_node: NOT_FOUND on re-delete");
    printf("ok delete_node\n");
  }

  /* --- 14. supersedes --- */
  {
    mg_node_t old_node, new_node;
    memset(&old_node, 0, sizeof(old_node));
    memset(&new_node, 0, sizeof(new_node));
    mg_uuidv7(old_node.id);
    mg_blake3((const uint8_t *)"old-superseded", 14, old_node.content_hash);
    old_node.title = (char *)"old superseded";
    old_node.body  = (char *)"old body";
    old_node.created_at = 30;
    old_node.state = MG_NODE_ACTIVE;
    CHECK(mg_storage_insert_node_with_edges(s, &old_node, emb, NULL, 0, NULL, 0, NULL) == MG_OK,
          "insert old node for supersedes");

    mg_uuidv7(new_node.id);
    mg_blake3((const uint8_t *)"new-superseder", 14, new_node.content_hash);
    new_node.title = (char *)"new superseder";
    new_node.body  = (char *)"new body";
    new_node.created_at = 31;
    new_node.state = MG_NODE_ACTIVE;
    CHECK(mg_storage_insert_node_with_edges(s, &new_node, emb, NULL, 0, NULL, 0, &old_node.id) == MG_OK,
          "insert node with supersedes");

    /* Old node must now be SUPERSEDED. */
    err = mg_storage_get_node(s, old_node.id, &got);
    CHECK(err == MG_OK, "get superseded node");
    CHECK(got.state == MG_NODE_SUPERSEDED, "old node state is SUPERSEDED");
    mg_node_free(&got);
    printf("ok supersedes\n");
  }

  /* --- 15. consolidate --- */
  {
    mg_storage_consolidate_report_t report;
    memset(&report, 0, sizeof(report));
    CHECK(mg_storage_consolidate(s, &report) == MG_OK, "consolidate ok");
    /* With data present, node/edge counts should be > 0. */
    CHECK(report.n_nodes > 0, "consolidate: n_nodes > 0");
    /* No contradictions expected in this test graph. */
    CHECK(report.contradictions_found == 0, "consolidate: no contradictions");
    printf("ok consolidate\n");
  }

  /* --- 16. distribution_percentiles for edges (kind=1) --- */
  {
    float pct[6];
    /* Insert a sample for edges. */
    CHECK(mg_storage_record_sample(s, 1, 0.75f) == MG_OK, "record edge sample");
    CHECK(mg_storage_distribution_percentiles(s, 1, pct) == MG_OK,
          "distribution_percentiles edges");
    printf("ok edge percentiles\n");
  }

  mg_storage_close(s);
  remove(path);
  remove("./test_storage.db-wal");
  remove("./test_storage.db-shm");

  if (g_fail > 0) {
    fprintf(stderr, "test_storage: %d assertion(s) failed\n", g_fail);
    return 1;
  }
  return 0;
}
