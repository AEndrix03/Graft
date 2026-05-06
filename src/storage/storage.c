#include "memgraph/storage.h"

#include <sqlite3.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int sqlite3_vec_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);
extern const char *mg_storage_schema_sql(void);

struct mg_storage {
  sqlite3 *db;
};

static char *mg_strdup(const char *s) {
  size_t n;
  char *out;
  if (!s) {
    return NULL;
  }
  n = strlen(s) + 1;
  out = (char *)malloc(n);
  if (!out) {
    return NULL;
  }
  memcpy(out, s, n);
  return out;
}

static int64_t now_ms(void) {
  return (int64_t)time(NULL) * 1000;
}

static mg_err_t exec_sql(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    sqlite3_free(err);
    return MG_ERR_STORAGE;
  }
  return MG_OK;
}

static mg_err_t step_done(sqlite3_stmt *stmt) {
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    return MG_OK;
  }
  if (rc == SQLITE_CONSTRAINT) {
    return MG_ERR_DUPLICATE;
  }
  return MG_ERR_STORAGE;
}

static mg_err_t prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
  return sqlite3_prepare_v2(db, sql, -1, stmt, NULL) == SQLITE_OK ? MG_OK : MG_ERR_STORAGE;
}

static float cosine_score(const float *a, const float *b) {
  double dot = 0.0;
  double na = 0.0;
  double nb = 0.0;
  size_t i;
  for (i = 0; i < MG_EMBEDDING_DIM; ++i) {
    dot += (double)a[i] * (double)b[i];
    na += (double)a[i] * (double)a[i];
    nb += (double)b[i] * (double)b[i];
  }
  if (na <= 0.0 || nb <= 0.0) {
    return 0.0f;
  }
  return (float)(dot / (sqrt(na) * sqrt(nb)));
}

static void push_topk(mg_node_score_t *out, int *count, int k, const void *id, float score) {
  int pos;
  int limit;
  if (k <= 0) {
    return;
  }
  pos = *count;
  if (pos < k) {
    ++(*count);
  } else if (score <= out[k - 1].score) {
    return;
  } else {
    pos = k - 1;
  }
  while (pos > 0 && out[pos - 1].score < score) {
    out[pos] = out[pos - 1];
    --pos;
  }
  memcpy(out[pos].id, id, MG_NODE_ID_BYTES);
  out[pos].score = score;
  limit = *count < k ? *count : k;
  *count = limit;
}

static mg_err_t create_vec_table(sqlite3 *db) {
  mg_err_t err = exec_sql(db,
    "CREATE VIRTUAL TABLE IF NOT EXISTS node_vec USING vec0("
    "id BLOB PRIMARY KEY, embedding FLOAT[1024]);");
  if (err == MG_OK) {
    return MG_OK;
  }
  return exec_sql(db,
    "CREATE TABLE IF NOT EXISTS node_vec("
    "id BLOB PRIMARY KEY REFERENCES nodes(id) ON DELETE CASCADE,"
    "embedding BLOB NOT NULL);");
}

mg_err_t mg_storage_open(const char *db_path, mg_storage_t **out) {
  static int initialized = 0;
  mg_storage_t *s;

  if (!db_path || !out) {
    return MG_ERR_INVALID_ARG;
  }
  *out = NULL;
  if (!initialized) {
    sqlite3_auto_extension((void (*)(void))sqlite3_vec_init);
    initialized = 1;
  }
  s = (mg_storage_t *)calloc(1, sizeof(*s));
  if (!s) {
    return MG_ERR_OOM;
  }
  if (sqlite3_open_v2(db_path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
    mg_storage_close(s);
    return MG_ERR_STORAGE;
  }
  *out = s;
  return MG_OK;
}

void mg_storage_close(mg_storage_t *s) {
  if (!s) {
    return;
  }
  if (s->db) {
    sqlite3_close(s->db);
  }
  free(s);
}

mg_err_t mg_storage_apply_schema(mg_storage_t *s) {
  mg_err_t err;
  if (!s || !s->db) {
    return MG_ERR_INVALID_ARG;
  }
  err = exec_sql(s->db, mg_storage_schema_sql());
  if (err != MG_OK) {
    return err;
  }
  return create_vec_table(s->db);
}

mg_err_t mg_storage_insert_node_with_edges(
  mg_storage_t *s,
  const mg_node_t *node,
  const mg_embedding_t embedding,
  const mg_keyword_id_t *keyword_ids, size_t n_keywords,
  const mg_edge_t *edges, size_t n_edges
) {
  sqlite3_stmt *stmt = NULL;
  size_t i;
  mg_err_t err;
  if (!s || !node || !embedding || (!keyword_ids && n_keywords > 0) || (!edges && n_edges > 0) ||
      !node->summary || !node->detail) {
    return MG_ERR_INVALID_ARG;
  }
  err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    return err;
  }

  err = prepare(s->db, "INSERT INTO nodes(id,content_hash,summary,detail,created_at,last_access,access_count,state) VALUES(?,?,?,?,?,?,?,?);", &stmt);
  if (err == MG_OK) {
    sqlite3_bind_blob(stmt, 1, node->id, MG_NODE_ID_BYTES, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, node->content_hash, MG_HASH_BYTES, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, node->summary, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, node->detail, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, node->created_at);
    sqlite3_bind_int64(stmt, 6, node->last_access);
    sqlite3_bind_int64(stmt, 7, node->access_count);
    sqlite3_bind_int(stmt, 8, (int)node->state);
    err = step_done(stmt);
  }
  sqlite3_finalize(stmt);

  if (err == MG_OK) {
    err = prepare(s->db, "INSERT INTO node_vec(id,embedding) VALUES(?,?);", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_blob(stmt, 1, node->id, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_blob(stmt, 2, embedding, (int)sizeof(mg_embedding_t), SQLITE_STATIC);
      err = step_done(stmt);
    }
    sqlite3_finalize(stmt);
  }

  for (i = 0; err == MG_OK && i < n_keywords; ++i) {
    err = prepare(s->db, "INSERT OR IGNORE INTO node_keywords(node_id,keyword_id) VALUES(?,?);", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_blob(stmt, 1, node->id, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, keyword_ids[i]);
      err = step_done(stmt);
    }
    sqlite3_finalize(stmt);
  }

  for (i = 0; err == MG_OK && i < n_edges; ++i) {
    err = prepare(s->db, "INSERT OR REPLACE INTO edges(src,dst,kind,keyword_id,weight) VALUES(?,?,?,?,?);", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_blob(stmt, 1, edges[i].src, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_blob(stmt, 2, edges[i].dst, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 3, (int)edges[i].kind);
      if (edges[i].keyword_id == 0) {
        sqlite3_bind_null(stmt, 4);
      } else {
        sqlite3_bind_int64(stmt, 4, edges[i].keyword_id);
      }
      sqlite3_bind_double(stmt, 5, (double)edges[i].weight);
      err = step_done(stmt);
    }
    sqlite3_finalize(stmt);
  }

  if (err == MG_OK) {
    err = exec_sql(s->db, "COMMIT;");
  } else {
    (void)exec_sql(s->db, "ROLLBACK;");
  }
  return err;
}

mg_err_t mg_storage_get_node(mg_storage_t *s, const mg_node_id_t id, mg_node_t *out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !id || !out) {
    return MG_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  if (prepare(s->db, "SELECT id,content_hash,summary,detail,created_at,last_access,access_count,state FROM nodes WHERE id=?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    memcpy(out->id, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    memcpy(out->content_hash, sqlite3_column_blob(stmt, 1), MG_HASH_BYTES);
    out->summary = mg_strdup((const char *)sqlite3_column_text(stmt, 2));
    out->detail = mg_strdup((const char *)sqlite3_column_text(stmt, 3));
    out->created_at = sqlite3_column_int64(stmt, 4);
    out->last_access = sqlite3_column_int64(stmt, 5);
    out->access_count = sqlite3_column_int64(stmt, 6);
    out->state = (mg_node_state_t)sqlite3_column_int(stmt, 7);
    sqlite3_finalize(stmt);
    if (!out->summary || !out->detail) {
      mg_node_free(out);
      return MG_ERR_OOM;
    }
    return MG_OK;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
}

mg_err_t mg_storage_node_id_by_hash(mg_storage_t *s, const mg_hash_t h, mg_node_id_t out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !h || !out) {
    return MG_ERR_INVALID_ARG;
  }
  if (prepare(s->db, "SELECT id FROM nodes WHERE content_hash=?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_blob(stmt, 1, h, MG_HASH_BYTES, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    memcpy(out, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    sqlite3_finalize(stmt);
    return MG_OK;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
}

mg_err_t mg_storage_touch_access(mg_storage_t *s, const mg_node_id_t id) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  if (!s || !id) {
    return MG_ERR_INVALID_ARG;
  }
  err = prepare(s->db, "UPDATE nodes SET last_access=?, access_count=access_count+1 WHERE id=?;", &stmt);
  if (err != MG_OK) {
    return err;
  }
  sqlite3_bind_int64(stmt, 1, now_ms());
  sqlite3_bind_blob(stmt, 2, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  err = step_done(stmt);
  sqlite3_finalize(stmt);
  return err == MG_OK && sqlite3_changes(s->db) == 0 ? MG_ERR_NOT_FOUND : err;
}

mg_err_t mg_storage_upsert_keyword(mg_storage_t *s, const char *text, const float *opt_embedding, mg_keyword_id_t *out_id) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  if (!s || !text || !out_id) {
    return MG_ERR_INVALID_ARG;
  }
  err = prepare(s->db, "INSERT OR IGNORE INTO keywords(text,embedding) VALUES(?,?);", &stmt);
  if (err == MG_OK) {
    sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC);
    if (opt_embedding) {
      sqlite3_bind_blob(stmt, 2, opt_embedding, (int)sizeof(mg_embedding_t), SQLITE_STATIC);
    } else {
      sqlite3_bind_null(stmt, 2);
    }
    err = step_done(stmt);
  }
  sqlite3_finalize(stmt);
  if (err != MG_OK) {
    return err;
  }
  err = prepare(s->db, "SELECT id FROM keywords WHERE text=? COLLATE NOCASE;", &stmt);
  if (err != MG_OK) {
    return err;
  }
  sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return MG_ERR_STORAGE;
  }
  *out_id = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return MG_OK;
}

mg_err_t mg_storage_get_keyword_text(mg_storage_t *s, mg_keyword_id_t id, char **out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !out || id <= 0) {
    return MG_ERR_INVALID_ARG;
  }
  *out = NULL;
  if (prepare(s->db, "SELECT text FROM keywords WHERE id=?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, id);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out = mg_strdup((const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return *out ? MG_OK : MG_ERR_OOM;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
}

static mg_err_t topk_scan(mg_storage_t *s, const mg_embedding_t query, int k, mg_keyword_id_t kw_id, int use_kw, mg_node_score_t *out, int *out_count) {
  sqlite3_stmt *stmt = NULL;
  const char *sql_all = "SELECT id,embedding FROM node_vec;";
  const char *sql_kw = "SELECT v.id,v.embedding FROM node_vec v JOIN node_keywords nk ON nk.node_id=v.id WHERE nk.keyword_id=?;";
  int rc;
  if (!s || !query || k < 0 || !out || !out_count) {
    return MG_ERR_INVALID_ARG;
  }
  *out_count = 0;
  if (k == 0) {
    return MG_OK;
  }
  if (prepare(s->db, use_kw ? sql_kw : sql_all, &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  if (use_kw) {
    sqlite3_bind_int64(stmt, 1, kw_id);
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const void *id = sqlite3_column_blob(stmt, 0);
    const void *blob = sqlite3_column_blob(stmt, 1);
    int bytes = sqlite3_column_bytes(stmt, 1);
    if (id && blob && bytes == (int)sizeof(mg_embedding_t)) {
      push_topk(out, out_count, k, id, cosine_score(query, (const float *)blob));
    }
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_OK : MG_ERR_STORAGE;
}

mg_err_t mg_storage_vector_topk(mg_storage_t *s, const mg_embedding_t query, int k, mg_node_score_t *out, int *out_count) {
  return topk_scan(s, query, k, 0, 0, out, out_count);
}

mg_err_t mg_storage_vector_topk_by_keyword(mg_storage_t *s, const mg_embedding_t query, mg_keyword_id_t kw_id, int k, mg_node_score_t *out, int *out_count) {
  if (kw_id <= 0) {
    return MG_ERR_INVALID_ARG;
  }
  return topk_scan(s, query, k, kw_id, 1, out, out_count);
}

mg_err_t mg_storage_fts_search(mg_storage_t *s, const char *query_text, int k, bool match_summary, bool match_detail, mg_node_score_t *out, int *out_count) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !query_text || k < 0 || !out || !out_count || (!match_summary && !match_detail)) {
    return MG_ERR_INVALID_ARG;
  }
  *out_count = 0;
  if (k == 0) {
    return MG_OK;
  }
  (void)match_summary;
  (void)match_detail;
  if (prepare(s->db, "SELECT nodes.id, -bm25(node_fts) FROM node_fts JOIN nodes ON nodes.rowid=node_fts.rowid WHERE node_fts MATCH ? ORDER BY bm25(node_fts) LIMIT ?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_text(stmt, 1, query_text, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, k);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    memcpy(out[*out_count].id, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    out[*out_count].score = (float)sqlite3_column_double(stmt, 1);
    ++(*out_count);
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_OK : MG_ERR_STORAGE;
}

mg_err_t mg_storage_neighbors(mg_storage_t *s, const mg_node_id_t src, int kind_filter, const mg_keyword_id_t *kw_filter, size_t n_kw, mg_edge_t *out, int max_out, int *out_count) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !src || !out || !out_count || max_out < 0 || (!kw_filter && n_kw > 0)) {
    return MG_ERR_INVALID_ARG;
  }
  *out_count = 0;
  if (prepare(s->db, "SELECT src,dst,kind,COALESCE(keyword_id,0),weight FROM edges WHERE src=? AND (?=-1 OR kind=?) ORDER BY weight DESC;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_blob(stmt, 1, src, MG_NODE_ID_BYTES, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, kind_filter);
  sqlite3_bind_int(stmt, 3, kind_filter);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_count < max_out) {
    mg_keyword_id_t kw = sqlite3_column_int64(stmt, 3);
    int keep = n_kw == 0;
    size_t i;
    for (i = 0; i < n_kw; ++i) {
      if (kw_filter[i] == kw) {
        keep = 1;
      }
    }
    if (keep) {
      memcpy(out[*out_count].src, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
      memcpy(out[*out_count].dst, sqlite3_column_blob(stmt, 1), MG_NODE_ID_BYTES);
      out[*out_count].kind = (mg_edge_kind_t)sqlite3_column_int(stmt, 2);
      out[*out_count].keyword_id = kw;
      out[*out_count].weight = (float)sqlite3_column_double(stmt, 4);
      ++(*out_count);
    }
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || *out_count == max_out ? MG_OK : MG_ERR_STORAGE;
}

mg_err_t mg_storage_record_sample(mg_storage_t *s, int kind, float cosine) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  if (!s) {
    return MG_ERR_INVALID_ARG;
  }
  err = prepare(s->db, "INSERT INTO similarity_samples(ts,kind,cosine) VALUES(?,?,?);", &stmt);
  if (err != MG_OK) {
    return err;
  }
  sqlite3_bind_int64(stmt, 1, now_ms());
  sqlite3_bind_int(stmt, 2, kind);
  sqlite3_bind_double(stmt, 3, (double)cosine);
  err = step_done(stmt);
  sqlite3_finalize(stmt);
  return err;
}

mg_err_t mg_storage_distribution_percentiles(mg_storage_t *s, int kind, float out[6]) {
  static const double pct[6] = {0.25, 0.50, 0.75, 0.90, 0.95, 0.99};
  sqlite3_stmt *stmt = NULL;
  sqlite3_int64 count;
  int i;
  if (!s || !out) {
    return MG_ERR_INVALID_ARG;
  }
  if (prepare(s->db, "SELECT COUNT(*) FROM similarity_samples WHERE kind=?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int(stmt, 1, kind);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return MG_ERR_STORAGE;
  }
  count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  if (count <= 0) {
    memset(out, 0, 6 * sizeof(float));
    return MG_OK;
  }
  for (i = 0; i < 6; ++i) {
    sqlite3_int64 off = (sqlite3_int64)floor((double)(count - 1) * pct[i]);
    if (prepare(s->db, "SELECT cosine FROM similarity_samples WHERE kind=? ORDER BY cosine LIMIT 1 OFFSET ?;", &stmt) != MG_OK) {
      return MG_ERR_STORAGE;
    }
    sqlite3_bind_int(stmt, 1, kind);
    sqlite3_bind_int64(stmt, 2, off);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      return MG_ERR_STORAGE;
    }
    out[i] = (float)sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return MG_OK;
}

mg_err_t mg_storage_get_embedding(mg_storage_t *s, const mg_node_id_t id, mg_embedding_t out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  const void *blob;
  if (!s || !id || !out) {
    return MG_ERR_INVALID_ARG;
  }
  if (prepare(s->db, "SELECT embedding FROM node_vec WHERE id=?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    blob = sqlite3_column_blob(stmt, 0);
    if (!blob || sqlite3_column_bytes(stmt, 0) != (int)sizeof(mg_embedding_t)) {
      sqlite3_finalize(stmt);
      return MG_ERR_STORAGE;
    }
    memcpy(out, blob, sizeof(mg_embedding_t));
    sqlite3_finalize(stmt);
    return MG_OK;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
}
