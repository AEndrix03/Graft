#include "memgraph/storage.h"
#include "memgraph/types.h"
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

int main(void) {
  const char *local_path = "./test_remote_local.db";
  const char *remote_path = "./test_remote_remote.db";
  remove(local_path); remove("./test_remote_local.db-wal"); remove("./test_remote_local.db-shm");
  remove(remote_path); remove("./test_remote_remote.db-wal"); remove("./test_remote_remote.db-shm");

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
  if (inserted != 1 || deleted != 0) return 1;
  if (get_origin(local_path, remote_node.id, &origin) != 0 || origin != 1) return 1;

  if (open_schema(remote_path, &remote) != 0) return 1;
  if (mg_storage_delete_node(remote, remote_node.id) != MG_OK) return 1;
  mg_storage_close(remote);
  if (open_schema(local_path, &local) != 0) return 1;
  if (mg_storage_pull_remote_file(local, remote_path, &inserted, &deleted) != MG_OK) return 1;
  mg_storage_close(local);
  if (deleted != 1) return 1;

  if (open_schema(local_path, &local) != 0) return 1;
  fill_node(&local_node, "local", 2);
  if (mg_storage_insert_node_with_edges(local, &local_node, emb, NULL, 0, NULL, 0, NULL) != MG_OK) return 1;
  if (mg_storage_pull_remote_file(local, remote_path, &inserted, &deleted) != MG_OK) return 1;
  mg_storage_close(local);
  if (deleted != 0 || get_origin(local_path, local_node.id, &origin) != 0 || origin != 0) return 1;

  if (open_schema(local_path, &local) != 0) return 1;
  if (mg_storage_mark_local_pushed(local, &pushed) != MG_OK) return 1;
  if (mg_storage_pull_remote_file(local, remote_path, &inserted, &deleted) != MG_OK) return 1;
  mg_storage_close(local);
  if (pushed != 1 || deleted != 1) return 1;

  remove(local_path); remove("./test_remote_local.db-wal"); remove("./test_remote_local.db-shm");
  remove(remote_path); remove("./test_remote_remote.db-wal"); remove("./test_remote_remote.db-shm");
  return 0;
}
