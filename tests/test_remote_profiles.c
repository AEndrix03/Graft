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

int main(void) {
  int rc = 0;
  rc |= test_pull();
  rc |= test_push();
  rc |= test_merge();
  if (rc == 0) printf("test_remote_profiles: PASS\n");
  return rc;
}
