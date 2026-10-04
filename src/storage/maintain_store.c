/* Maintenance storage (issue #5): candidate cache, dismissals, audit log,
 * reversible state transitions and the apply-safe steps. See
 * include/graft/maintain.h for the contract and src/storage/schema.c
 * (v5) for the tables. Every public function holds the storage mutex for
 * its whole duration; the mutex is recursive, so calling other
 * mg_storage_* functions from inside is fine. */

#include "graft/maintain.h"
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAINT_LOCKED(s, call) do { \
  mg_err_t locked_err_; \
  if (!(s)) return MG_ERR_INVALID_ARG; \
  mg_storage_lock(s); \
  locked_err_ = (call); \
  mg_storage_unlock(s); \
  return locked_err_; \
} while (0)

static const char *const kind_names[MG_MAINT_N_KINDS] = {
  "contradiction",
  "source_removed",
  "source_changed",
  "possible_supersession",
  "near_duplicate",
  "keyword_fragmentation",
  "isolated_low_value",
};

const char *mg_maint_kind_name(mg_maint_kind_t k) {
  return (k >= 0 && k < MG_MAINT_N_KINDS) ? kind_names[k] : "unknown";
}

int mg_maint_kind_from_name(const char *name) {
  int i;
  for (i = 0; name && i < MG_MAINT_N_KINDS; ++i) {
    if (!strcmp(name, kind_names[i])) return i;
  }
  return -1;
}

static char *dup_str(const char *s) {
  size_t n;
  char *out;
  if (!s) return NULL;
  n = strlen(s) + 1;
  out = (char *)malloc(n);
  if (out) memcpy(out, s, n);
  return out;
}

static char *column_dup(sqlite3_stmt *stmt, int col) {
  const unsigned char *t = sqlite3_column_text(stmt, col);
  return t ? dup_str((const char *)t) : NULL;
}

static void hex_id(const mg_node_id_t id, char out[2 * MG_NODE_ID_BYTES + 1]) {
  static const char hx[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < MG_NODE_ID_BYTES; ++i) {
    out[2 * i] = hx[id[i] >> 4];
    out[2 * i + 1] = hx[id[i] & 15];
  }
  out[2 * MG_NODE_ID_BYTES] = '\0';
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int unhex_id(const char *hex, size_t len, mg_node_id_t out) {
  size_t i;
  if (len != 2 * MG_NODE_ID_BYTES) return -1;
  for (i = 0; i < MG_NODE_ID_BYTES; ++i) {
    int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

static int64_t now_ms(void) {
  return (int64_t)time(NULL) * 1000;
}

static mg_err_t exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? MG_OK : MG_ERR_STORAGE;
}

static mg_err_t prep(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
  return sqlite3_prepare_v2(db, sql, -1, stmt, NULL) == SQLITE_OK ? MG_OK : MG_ERR_STORAGE;
}

/* Steps a statement expected to finish without rows, then finalizes it. */
static mg_err_t run(sqlite3_stmt *stmt) {
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_OK : MG_ERR_STORAGE;
}

static void bind_opt_text(sqlite3_stmt *stmt, int idx, const char *v) {
  if (v) sqlite3_bind_text(stmt, idx, v, -1, SQLITE_TRANSIENT);
  else   sqlite3_bind_null(stmt, idx);
}

/* ------------------------------------------------------------------ meta */

static mg_err_t meta_get_unlocked(sqlite3 *db, const char *key, int64_t *out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  *out = 0;
  if (prep(db, "SELECT value FROM maintenance_meta WHERE key=?;", &stmt) != MG_OK) return MG_ERR_STORAGE;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) *out = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? MG_OK : MG_ERR_STORAGE;
}

static mg_err_t meta_set_unlocked(sqlite3 *db, const char *key, int64_t value) {
  sqlite3_stmt *stmt = NULL;
  if (prep(db, "INSERT INTO maintenance_meta(key, value) VALUES(?, ?) "
               "ON CONFLICT(key) DO UPDATE SET value = excluded.value;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, value);
  return run(stmt);
}

mg_err_t mg_storage_maint_meta_get(mg_storage_t *s, const char *key, int64_t *out) {
  if (!key || !out) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, meta_get_unlocked(mg_storage_sqlite(s), key, out));
}

mg_err_t mg_storage_maint_meta_set(mg_storage_t *s, const char *key, int64_t value) {
  if (!key) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, meta_set_unlocked(mg_storage_sqlite(s), key, value));
}

/* ----------------------------------------------------------- scan inputs */

static mg_err_t active_vectors_unlocked(sqlite3 *db, size_t max_nodes,
                                        mg_maint_vec_t **out, size_t *n) {
  sqlite3_stmt *stmt = NULL;
  mg_maint_vec_t *v = NULL;
  size_t count = 0, cap = 0;
  int rc;
  if (prep(db,
      "SELECT n.id, n.created_at, v.embedding FROM nodes n "
      "JOIN node_vec v ON v.id = n.id "
      "WHERE n.state = 0 AND (n.expires_at = 0 OR n.expires_at > ?) "
      "ORDER BY n.created_at DESC, n.id DESC LIMIT ?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, now_ms());
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)max_nodes);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const void *blob = sqlite3_column_blob(stmt, 2);
    double norm = 0.0;
    size_t i;
    if (sqlite3_column_bytes(stmt, 0) != MG_NODE_ID_BYTES || !blob ||
        sqlite3_column_bytes(stmt, 2) != (int)sizeof(mg_embedding_t)) {
      continue;
    }
    if (count == cap) {
      size_t nc = cap ? cap * 2 : 64;
      mg_maint_vec_t *nv = (mg_maint_vec_t *)realloc(v, nc * sizeof(*v));
      if (!nv) {
        rc = SQLITE_NOMEM;
        break;
      }
      v = nv;
      cap = nc;
    }
    memcpy(v[count].id, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    v[count].created_at = sqlite3_column_int64(stmt, 1);
    memcpy(v[count].emb, blob, sizeof(mg_embedding_t));
    for (i = 0; i < MG_EMBEDDING_DIM; ++i) norm += (double)v[count].emb[i] * v[count].emb[i];
    if (norm <= 0.0) continue;
    norm = sqrt(norm);
    for (i = 0; i < MG_EMBEDDING_DIM; ++i) v[count].emb[i] = (float)(v[count].emb[i] / norm);
    count++;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    free(v);
    return rc == SQLITE_NOMEM ? MG_ERR_OOM : MG_ERR_STORAGE;
  }
  *out = v;
  *n = count;
  return MG_OK;
}

mg_err_t mg_storage_maint_active_vectors(mg_storage_t *s, size_t max_nodes,
                                         mg_maint_vec_t **out, size_t *n) {
  if (!out || !n) return MG_ERR_INVALID_ARG;
  *out = NULL;
  *n = 0;
  MAINT_LOCKED(s, active_vectors_unlocked(mg_storage_sqlite(s), max_nodes, out, n));
}

static mg_err_t contradictions_unlocked(sqlite3 *db, size_t limit,
                                        mg_maint_pair_t **out, size_t *n) {
  sqlite3_stmt *stmt = NULL;
  mg_maint_pair_t *p;
  size_t count = 0;
  int rc;
  p = (mg_maint_pair_t *)calloc(limit ? limit : 1, sizeof(*p));
  if (!p) return MG_ERR_OOM;
  if (prep(db,
      "SELECT CASE WHEN e.src < e.dst THEN e.src ELSE e.dst END AS a, "
      "       CASE WHEN e.src < e.dst THEN e.dst ELSE e.src END AS b, "
      "       MAX(e.weight) AS w "
      "FROM edges e "
      "JOIN nodes x ON x.id = e.src JOIN nodes y ON y.id = e.dst "
      "WHERE e.kind = 2 AND e.src != e.dst AND x.state = 0 AND y.state = 0 "
      "GROUP BY a, b ORDER BY w DESC, a, b LIMIT ?;", &stmt) != MG_OK) {
    free(p);
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)limit);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < limit) {
    if (sqlite3_column_bytes(stmt, 0) != MG_NODE_ID_BYTES ||
        sqlite3_column_bytes(stmt, 1) != MG_NODE_ID_BYTES) {
      continue;
    }
    memcpy(p[count].a, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    memcpy(p[count].b, sqlite3_column_blob(stmt, 1), MG_NODE_ID_BYTES);
    p[count].weight = (float)sqlite3_column_double(stmt, 2);
    count++;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    free(p);
    return MG_ERR_STORAGE;
  }
  *out = p;
  *n = count;
  return MG_OK;
}

mg_err_t mg_storage_maint_contradictions(mg_storage_t *s, size_t limit,
                                         mg_maint_pair_t **out, size_t *n) {
  if (!out || !n) return MG_ERR_INVALID_ARG;
  *out = NULL;
  *n = 0;
  MAINT_LOCKED(s, contradictions_unlocked(mg_storage_sqlite(s), limit, out, n));
}

static mg_err_t node_ids_query(sqlite3_stmt *stmt, size_t limit, mg_node_id_t *ids, size_t *n) {
  int rc;
  size_t count = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < limit) {
    if (sqlite3_column_bytes(stmt, 0) == MG_NODE_ID_BYTES) {
      memcpy(ids[count++], sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    }
  }
  sqlite3_finalize(stmt);
  *n = count;
  return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? MG_OK : MG_ERR_STORAGE;
}

static mg_err_t isolated_unlocked(sqlite3 *db, int64_t before, size_t limit,
                                  mg_node_id_t **out, size_t *n) {
  sqlite3_stmt *stmt = NULL;
  mg_node_id_t *ids = (mg_node_id_t *)calloc(limit ? limit : 1, sizeof(*ids));
  mg_err_t err;
  if (!ids) return MG_ERR_OOM;
  if (prep(db,
      "SELECT n.id FROM nodes n "
      "WHERE n.state = 0 AND n.access_count = 0 AND n.created_at < ? "
      "  AND NOT EXISTS (SELECT 1 FROM edges e WHERE e.src = n.id) "
      "  AND NOT EXISTS (SELECT 1 FROM edges e WHERE e.dst = n.id) "
      "ORDER BY n.created_at, n.id LIMIT ?;", &stmt) != MG_OK) {
    free(ids);
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, before);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);
  err = node_ids_query(stmt, limit, ids, n);
  if (err != MG_OK) {
    free(ids);
    return err;
  }
  *out = ids;
  return MG_OK;
}

mg_err_t mg_storage_maint_isolated(mg_storage_t *s, int64_t before, size_t limit,
                                   mg_node_id_t **out, size_t *n) {
  if (!out || !n) return MG_ERR_INVALID_ARG;
  *out = NULL;
  *n = 0;
  MAINT_LOCKED(s, isolated_unlocked(mg_storage_sqlite(s), before, limit, out, n));
}

void mg_maint_keywords_free(mg_maint_keyword_t *kw, size_t n) {
  size_t i;
  if (!kw) return;
  for (i = 0; i < n; ++i) free(kw[i].text);
  free(kw);
}

static mg_err_t keyword_usage_unlocked(sqlite3 *db, mg_maint_keyword_t **out, size_t *n) {
  sqlite3_stmt *stmt = NULL;
  mg_maint_keyword_t *kw = NULL;
  size_t count = 0, cap = 0;
  int rc;
  if (prep(db,
      "SELECT k.id, k.text, COUNT(*) FROM keywords k "
      "JOIN node_keywords nk ON nk.keyword_id = k.id "
      "JOIN nodes n ON n.id = nk.node_id "
      "WHERE n.state = 0 GROUP BY k.id ORDER BY k.id;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (count == cap) {
      size_t nc = cap ? cap * 2 : 64;
      mg_maint_keyword_t *nk = (mg_maint_keyword_t *)realloc(kw, nc * sizeof(*kw));
      if (!nk) {
        rc = SQLITE_NOMEM;
        break;
      }
      kw = nk;
      cap = nc;
    }
    kw[count].id = sqlite3_column_int64(stmt, 0);
    kw[count].text = column_dup(stmt, 1);
    kw[count].uses = sqlite3_column_int64(stmt, 2);
    if (!kw[count].text) {
      rc = SQLITE_NOMEM;
      break;
    }
    count++;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    mg_maint_keywords_free(kw, count);
    return rc == SQLITE_NOMEM ? MG_ERR_OOM : MG_ERR_STORAGE;
  }
  *out = kw;
  *n = count;
  return MG_OK;
}

mg_err_t mg_storage_maint_keyword_usage(mg_storage_t *s, mg_maint_keyword_t **out, size_t *n) {
  if (!out || !n) return MG_ERR_INVALID_ARG;
  *out = NULL;
  *n = 0;
  MAINT_LOCKED(s, keyword_usage_unlocked(mg_storage_sqlite(s), out, n));
}

static mg_err_t keyword_nodes_unlocked(sqlite3 *db, mg_keyword_id_t kw, mg_node_id_t *out,
                                       size_t max, size_t *n) {
  sqlite3_stmt *stmt = NULL;
  if (prep(db,
      "SELECT n.id FROM node_keywords nk JOIN nodes n ON n.id = nk.node_id "
      "WHERE nk.keyword_id = ? AND n.state = 0 ORDER BY n.created_at, n.id LIMIT ?;",
      &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, kw);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)max);
  return node_ids_query(stmt, max, out, n);
}

mg_err_t mg_storage_maint_keyword_nodes(mg_storage_t *s, mg_keyword_id_t kw,
                                        mg_node_id_t *out, size_t max, size_t *n) {
  if (!out || !n) return MG_ERR_INVALID_ARG;
  *n = 0;
  MAINT_LOCKED(s, keyword_nodes_unlocked(mg_storage_sqlite(s), kw, out, max, n));
}

static mg_err_t is_dismissed_unlocked(sqlite3 *db, const char *id, const char *evidence, int *out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  *out = 0;
  if (prep(db, "SELECT 1 FROM maintenance_dismissals WHERE candidate_id = ? AND evidence = ?;",
           &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, evidence, -1, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  *out = rc == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? MG_OK : MG_ERR_STORAGE;
}

mg_err_t mg_storage_maint_is_dismissed(mg_storage_t *s, const char *candidate_id,
                                       const char *evidence, int *out) {
  if (!candidate_id || !evidence || !out) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, is_dismissed_unlocked(mg_storage_sqlite(s), candidate_id, evidence, out));
}

/* ------------------------------------------------------- candidate cache */

static mg_err_t store_candidates_unlocked(sqlite3 *db, const mg_maint_candidate_t *rows,
                                          size_t n, int64_t now) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  size_t i;
  if (exec(db, "BEGIN IMMEDIATE;") != MG_OK) return MG_ERR_STORAGE;
  err = exec(db, "DELETE FROM maintenance_candidates;");
  for (i = 0; err == MG_OK && i < n; ++i) {
    err = prep(db,
      "INSERT OR REPLACE INTO maintenance_candidates"
      "(id, kind, priority, score, nodes, evidence, payload, created_at) "
      "VALUES(?,?,?,?,?,?,?,?);", &stmt);
    if (err != MG_OK) break;
    sqlite3_bind_text(stmt, 1, rows[i].id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, rows[i].kind, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, rows[i].priority);
    sqlite3_bind_double(stmt, 4, rows[i].score);
    sqlite3_bind_text(stmt, 5, rows[i].nodes ? rows[i].nodes : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, rows[i].evidence, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 7, rows[i].payload ? rows[i].payload : "", (int)rows[i].payload_len,
                      SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 8, now);
    err = run(stmt);
  }
  if (err == MG_OK) err = meta_set_unlocked(db, MG_MAINT_META_LAST_SCAN, now);
  if (err == MG_OK) err = exec(db, "COMMIT;");
  if (err != MG_OK) (void)exec(db, "ROLLBACK;");
  return err;
}

mg_err_t mg_storage_maint_store_candidates(mg_storage_t *s, const mg_maint_candidate_t *rows,
                                           size_t n, int64_t now) {
  if (!rows && n > 0) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, store_candidates_unlocked(mg_storage_sqlite(s), rows, n, now));
}

void mg_maint_candidate_clear(mg_maint_candidate_t *c) {
  if (!c) return;
  free(c->nodes);
  free(c->payload);
  memset(c, 0, sizeof(*c));
}

void mg_maint_candidates_free(mg_maint_candidate_t *c, size_t n) {
  size_t i;
  if (!c) return;
  for (i = 0; i < n; ++i) mg_maint_candidate_clear(&c[i]);
  free(c);
}

/* columns: id, kind, priority, score, nodes, evidence, payload */
static int read_candidate(sqlite3_stmt *stmt, mg_maint_candidate_t *c) {
  const unsigned char *t;
  const void *blob;
  int len;
  memset(c, 0, sizeof(*c));
  t = sqlite3_column_text(stmt, 0);
  snprintf(c->id, sizeof(c->id), "%s", t ? (const char *)t : "");
  t = sqlite3_column_text(stmt, 1);
  snprintf(c->kind, sizeof(c->kind), "%s", t ? (const char *)t : "");
  c->priority = sqlite3_column_int(stmt, 2);
  c->score = sqlite3_column_double(stmt, 3);
  c->nodes = column_dup(stmt, 4);
  t = sqlite3_column_text(stmt, 5);
  snprintf(c->evidence, sizeof(c->evidence), "%s", t ? (const char *)t : "");
  blob = sqlite3_column_blob(stmt, 6);
  len = sqlite3_column_bytes(stmt, 6);
  c->payload = malloc(len > 0 ? (size_t)len : 1);
  if (c->payload && len > 0) memcpy(c->payload, blob, (size_t)len);
  c->payload_len = len > 0 ? (size_t)len : 0;
  if (!c->nodes || !c->payload) {
    mg_maint_candidate_clear(c);
    return -1;
  }
  return 0;
}

static mg_err_t list_candidates_unlocked(sqlite3 *db, size_t limit,
                                         mg_maint_candidate_t **out, size_t *n) {
  sqlite3_stmt *stmt = NULL;
  mg_maint_candidate_t *c;
  size_t count = 0;
  int rc;
  c = (mg_maint_candidate_t *)calloc(limit ? limit : 1, sizeof(*c));
  if (!c) return MG_ERR_OOM;
  if (prep(db,
      "SELECT id, kind, priority, score, nodes, evidence, payload "
      "FROM maintenance_candidates ORDER BY priority, score DESC, id LIMIT ?;", &stmt) != MG_OK) {
    free(c);
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)limit);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < limit) {
    if (read_candidate(stmt, &c[count]) != 0) {
      rc = SQLITE_NOMEM;
      break;
    }
    count++;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    mg_maint_candidates_free(c, count);
    return rc == SQLITE_NOMEM ? MG_ERR_OOM : MG_ERR_STORAGE;
  }
  *out = c;
  *n = count;
  return MG_OK;
}

mg_err_t mg_storage_maint_list_candidates(mg_storage_t *s, size_t limit,
                                          mg_maint_candidate_t **out, size_t *n) {
  if (!out || !n) return MG_ERR_INVALID_ARG;
  *out = NULL;
  *n = 0;
  MAINT_LOCKED(s, list_candidates_unlocked(mg_storage_sqlite(s), limit, out, n));
}

static mg_err_t get_candidate_unlocked(sqlite3 *db, const char *id, mg_maint_candidate_t *out) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  int rc;
  if (prep(db,
      "SELECT id, kind, priority, score, nodes, evidence, payload "
      "FROM maintenance_candidates WHERE id = ?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    err = read_candidate(stmt, out) == 0 ? MG_OK : MG_ERR_OOM;
  } else {
    err = rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
  }
  sqlite3_finalize(stmt);
  return err;
}

mg_err_t mg_storage_maint_get_candidate(mg_storage_t *s, const char *id,
                                        mg_maint_candidate_t *out) {
  if (!id || !out) return MG_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  MAINT_LOCKED(s, get_candidate_unlocked(mg_storage_sqlite(s), id, out));
}

static mg_err_t drop_candidates_naming(sqlite3 *db, const mg_node_id_t id) {
  sqlite3_stmt *stmt = NULL;
  char hex[2 * MG_NODE_ID_BYTES + 1];
  hex_id(id, hex);
  if (prep(db, "DELETE FROM maintenance_candidates WHERE instr(nodes, ?) > 0;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_text(stmt, 1, hex, -1, SQLITE_TRANSIENT);
  return run(stmt);
}

/* ------------------------------------------------------------ audit log */

static mg_err_t log_append_unlocked(sqlite3 *db, const mg_maint_log_t *log) {
  sqlite3_stmt *stmt = NULL;
  if (!log->actor || !log->action) return MG_ERR_INVALID_ARG;
  if (prep(db,
      "INSERT INTO maintenance_log(ts, actor, action, candidate_id, kind, nodes, detail, note) "
      "VALUES(?,?,?,?,?,?,?,?);", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, log->ts ? log->ts : now_ms());
  sqlite3_bind_text(stmt, 2, log->actor, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, log->action, -1, SQLITE_TRANSIENT);
  bind_opt_text(stmt, 4, log->candidate_id);
  bind_opt_text(stmt, 5, log->kind);
  sqlite3_bind_text(stmt, 6, log->nodes ? log->nodes : "", -1, SQLITE_TRANSIENT);
  bind_opt_text(stmt, 7, log->detail);
  bind_opt_text(stmt, 8, log->note);
  return run(stmt);
}

mg_err_t mg_storage_maint_log_append(mg_storage_t *s, const mg_maint_log_t *log) {
  if (!log) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, log_append_unlocked(mg_storage_sqlite(s), log));
}

void mg_maint_log_rows_free(mg_maint_log_row_t *rows, size_t n) {
  size_t i;
  if (!rows) return;
  for (i = 0; i < n; ++i) {
    free(rows[i].actor);
    free(rows[i].action);
    free(rows[i].candidate_id);
    free(rows[i].kind);
    free(rows[i].nodes);
    free(rows[i].detail);
    free(rows[i].note);
  }
  free(rows);
}

static mg_err_t log_list_unlocked(sqlite3 *db, size_t limit, const char *node_hex,
                                  mg_maint_log_row_t **out, size_t *n) {
  sqlite3_stmt *stmt = NULL;
  mg_maint_log_row_t *rows;
  size_t count = 0;
  int rc;
  rows = (mg_maint_log_row_t *)calloc(limit ? limit : 1, sizeof(*rows));
  if (!rows) return MG_ERR_OOM;
  if (prep(db,
      "SELECT id, ts, actor, action, candidate_id, kind, nodes, detail, note "
      "FROM maintenance_log WHERE (?1 IS NULL OR instr(nodes, ?1) > 0) "
      "ORDER BY id DESC LIMIT ?2;", &stmt) != MG_OK) {
    free(rows);
    return MG_ERR_STORAGE;
  }
  bind_opt_text(stmt, 1, node_hex);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < limit) {
    mg_maint_log_row_t *r = &rows[count++];
    r->id = sqlite3_column_int64(stmt, 0);
    r->ts = sqlite3_column_int64(stmt, 1);
    r->actor = column_dup(stmt, 2);
    r->action = column_dup(stmt, 3);
    r->candidate_id = column_dup(stmt, 4);
    r->kind = column_dup(stmt, 5);
    r->nodes = column_dup(stmt, 6);
    r->detail = column_dup(stmt, 7);
    r->note = column_dup(stmt, 8);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    mg_maint_log_rows_free(rows, count);
    return MG_ERR_STORAGE;
  }
  *out = rows;
  *n = count;
  return MG_OK;
}

mg_err_t mg_storage_maint_log_list(mg_storage_t *s, size_t limit, const char *node_hex,
                                   mg_maint_log_row_t **out, size_t *n) {
  if (!out || !n) return MG_ERR_INVALID_ARG;
  *out = NULL;
  *n = 0;
  MAINT_LOCKED(s, log_list_unlocked(mg_storage_sqlite(s), limit, node_hex, out, n));
}

/* ----------------------------------------------------------- transitions */

static mg_err_t node_state(sqlite3 *db, const mg_node_id_t id, int *state) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (prep(db, "SELECT state FROM nodes WHERE id = ?;", &stmt) != MG_OK) return MG_ERR_STORAGE;
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) *state = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW) return MG_OK;
  return rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
}

static mg_err_t set_state(sqlite3 *db, const mg_node_id_t id, int state) {
  sqlite3_stmt *stmt = NULL;
  if (prep(db, "UPDATE nodes SET state = ? WHERE id = ?;", &stmt) != MG_OK) return MG_ERR_STORAGE;
  sqlite3_bind_int(stmt, 1, state);
  sqlite3_bind_blob(stmt, 2, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  return run(stmt);
}

static mg_err_t id_stmt(sqlite3 *db, const char *sql, const mg_node_id_t id) {
  sqlite3_stmt *stmt = NULL;
  if (prep(db, sql, &stmt) != MG_OK) return MG_ERR_STORAGE;
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  return run(stmt);
}

/* Runs inside the caller's transaction. */
static mg_err_t transition(sqlite3 *db, const mg_maint_change_t *c, int64_t now) {
  int state = 0, by_state = 0;
  mg_err_t err = node_state(db, c->node, &state);
  if (err != MG_OK) return err;
  switch (c->to) {
    case MG_MAINT_TO_STALE:
      if (state == MG_NODE_STALE) return MG_OK;
      if (state != MG_NODE_ACTIVE) return MG_ERR_INVALID_ARG;
      return set_state(db, c->node, MG_NODE_STALE);

    case MG_MAINT_TO_RETIRED: {
      sqlite3_stmt *stmt = NULL;
      if (state == MG_NODE_RETIRED) return MG_OK;
      if (prep(db, "INSERT OR REPLACE INTO maintenance_retired(node_id, retired_at, prev_state) "
                   "VALUES(?,?,?);", &stmt) != MG_OK) {
        return MG_ERR_STORAGE;
      }
      sqlite3_bind_blob(stmt, 1, c->node, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, now);
      sqlite3_bind_int(stmt, 3, state);
      err = run(stmt);
      return err == MG_OK ? set_state(db, c->node, MG_NODE_RETIRED) : err;
    }

    case MG_MAINT_TO_ACTIVE:
      if (state == MG_NODE_ACTIVE) return MG_OK;
      err = set_state(db, c->node, MG_NODE_ACTIVE);
      if (err == MG_OK) {
        err = id_stmt(db, "DELETE FROM maintenance_retired WHERE node_id = ?;", c->node);
      }
      /* a restored node is nobody's predecessor any more */
      if (err == MG_OK) {
        err = id_stmt(db, "DELETE FROM edges WHERE dst = ? AND kind = 3;", c->node);
      }
      return err;

    case MG_MAINT_TO_SUPERSEDED: {
      sqlite3_stmt *stmt = NULL;
      if (memcmp(c->node, c->by, MG_NODE_ID_BYTES) == 0) return MG_ERR_INVALID_ARG;
      if (state != MG_NODE_ACTIVE && state != MG_NODE_STALE && state != MG_NODE_SUPERSEDED) {
        return MG_ERR_INVALID_ARG;
      }
      err = node_state(db, c->by, &by_state);
      if (err != MG_OK) return err;
      if (by_state != MG_NODE_ACTIVE && by_state != MG_NODE_STALE) return MG_ERR_INVALID_ARG;
      if (prep(db, "INSERT OR REPLACE INTO edges(src, dst, kind, keyword_id, weight) "
                   "VALUES(?, ?, 3, NULL, 1.0);", &stmt) != MG_OK) {
        return MG_ERR_STORAGE;
      }
      sqlite3_bind_blob(stmt, 1, c->by, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_blob(stmt, 2, c->node, MG_NODE_ID_BYTES, SQLITE_STATIC);
      err = run(stmt);
      return err == MG_OK ? set_state(db, c->node, MG_NODE_SUPERSEDED) : err;
    }
  }
  return MG_ERR_INVALID_ARG;
}

static mg_err_t apply_unlocked(sqlite3 *db, const mg_maint_change_t *changes, size_t n_changes,
                               const char *dismiss_id, const char *dismiss_kind,
                               const char *dismiss_evidence, const char *drop_id,
                               const mg_maint_log_t *log) {
  mg_err_t err;
  size_t i;
  int64_t ts = log && log->ts ? log->ts : now_ms();
  if (exec(db, "BEGIN IMMEDIATE;") != MG_OK) return MG_ERR_STORAGE;
  err = MG_OK;
  for (i = 0; err == MG_OK && i < n_changes; ++i) {
    err = transition(db, &changes[i], ts);
    if (err == MG_OK) err = drop_candidates_naming(db, changes[i].node);
  }
  if (err == MG_OK && dismiss_id) {
    sqlite3_stmt *stmt = NULL;
    err = prep(db,
      "INSERT OR REPLACE INTO maintenance_dismissals(candidate_id, kind, evidence, dismissed_at, note) "
      "VALUES(?,?,?,?,?);", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_text(stmt, 1, dismiss_id, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, dismiss_kind ? dismiss_kind : "", -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 3, dismiss_evidence ? dismiss_evidence : "", -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 4, ts);
      bind_opt_text(stmt, 5, log ? log->note : NULL);
      err = run(stmt);
    }
  }
  if (err == MG_OK && drop_id) {
    sqlite3_stmt *stmt = NULL;
    err = prep(db, "DELETE FROM maintenance_candidates WHERE id = ?;", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_text(stmt, 1, drop_id, -1, SQLITE_STATIC);
      err = run(stmt);
    }
  }
  if (err == MG_OK && log) err = log_append_unlocked(db, log);
  if (err == MG_OK) err = exec(db, "COMMIT;");
  if (err != MG_OK) (void)exec(db, "ROLLBACK;");
  return err;
}

mg_err_t mg_storage_maint_apply(mg_storage_t *s,
                                const mg_maint_change_t *changes, size_t n_changes,
                                const char *dismiss_id, const char *dismiss_kind,
                                const char *dismiss_evidence, const char *drop_id,
                                const mg_maint_log_t *log) {
  if (!changes && n_changes > 0) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, apply_unlocked(mg_storage_sqlite(s), changes, n_changes, dismiss_id,
                                 dismiss_kind, dismiss_evidence, drop_id, log));
}

/* ------------------------------------------------------- apply-safe steps */

static mg_err_t purge_retired_unlocked(mg_storage_t *s, int64_t cutoff, int64_t now,
                                       const char *actor, int64_t *purged) {
  sqlite3 *db = mg_storage_sqlite(s);
  sqlite3_stmt *stmt = NULL;
  mg_node_id_t *ids = NULL;
  size_t n = 0, cap = 0, i;
  mg_err_t err = MG_OK;
  int rc;

  *purged = 0;
  /* retired rows that lost their node state (restored by a merge, ...) and
   * retired nodes without a record (pulled from a remote profile) */
  if (exec(db, "DELETE FROM maintenance_retired WHERE node_id NOT IN "
               "(SELECT id FROM nodes WHERE state = 3);") != MG_OK) {
    return MG_ERR_STORAGE;
  }
  if (prep(db, "INSERT OR IGNORE INTO maintenance_retired(node_id, retired_at, prev_state) "
               "SELECT id, ?, 0 FROM nodes WHERE state = 3;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, now);
  if (run(stmt) != MG_OK) return MG_ERR_STORAGE;

  if (prep(db, "SELECT r.node_id FROM maintenance_retired r JOIN nodes n ON n.id = r.node_id "
               "WHERE n.state = 3 AND r.retired_at <= ? ORDER BY r.retired_at;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, cutoff);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (sqlite3_column_bytes(stmt, 0) != MG_NODE_ID_BYTES) continue;
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 16;
      mg_node_id_t *ni = (mg_node_id_t *)realloc(ids, nc * sizeof(*ids));
      if (!ni) {
        rc = SQLITE_NOMEM;
        break;
      }
      ids = ni;
      cap = nc;
    }
    memcpy(ids[n++], sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    free(ids);
    return rc == SQLITE_NOMEM ? MG_ERR_OOM : MG_ERR_STORAGE;
  }

  for (i = 0; err == MG_OK && i < n; ++i) {
    char hex[2 * MG_NODE_ID_BYTES + 1];
    mg_maint_log_t log;
    err = mg_storage_delete_node(s, ids[i]);
    if (err == MG_ERR_NOT_FOUND) {
      err = MG_OK;
      continue;
    }
    if (err != MG_OK) break;
    (*purged)++;
    hex_id(ids[i], hex);
    (void)drop_candidates_naming(db, ids[i]);
    memset(&log, 0, sizeof(log));
    log.ts = now;
    log.actor = actor;
    log.action = "purge";
    log.nodes = hex;
    log.detail = "retention window elapsed";
    err = log_append_unlocked(db, &log);
  }
  free(ids);
  return err;
}

mg_err_t mg_storage_maint_purge_retired(mg_storage_t *s, int64_t cutoff, int64_t now,
                                        const char *actor, int64_t *purged) {
  if (!actor || !purged) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, purge_retired_unlocked(s, cutoff, now, actor, purged));
}

static mg_err_t collapse_exact_unlocked(sqlite3 *db, int64_t now, const char *actor,
                                        int64_t *collapsed) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err = MG_OK;
  char *prev_title = NULL, *prev_body = NULL;
  mg_node_id_t keeper;
  int rc;

  *collapsed = 0;
  if (exec(db, "BEGIN IMMEDIATE;") != MG_OK) return MG_ERR_STORAGE;
  /* active first, then oldest: the keeper is the first row of each group */
  if (prep(db,
      "SELECT id, title, body FROM nodes "
      "WHERE state IN (0,1) AND (title, body) IN ("
      "  SELECT title, body FROM nodes WHERE state IN (0,1) "
      "  GROUP BY title, body HAVING COUNT(*) > 1) "
      "ORDER BY title, body, state, created_at, id;", &stmt) != MG_OK) {
    (void)exec(db, "ROLLBACK;");
    return MG_ERR_STORAGE;
  }
  while (err == MG_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *title = (const char *)sqlite3_column_text(stmt, 1);
    const char *body = (const char *)sqlite3_column_text(stmt, 2);
    mg_node_id_t id;
    if (sqlite3_column_bytes(stmt, 0) != MG_NODE_ID_BYTES || !title || !body) continue;
    memcpy(id, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    if (!prev_title || strcmp(prev_title, title) != 0 || strcmp(prev_body, body) != 0) {
      free(prev_title);
      free(prev_body);
      prev_title = dup_str(title);
      prev_body = dup_str(body);
      if (!prev_title || !prev_body) err = MG_ERR_OOM;
      memcpy(keeper, id, MG_NODE_ID_BYTES);
      continue;
    }
    {
      mg_maint_change_t c;
      mg_maint_log_t log;
      sqlite3_stmt *cp = NULL;
      char nodes[2 * (2 * MG_NODE_ID_BYTES + 1)];
      char detail[64];
      memset(&c, 0, sizeof(c));
      c.to = MG_MAINT_TO_SUPERSEDED;
      memcpy(c.node, id, MG_NODE_ID_BYTES);
      memcpy(c.by, keeper, MG_NODE_ID_BYTES);
      err = transition(db, &c, now);
      if (err != MG_OK) break;
      /* the keeper inherits the duplicate's provenance */
      err = prep(db,
        "INSERT OR IGNORE INTO node_sources(node_id, source_id, role, fingerprint, observed_at) "
        "SELECT ?1, source_id, role, fingerprint, observed_at FROM node_sources WHERE node_id = ?2;",
        &cp);
      if (err != MG_OK) break;
      sqlite3_bind_blob(cp, 1, keeper, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_blob(cp, 2, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
      err = run(cp);
      if (err == MG_OK) err = drop_candidates_naming(db, id);
      if (err != MG_OK) break;
      hex_id(id, nodes);
      nodes[2 * MG_NODE_ID_BYTES] = ',';
      hex_id(keeper, nodes + 2 * MG_NODE_ID_BYTES + 1);
      snprintf(detail, sizeof(detail), "superseded_by=%s", nodes + 2 * MG_NODE_ID_BYTES + 1);
      memset(&log, 0, sizeof(log));
      log.ts = now;
      log.actor = actor;
      log.action = "collapse_duplicate";
      log.nodes = nodes;
      log.detail = detail;
      err = log_append_unlocked(db, &log);
      if (err == MG_OK) (*collapsed)++;
    }
  }
  sqlite3_finalize(stmt);
  free(prev_title);
  free(prev_body);
  if (err == MG_OK) err = exec(db, "COMMIT;");
  if (err != MG_OK) (void)exec(db, "ROLLBACK;");
  return err;
}

mg_err_t mg_storage_maint_collapse_exact(mg_storage_t *s, int64_t now, const char *actor,
                                         int64_t *collapsed) {
  if (!actor || !collapsed) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, collapse_exact_unlocked(mg_storage_sqlite(s), now, actor, collapsed));
}

/* 1 when every comma-separated id in nodes names an ACTIVE node. */
static int all_active(sqlite3 *db, const char *nodes) {
  const char *p = nodes;
  while (p && *p) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    mg_node_id_t id;
    int state = -1;
    if (unhex_id(p, len, id) != 0 || node_state(db, id, &state) != MG_OK ||
        state != MG_NODE_ACTIVE) {
      return 0;
    }
    p = comma ? comma + 1 : NULL;
  }
  return 1;
}

static mg_err_t prune_candidates_unlocked(sqlite3 *db, int64_t *dropped) {
  sqlite3_stmt *stmt = NULL;
  char (*gone)[MG_MAINT_ID_HEX + 1] = NULL;
  size_t n = 0, cap = 0, i;
  mg_err_t err = MG_OK;
  int rc;
  *dropped = 0;
  if (prep(db, "SELECT id, nodes FROM maintenance_candidates;", &stmt) != MG_OK) return MG_ERR_STORAGE;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const char *nodes = (const char *)sqlite3_column_text(stmt, 1);
    if (!id || all_active(db, nodes)) continue;
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 16;
      void *ng = realloc(gone, nc * sizeof(*gone));
      if (!ng) {
        rc = SQLITE_NOMEM;
        break;
      }
      gone = (char (*)[MG_MAINT_ID_HEX + 1])ng;
      cap = nc;
    }
    snprintf(gone[n++], MG_MAINT_ID_HEX + 1, "%s", id);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    free(gone);
    return rc == SQLITE_NOMEM ? MG_ERR_OOM : MG_ERR_STORAGE;
  }
  for (i = 0; err == MG_OK && i < n; ++i) {
    err = prep(db, "DELETE FROM maintenance_candidates WHERE id = ?;", &stmt);
    if (err != MG_OK) break;
    sqlite3_bind_text(stmt, 1, gone[i], -1, SQLITE_STATIC);
    err = run(stmt);
    if (err == MG_OK) (*dropped)++;
  }
  free(gone);
  return err;
}

mg_err_t mg_storage_maint_prune_candidates(mg_storage_t *s, int64_t *dropped) {
  int64_t ignored;
  MAINT_LOCKED(s, prune_candidates_unlocked(mg_storage_sqlite(s), dropped ? dropped : &ignored));
}

/* ---------------------------------------------------------------- status */

static mg_err_t count_since(sqlite3 *db, int64_t since, int64_t *out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  *out = 0;
  if (prep(db, "SELECT COUNT(*) FROM nodes WHERE created_at > ?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, since);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) *out = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? MG_OK : MG_ERR_STORAGE;
}

static mg_err_t status_unlocked(sqlite3 *db, int64_t cutoff, mg_maint_status_t *out) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  int rc;

  memset(out, 0, sizeof(*out));
  err = meta_get_unlocked(db, MG_MAINT_META_LAST_APPLY_SAFE, &out->last_apply_safe_at);
  if (err == MG_OK) err = meta_get_unlocked(db, MG_MAINT_META_LAST_SCAN, &out->last_scan_at);
  if (err == MG_OK) err = count_since(db, out->last_apply_safe_at, &out->inserts_since_apply_safe);
  if (err == MG_OK) err = count_since(db, out->last_scan_at, &out->inserts_since_scan);
  if (err != MG_OK) return err;

  if (prep(db, "SELECT state, COUNT(*) FROM nodes GROUP BY state;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int64_t c = sqlite3_column_int64(stmt, 1);
    switch (sqlite3_column_int(stmt, 0)) {
      case MG_NODE_ACTIVE:     out->n_active = c; break;
      case MG_NODE_STALE:      out->n_stale = c; break;
      case MG_NODE_SUPERSEDED: out->n_superseded = c; break;
      case MG_NODE_RETIRED:    out->n_retired = c; break;
      default: break;
    }
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return MG_ERR_STORAGE;

  if (prep(db, "SELECT COUNT(*) FROM maintenance_retired r JOIN nodes n ON n.id = r.node_id "
               "WHERE n.state = 3 AND r.retired_at <= ?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_int64(stmt, 1, cutoff);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) out->retired_due = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW) return MG_ERR_STORAGE;

  if (prep(db, "SELECT kind, COUNT(*) FROM maintenance_candidates GROUP BY kind;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int k = mg_maint_kind_from_name((const char *)sqlite3_column_text(stmt, 0));
    int64_t c = sqlite3_column_int64(stmt, 1);
    if (k >= 0) out->pending[k] = c;
    out->pending_total += c;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_OK : MG_ERR_STORAGE;
}

mg_err_t mg_storage_maint_status(mg_storage_t *s, int64_t retired_cutoff,
                                 mg_maint_status_t *out) {
  if (!out) return MG_ERR_INVALID_ARG;
  MAINT_LOCKED(s, status_unlocked(mg_storage_sqlite(s), retired_cutoff, out));
}
