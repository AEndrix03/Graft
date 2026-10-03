#include "graft/storage.h"
#include "graft/types.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_embedding(mg_embedding_t e, float seed) {
  for (int i = 0; i < MG_EMBEDDING_DIM; ++i) e[i] = seed + (float)i * 0.0001f;
}

static void fill_node(mg_node_t *n, const char *title, int seed) {
  memset(n, 0, sizeof(*n));
  mg_uuidv7(n->id);
  mg_blake3((const uint8_t *)title, strlen(title), n->content_hash);
  n->title = (char *)title;
  n->body = (char *)"body";
  n->created_at = seed;
  n->last_access = seed;
  n->state = MG_NODE_ACTIVE;
}

static int get_origin(const char *path, const mg_node_id_t id, int *origin) {
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
  rc = sqlite3_prepare_v2(db, "SELECT origin FROM nodes WHERE id=?;", -1, &stmt, NULL);
  if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *origin = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return 1;
}

static int open_schema(const char *path, mg_storage_t **out) {
  if (mg_storage_open(path, out) != MG_OK) return -1;
  if (mg_storage_apply_schema(*out) != MG_OK) {
    mg_storage_close(*out);
    *out = NULL;
    return -1;
  }
  return 0;
}

static void cleanup(const char *path) {
  char wal[512], shm[512];
  remove(path);
  snprintf(wal, sizeof(wal), "%s-wal", path);
  snprintf(shm, sizeof(shm), "%s-shm", path);
  remove(wal);
  remove(shm);
}

/* ---- Part 1: pull_remote_file (original tests) ---- */

static int test_pull(void) {
  const char *local_path  = "./test_remote_local.db";
  const char *remote_path = "./test_remote_remote.db";
  cleanup(local_path);
  cleanup(remote_path);

  mg_storage_t *local = NULL, *remote = NULL;
  mg_node_t remote_node, local_node;
  mg_embedding_t emb;
  int64_t inserted = 0, deleted = 0, pushed = 0;
  int origin = -1;

  if (open_schema(remote_path, &remote) != 0) return 1;
  fill_embedding(emb, 1.0f);
  fill_node(&remote_node, "remote", 1);
  if (mg_storage_insert_node_with_edges(remote, &remote_node, emb, NULL, 0, NULL, 0, NULL) != MG_OK) return 1;
  mg_storage_close(remote);

  if (open_schema(local_path, &local) != 0) return 1;
  if (mg_storage_pull_remote_file(local, remote_path, &inserted, &deleted) != MG_OK) return 1;
  mg_storage_close(local);
  if (inserted != 1 || deleted != 0) { fprintf(stderr, "pull: expected inserted=1 deleted=0, got %lld/%lld\n", (long long)inserted, (long long)deleted); return 1; }
  if (get_origin(local_path, remote_node.id, &origin) != 0 || origin != 1) { fprintf(stderr, "pull: origin!=1\n"); return 1; }

  /* Remote deletes node -> pull should mark it deleted locally. */
  if (open_schema(remote_path, &remote) != 0) return 1;
  if (mg_storage_delete_node(remote, remote_node.id) != MG_OK) return 1;
  mg_storage_close(remote);
  if (open_schema(local_path, &local) != 0) return 1;
  if (mg_storage_pull_remote_file(local, remote_path, &inserted, &deleted) != MG_OK) return 1;
  mg_storage_close(local);
  if (deleted != 1) { fprintf(stderr, "pull delete: expected deleted=1, got %lld\n", (long long)deleted); return 1; }

  /* Local node must not be overwritten by a pull. */
  if (open_schema(local_path, &local) != 0) return 1;
  fill_node(&local_node, "local", 2);
  if (mg_storage_insert_node_with_edges(local, &local_node, emb, NULL, 0, NULL, 0, NULL) != MG_OK) return 1;
  if (mg_storage_pull_remote_file(local, remote_path, &inserted, &deleted) != MG_OK) return 1;
  mg_storage_close(local);
  if (deleted != 0 || get_origin(local_path, local_node.id, &origin) != 0 || origin != 0) {
    fprintf(stderr, "pull: local node's origin was changed\n"); return 1;
  }

  /* mark_local_pushed then pull: pushed node treated as local-done, not re-deleted. */
  if (open_schema(local_path, &local) != 0) return 1;
  if (mg_storage_mark_local_pushed(local, &pushed) != MG_OK) return 1;
  if (mg_storage_pull_remote_file(local, remote_path, &inserted, &deleted) != MG_OK) return 1;
  mg_storage_close(local);
  if (pushed != 1 || deleted != 1) {
    fprintf(stderr, "pull after push: pushed=%lld deleted=%lld\n", (long long)pushed, (long long)deleted);
    return 1;
  }

  cleanup(local_path);
  cleanup(remote_path);
  printf("ok pull_remote_file\n");
  return 0;
}

/* ---- Part 2: push_to_remote_file ---- */

static int test_push(void) {
  const char *local_path  = "./test_push_local.db";
  const char *remote_path = "./test_push_remote.db";
  cleanup(local_path);
  cleanup(remote_path);

  mg_storage_t *local = NULL, *remote = NULL;
  mg_node_t n1, n2;
  mg_embedding_t emb;
  fill_embedding(emb, 2.0f);

  /* Populate local with two nodes. */
  if (open_schema(local_path, &local) != 0) return 1;
  fill_node(&n1, "push-node-1", 10);
  fill_node(&n2, "push-node-2", 11);
  if (mg_storage_insert_node_with_edges(local, &n1, emb, NULL, 0, NULL, 0, NULL) != MG_OK) return 1;
  if (mg_storage_insert_node_with_edges(local, &n2, emb, NULL, 0, NULL, 0, NULL) != MG_OK) return 1;
  mg_storage_close(local);

  /* Create (empty) remote. */
  if (open_schema(remote_path, &remote) != 0) return 1;
  mg_storage_close(remote);

  /* Push from local to remote. */
  if (open_schema(local_path, &local) != 0) return 1;
  int64_t pushed = 0;
  if (mg_storage_push_to_remote_file(local, remote_path, &pushed) != MG_OK) {
    fprintf(stderr, "push_to_remote_file failed\n");
    mg_storage_close(local);
    cleanup(local_path); cleanup(remote_path);
    return 1;
  }
  mg_storage_close(local);
  if (pushed != 2) {
    fprintf(stderr, "push: expected pushed=2, got %lld\n", (long long)pushed);
    cleanup(local_path); cleanup(remote_path);
    return 1;
  }

  /* Remote must now have the two nodes. */
  if (open_schema(remote_path, &remote) != 0) return 1;
  mg_node_t got;
  mg_err_t err1 = mg_storage_get_node(remote, n1.id, &got);
  if (err1 == MG_OK) mg_node_free(&got);
  mg_err_t err2 = mg_storage_get_node(remote, n2.id, &got);
  if (err2 == MG_OK) mg_node_free(&got);
  mg_storage_close(remote);
  if (err1 != MG_OK || err2 != MG_OK) {
    fprintf(stderr, "push: pushed nodes not found in remote\n");
    cleanup(local_path); cleanup(remote_path);
    return 1;
  }

  /* Origin in local must now be PUSHED (2). */
  int origin = -1;
  if (get_origin(local_path, n1.id, &origin) != 0 || origin != 2) {
    fprintf(stderr, "push: local origin not updated to PUSHED (got %d)\n", origin);
    cleanup(local_path); cleanup(remote_path);
    return 1;
  }

  /* Second push of the same nodes: pushed=0 (already marked). */
  if (open_schema(local_path, &local) != 0) return 1;
  pushed = 99;
  if (mg_storage_push_to_remote_file(local, remote_path, &pushed) != MG_OK) {
    fprintf(stderr, "second push failed\n");
    mg_storage_close(local);
    cleanup(local_path); cleanup(remote_path);
    return 1;
  }
  mg_storage_close(local);
  if (pushed != 0) {
    fprintf(stderr, "second push: expected pushed=0, got %lld\n", (long long)pushed);
    cleanup(local_path); cleanup(remote_path);
    return 1;
  }

  cleanup(local_path);
  cleanup(remote_path);
  printf("ok push_to_remote_file\n");
  return 0;
}

/* ---- Part 3: merge_from ---- */

static int test_merge(void) {
  const char *src_path = "./test_merge_src.db";
  const char *dst_path = "./test_merge_dst.db";
  cleanup(src_path);
  cleanup(dst_path);

  mg_storage_t *src = NULL, *dst = NULL;
  mg_node_t na, nb;
  mg_embedding_t emb;
  fill_embedding(emb, 3.0f);

  /* Source has two nodes. */
  if (open_schema(src_path, &src) != 0) return 1;
  fill_node(&na, "merge-a", 20);
  fill_node(&nb, "merge-b", 21);
  if (mg_storage_insert_node_with_edges(src, &na, emb, NULL, 0, NULL, 0, NULL) != MG_OK) return 1;
  if (mg_storage_insert_node_with_edges(src, &nb, emb, NULL, 0, NULL, 0, NULL) != MG_OK) return 1;
  mg_storage_close(src);

  /* Destination is empty. Merge overwrite=0. */
  if (open_schema(dst_path, &dst) != 0) return 1;
  if (mg_storage_merge_from(dst, src_path, 0) != MG_OK) {
    fprintf(stderr, "merge_from failed\n");
    mg_storage_close(dst);
    cleanup(src_path); cleanup(dst_path);
    return 1;
  }

  /* Both nodes must appear in destination. */
  mg_node_t got;
  mg_err_t ea = mg_storage_get_node(dst, na.id, &got);
  if (ea == MG_OK) mg_node_free(&got);
  mg_err_t eb = mg_storage_get_node(dst, nb.id, &got);
  if (eb == MG_OK) mg_node_free(&got);
  if (ea != MG_OK || eb != MG_OK) {
    fprintf(stderr, "merge: nodes not found in dst\n");
    mg_storage_close(dst);
    cleanup(src_path); cleanup(dst_path);
    return 1;
  }

  /* Idempotent: merging again (overwrite=0) must not fail. */
  if (mg_storage_merge_from(dst, src_path, 0) != MG_OK) {
    fprintf(stderr, "merge idempotent failed\n");
    mg_storage_close(dst);
    cleanup(src_path); cleanup(dst_path);
    return 1;
  }

  /* overwrite=1 must also succeed. */
  if (mg_storage_merge_from(dst, src_path, 1) != MG_OK) {
    fprintf(stderr, "merge overwrite=1 failed\n");
    mg_storage_close(dst);
    cleanup(src_path); cleanup(dst_path);
    return 1;
  }

  mg_storage_close(dst);
  cleanup(src_path);
  cleanup(dst_path);
  printf("ok merge_from\n");
  return 0;
}

/* Runs `sql` against the DB at `path` with up to two blob parameters and
 * returns the first column of the first row as an integer, or -1. */
static int64_t query_int(const char *path, const char *sql,
                         const void *b1, int n1, const void *b2, int n2) {
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  int64_t out = -1;
  if (sqlite3_open(path, &db) != SQLITE_OK) { sqlite3_close(db); return -1; }
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    if (b1) sqlite3_bind_blob(stmt, 1, b1, n1, SQLITE_STATIC);
    if (b2) sqlite3_bind_blob(stmt, 2, b2, n2, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) out = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return out;
}

#define ID_ARG(x) (x), MG_NODE_ID_BYTES
#define EXPECT_EQ(what, got, want) do { \
    int64_t g_ = (got), w_ = (want); \
    if (g_ != w_) { \
      fprintf(stderr, "merge identity (overwrite=%d): %s: got %lld, want %lld\n", \
              overwrite, (what), (long long)g_, (long long)w_); \
      ok = 0; \
    } \
  } while (0)

/* Issue #11: the target holds node A (hash H) with a target-only edge
 * X -> A; the source holds node B with the same hash H plus a source-only
 * node Y with an edge Y -> B. The merge must keep A's id and its edge,
 * leave exactly one H node, and remap Y -> B onto Y -> A. overwrite=1
 * adopts B's metadata on A; overwrite=0 keeps A's. */
static int test_merge_identity(int overwrite) {
  const char *src_path = "./test_merge_id_src.db";
  const char *dst_path = "./test_merge_id_dst.db";
  cleanup(src_path);
  cleanup(dst_path);

  mg_storage_t *src = NULL, *dst = NULL;
  mg_node_t na, nx, nb, ny;
  mg_embedding_t emb;
  mg_keyword_id_t kw = 0;
  fill_embedding(emb, 4.0f);

  /* Target: A (keyword "merge-dst-kw") and X with edge X -> A. */
  if (open_schema(dst_path, &dst) != 0) return 1;
  fill_node(&na, "merge-shared", 100);
  na.author = (char *)"dst-author";
  na.access_count = 3;
  fill_node(&nx, "merge-x", 101);
  if (mg_storage_upsert_keyword(dst, "merge-dst-kw", NULL, &kw) != MG_OK) return 1;
  if (mg_storage_insert_node_with_edges(dst, &na, emb, &kw, 1, NULL, 0, NULL) != MG_OK) return 1;
  mg_edge_t ex = {{0}, {0}, MG_EDGE_SEMANTIC, 0, 0.9f};
  memcpy(ex.src, nx.id, MG_NODE_ID_BYTES);
  memcpy(ex.dst, na.id, MG_NODE_ID_BYTES);
  if (mg_storage_insert_node_with_edges(dst, &nx, emb, NULL, 0, &ex, 1, NULL) != MG_OK) return 1;
  mg_storage_close(dst);

  /* Source: B (same hash as A, new id, keyword "merge-src-kw") and Y with
   * edge Y -> B. */
  if (open_schema(src_path, &src) != 0) return 1;
  fill_node(&nb, "merge-shared", 500);
  nb.author = (char *)"src-author";
  nb.access_count = 7;
  nb.state = MG_NODE_STALE;
  fill_node(&ny, "merge-y", 501);
  if (mg_storage_upsert_keyword(src, "merge-src-kw", NULL, &kw) != MG_OK) return 1;
  if (mg_storage_insert_node_with_edges(src, &nb, emb, &kw, 1, NULL, 0, NULL) != MG_OK) return 1;
  mg_edge_t ey = {{0}, {0}, MG_EDGE_SEMANTIC, 0, 0.8f};
  memcpy(ey.src, ny.id, MG_NODE_ID_BYTES);
  memcpy(ey.dst, nb.id, MG_NODE_ID_BYTES);
  if (mg_storage_insert_node_with_edges(src, &ny, emb, NULL, 0, &ey, 1, NULL) != MG_OK) return 1;
  mg_storage_close(src);

  if (open_schema(dst_path, &dst) != 0) return 1;
  if (mg_storage_merge_from(dst, src_path, overwrite) != MG_OK) {
    fprintf(stderr, "merge identity (overwrite=%d): merge_from failed\n", overwrite);
    mg_storage_close(dst);
    cleanup(src_path); cleanup(dst_path);
    return 1;
  }
  /* A second pass must be a no-op for the graph shape. */
  if (mg_storage_merge_from(dst, src_path, overwrite) != MG_OK) {
    fprintf(stderr, "merge identity (overwrite=%d): second merge failed\n", overwrite);
    mg_storage_close(dst);
    cleanup(src_path); cleanup(dst_path);
    return 1;
  }

  int ok = 1;
  mg_node_t got;
  if (mg_storage_get_node(dst, na.id, &got) != MG_OK) {
    fprintf(stderr, "merge identity (overwrite=%d): node A lost\n", overwrite);
    ok = 0;
  } else {
    EXPECT_EQ("A created_at", got.created_at, overwrite ? 500 : 100);
    EXPECT_EQ("A access_count", got.access_count, overwrite ? 7 : 3);
    EXPECT_EQ("A state", got.state, overwrite ? MG_NODE_STALE : MG_NODE_ACTIVE);
    EXPECT_EQ("A author",
              got.author && strcmp(got.author, overwrite ? "src-author" : "dst-author") == 0,
              1);
    mg_node_free(&got);
  }
  mg_storage_close(dst);

  EXPECT_EQ("nodes with hash H",
            query_int(dst_path, "SELECT COUNT(*) FROM nodes WHERE content_hash = ?;",
                      na.content_hash, MG_HASH_BYTES, NULL, 0), 1);
  EXPECT_EQ("node B present",
            query_int(dst_path, "SELECT COUNT(*) FROM nodes WHERE id = ?;",
                      ID_ARG(nb.id), NULL, 0), 0);
  EXPECT_EQ("total nodes",
            query_int(dst_path, "SELECT COUNT(*) FROM nodes;", NULL, 0, NULL, 0), 3);
  EXPECT_EQ("edge X -> A",
            query_int(dst_path, "SELECT COUNT(*) FROM edges WHERE src = ? AND dst = ?;",
                      ID_ARG(nx.id), ID_ARG(na.id)), 1);
  EXPECT_EQ("edge Y -> A",
            query_int(dst_path, "SELECT COUNT(*) FROM edges WHERE src = ? AND dst = ?;",
                      ID_ARG(ny.id), ID_ARG(na.id)), 1);
  EXPECT_EQ("edges touching B",
            query_int(dst_path, "SELECT COUNT(*) FROM edges WHERE src = ?1 OR dst = ?1;",
                      ID_ARG(nb.id), NULL, 0), 0);
  EXPECT_EQ("total edges",
            query_int(dst_path, "SELECT COUNT(*) FROM edges;", NULL, 0, NULL, 0), 2);
  EXPECT_EQ("keywords on A",
            query_int(dst_path, "SELECT COUNT(*) FROM node_keywords WHERE node_id = ?;",
                      ID_ARG(na.id), NULL, 0), 2);
  EXPECT_EQ("node_vec for B",
            query_int(dst_path, "SELECT COUNT(*) FROM node_vec WHERE id = ?;",
                      ID_ARG(nb.id), NULL, 0), 0);
  EXPECT_EQ("node_vec for Y",
            query_int(dst_path, "SELECT COUNT(*) FROM node_vec WHERE id = ?;",
                      ID_ARG(ny.id), NULL, 0), 1);

  cleanup(src_path);
  cleanup(dst_path);
  if (!ok) return 1;
  printf("ok merge_from identity (overwrite=%d)\n", overwrite);
  return 0;
}

int main(void) {
  int rc = 0;
  rc |= test_pull();
  rc |= test_push();
  rc |= test_merge();
  rc |= test_merge_identity(0);
  rc |= test_merge_identity(1);
  if (rc == 0) printf("test_remote_profiles: PASS\n");
  return rc;
}
