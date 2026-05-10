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

  mg_storage_close(s);
  remove(path);
  remove("./test_storage.db-wal");
  remove("./test_storage.db-shm");
  return 0;
}
