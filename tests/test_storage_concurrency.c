/* Regression test for #7: daemon workers share one mg_storage_t, so storage
 * calls from several threads must never interleave their transactions on the
 * shared SQLite connection. Without the storage mutex this test fails with
 * "cannot start a transaction within a transaction" style errors. */
#include "graft/storage.h"

#include <pthread.h>
#include <sched.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_WORKERS      6
#define N_ITERS        40
#define N_DUP_THREADS  6
#define N_DUP_ROUNDS   20

static mg_storage_t *g_s = NULL;
static atomic_int g_fail = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "test_storage_concurrency: %s\n", msg); \
    atomic_fetch_add(&g_fail, 1); \
  } \
} while (0)

#define CHECK_OK(err, what) do { \
  mg_err_t e_ = (err); \
  if (e_ != MG_OK) { \
    fprintf(stderr, "test_storage_concurrency: %s: %s\n", what, mg_strerror(e_)); \
    atomic_fetch_add(&g_fail, 1); \
  } \
} while (0)

static void make_embedding(mg_embedding_t e, int seed) {
  float norm = 0.0f;
  size_t i;
  for (i = 0; i < MG_EMBEDDING_DIM; ++i) {
    e[i] = (float)(((i + (size_t)seed) % 17) + 1);
    norm += e[i] * e[i];
  }
  norm = sqrtf(norm);
  for (i = 0; i < MG_EMBEDDING_DIM; ++i) {
    e[i] /= norm;
  }
}

static void fill_node(mg_node_t *node, const char *key, char *title, size_t title_cap) {
  memset(node, 0, sizeof(*node));
  mg_uuidv7(node->id);
  mg_blake3((const uint8_t *)key, strlen(key), node->content_hash);
  snprintf(title, title_cap, "alpha %s", key);
  node->title = title;
  node->body = (char *)"concurrent body";
  node->created_at = 1;
  node->last_access = 1;
  node->state = MG_NODE_ACTIVE;
}

/* --- 1. Mixed workload: inserts with edges, touches, reads, consolidate --- */

static void *worker(void *arg) {
  int t = (int)(intptr_t)arg;
  mg_node_id_t prev;
  int have_prev = 0;
  char kw_text[32];
  mg_keyword_id_t kw = 0;
  int i;

  snprintf(kw_text, sizeof(kw_text), "kw-%d", t % 3);
  CHECK_OK(mg_storage_upsert_keyword(g_s, kw_text, NULL, &kw), "upsert_keyword");

  for (i = 0; i < N_ITERS; ++i) {
    char key[64];
    char title[96];
    mg_node_t node;
    mg_node_t got;
    mg_embedding_t emb;
    mg_edge_t edge;
    mg_node_score_t scores[8];
    mg_edge_t nbr[8];
    int count = 0;

    snprintf(key, sizeof(key), "w%d-i%d", t, i);
    fill_node(&node, key, title, sizeof(title));
    make_embedding(emb, t * N_ITERS + i);

    memset(&edge, 0, sizeof(edge));
    if (have_prev) {
      memcpy(edge.src, node.id, MG_NODE_ID_BYTES);
      memcpy(edge.dst, prev, MG_NODE_ID_BYTES);
      edge.kind = MG_EDGE_KEYWORD;
      edge.keyword_id = kw;
      edge.weight = 0.5f;
    }
    CHECK_OK(mg_storage_insert_node_with_edges(g_s, &node, emb, &kw, 1,
                                               have_prev ? &edge : NULL,
                                               have_prev ? 1 : 0, NULL),
             "insert_node_with_edges");

    CHECK_OK(mg_storage_touch_access(g_s, node.id), "touch_access");
    if (mg_storage_get_node(g_s, node.id, &got) == MG_OK) {
      CHECK(strcmp(got.title, title) == 0, "get_node returned the wrong title");
      mg_node_free(&got);
    } else {
      CHECK(0, "get_node failed");
    }
    CHECK_OK(mg_storage_vector_topk(g_s, emb, 8, scores, &count), "vector_topk");
    CHECK_OK(mg_storage_vector_topk_by_keyword(g_s, emb, kw, 8, scores, &count),
             "vector_topk_by_keyword");
    CHECK_OK(mg_storage_fts_search(g_s, "alpha", 8, true, true, scores, &count), "fts_search");
    CHECK_OK(mg_storage_neighbors(g_s, node.id, -1, NULL, 0, nbr, 8, &count), "neighbors");
    CHECK_OK(mg_storage_record_sample(g_s, 0, 0.5f), "record_sample");

    if (i % 10 == 9) {
      mg_storage_consolidate_report_t rep;
      CHECK_OK(mg_storage_consolidate(g_s, &rep), "consolidate");
    }
    /* Exercise the delete transaction too: drop every 8th node right away. */
    if (i % 8 == 7) {
      CHECK_OK(mg_storage_delete_node(g_s, node.id), "delete_node");
      continue;
    }
    memcpy(prev, node.id, MG_NODE_ID_BYTES);
    have_prev = 1;
  }
  return NULL;
}

/* --- 2. Identical concurrent inserts --- */

/* Reusable spin barrier shared by the dup workers and main (pthread_barrier_t
 * is missing on macOS). */
static atomic_int g_bar_count = 0;
static atomic_int g_bar_gen = 0;

static void barrier_wait(void) {
  int gen = atomic_load(&g_bar_gen);
  if (atomic_fetch_add(&g_bar_count, 1) == N_DUP_THREADS) {
    atomic_store(&g_bar_count, 0);
    atomic_fetch_add(&g_bar_gen, 1);
    return;
  }
  while (atomic_load(&g_bar_gen) == gen) {
    sched_yield();
  }
}

static mg_node_id_t g_resolved[N_DUP_THREADS];
static int g_inserted[N_DUP_THREADS];

static void *dup_worker(void *arg) {
  int t = (int)(intptr_t)arg;
  int round;
  for (round = 0; round < N_DUP_ROUNDS; ++round) {
    char key[32];
    char title[64];
    mg_node_t node;
    mg_embedding_t emb;
    mg_node_id_t found;
    mg_err_t err;

    /* Line every thread up on the same round so they race on one hash. */
    barrier_wait();

    snprintf(key, sizeof(key), "dup-%d", round);
    fill_node(&node, key, title, sizeof(title));
    make_embedding(emb, round);
    g_inserted[t] = 0;

    /* Same sequence as mg_op_insert: hash lookup, then insert, and on a
     * UNIQUE content_hash conflict resolve to the node that won. */
    err = mg_storage_node_id_by_hash(g_s, node.content_hash, found);
    if (err == MG_ERR_NOT_FOUND) {
      err = mg_storage_insert_node_with_edges(g_s, &node, emb, NULL, 0, NULL, 0, NULL);
      if (err == MG_OK) {
        memcpy(found, node.id, MG_NODE_ID_BYTES);
        g_inserted[t] = 1;
      } else if (err == MG_ERR_DUPLICATE) {
        err = mg_storage_node_id_by_hash(g_s, node.content_hash, found);
      }
    }
    CHECK_OK(err, "identical insert did not resolve");
    memcpy(g_resolved[t], found, MG_NODE_ID_BYTES);

    barrier_wait();
  }
  return NULL;
}

int main(void) {
  const char *path = "./test_storage_concurrency.db";
  pthread_t th[N_WORKERS > N_DUP_THREADS ? N_WORKERS : N_DUP_THREADS];
  int64_t n_nodes = 0;
  int i;
  int round;
  mg_err_t err;

  remove(path);
  remove("./test_storage_concurrency.db-wal");
  remove("./test_storage_concurrency.db-shm");

  err = mg_storage_open(path, &g_s);
  if (err != MG_OK) {
    fprintf(stderr, "open: %s\n", mg_strerror(err));
    return 1;
  }
  err = mg_storage_apply_schema(g_s);
  if (err != MG_OK) {
    fprintf(stderr, "schema: %s\n", mg_strerror(err));
    mg_storage_close(g_s);
    return 1;
  }

  for (i = 0; i < N_WORKERS; ++i) {
    if (pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      return 1;
    }
  }
  for (i = 0; i < N_WORKERS; ++i) {
    pthread_join(th[i], NULL);
  }

  /* Every 8th insert of each worker was deleted again. */
  CHECK_OK(mg_storage_count(g_s, MG_STORAGE_COUNT_NODES, &n_nodes), "count");
  CHECK(n_nodes == (int64_t)N_WORKERS * (N_ITERS - N_ITERS / 8),
        "unexpected node count after the mixed workload");

  for (i = 0; i < N_DUP_THREADS; ++i) {
    if (pthread_create(&th[i], NULL, dup_worker, (void *)(intptr_t)i) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      return 1;
    }
  }
  for (round = 0; round < N_DUP_ROUNDS; ++round) {
    int winners = 0;
    barrier_wait(); /* start of the round */
    barrier_wait(); /* every thread resolved its insert */
    for (i = 0; i < N_DUP_THREADS; ++i) {
      winners += g_inserted[i];
      CHECK(memcmp(g_resolved[i], g_resolved[0], MG_NODE_ID_BYTES) == 0,
            "identical inserts resolved to different nodes");
    }
    CHECK(winners == 1, "identical inserts must create exactly one node");
  }
  for (i = 0; i < N_DUP_THREADS; ++i) {
    pthread_join(th[i], NULL);
  }

  CHECK_OK(mg_storage_count(g_s, MG_STORAGE_COUNT_NODES, &n_nodes), "count");
  CHECK(n_nodes == (int64_t)N_WORKERS * (N_ITERS - N_ITERS / 8) + N_DUP_ROUNDS,
        "unexpected node count after the identical inserts");

  mg_storage_close(g_s);
  remove(path);
  remove("./test_storage_concurrency.db-wal");
  remove("./test_storage_concurrency.db-shm");

  if (atomic_load(&g_fail) != 0) {
    fprintf(stderr, "test_storage_concurrency: %d failure(s)\n", atomic_load(&g_fail));
    return 1;
  }
  printf("test_storage_concurrency: OK\n");
  return 0;
}
