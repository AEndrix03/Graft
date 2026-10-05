#include "graft/storage.h"
#include "internal.h"

#include <sqlite3.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
#include <windows.h>
#else
#include <pthread.h>
#endif

extern int sqlite3_vec_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);
extern const char *mg_storage_schema_sql(void);
extern const char *mg_storage_migration_v2_sql(void);
extern const char *mg_storage_migration_v3_sql(void);
extern const char *mg_storage_schema_v4_sql(void);
extern const char *mg_storage_schema_v5_sql(void);

/* The daemon runs one thread per connection and every worker shares this
 * handle. SQLITE_OPEN_FULLMUTEX only serializes single API calls: a
 * BEGIN IMMEDIATE ... COMMIT sequence from one thread could otherwise
 * interleave with another thread's statements on the same connection. Every
 * public function holds `lock` for its whole duration. The mutex is recursive
 * because public functions call each other (insert -> prune_expired,
 * consolidate -> count). */
#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
typedef CRITICAL_SECTION mg_storage_mutex_t;
static int storage_mutex_init(mg_storage_mutex_t *m) {
  InitializeCriticalSection(m);
  return 0;
}
static void storage_mutex_destroy(mg_storage_mutex_t *m) {
  DeleteCriticalSection(m);
}
static void storage_mutex_lock(mg_storage_mutex_t *m) {
  EnterCriticalSection(m);
}
static void storage_mutex_unlock(mg_storage_mutex_t *m) {
  LeaveCriticalSection(m);
}
#else
typedef pthread_mutex_t mg_storage_mutex_t;
static int storage_mutex_init(mg_storage_mutex_t *m) {
  pthread_mutexattr_t attr;
  int rc;
  if (pthread_mutexattr_init(&attr) != 0) {
    return -1;
  }
  rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  if (rc == 0) {
    rc = pthread_mutex_init(m, &attr);
  }
  pthread_mutexattr_destroy(&attr);
  return rc;
}
static void storage_mutex_destroy(mg_storage_mutex_t *m) {
  pthread_mutex_destroy(m);
}
static void storage_mutex_lock(mg_storage_mutex_t *m) {
  pthread_mutex_lock(m);
}
static void storage_mutex_unlock(mg_storage_mutex_t *m) {
  pthread_mutex_unlock(m);
}
#endif

/* `db` must stay the first member: src/retrieve/view.c redeclares the struct
 * with only that field to reach the raw handle. */
struct mg_storage {
  sqlite3 *db;
  mg_storage_mutex_t lock;
  int lock_init;
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
  if (storage_mutex_init(&s->lock) != 0) {
    free(s);
    return MG_ERR_OOM;
  }
  s->lock_init = 1;
  if (sqlite3_open_v2(db_path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
    mg_storage_close(s);
    return MG_ERR_STORAGE;
  }
#ifndef _WIN32
  /* Restrict the DB file to the owner. The file holds embeddings, FTS data,
   * and usage history — treat it as secret. SQLite may have created it with
   * the default umask (often world-readable on POSIX); force 0600 after open
   * regardless of umask. Errors are non-fatal: the daemon can still operate,
   * but log so operators notice if the FS rejects the chmod (rare). */
  if (chmod(db_path, S_IRUSR | S_IWUSR) != 0) {
    fprintf(stderr,
            "graft: warning: chmod(0600) on db file failed (%s)\n",
            db_path);
  }
#endif
  /* Optional encryption-at-rest via SQLCipher. When GRAFT_DB_KEY is set,
   * apply PRAGMA key as the first statement on the connection (SQLCipher
   * requires this BEFORE any other access). Then probe PRAGMA cipher_version
   * to confirm the linked sqlite is actually SQLCipher — vanilla sqlite
   * silently ignores PRAGMA key, leaving the DB unencrypted. We log the
   * mismatch loudly so operators don't get a false sense of security.
   * Leaving GRAFT_DB_KEY unset preserves the legacy unencrypted behaviour. */
  {
    const char *key = getenv("GRAFT_DB_KEY");
    if (key && *key) {
      sqlite3_stmt *probe = NULL;
      /* %Q quotes the key and doubles any embedded single quote, sizing the
       * buffer itself: a hand-sized buffer overflowed on keys full of '. */
      char *pragma = sqlite3_mprintf("PRAGMA key=%Q;", key);
      if (pragma) {
        if (sqlite3_exec(s->db, pragma, NULL, NULL, NULL) != SQLITE_OK) {
          fprintf(stderr, "graft: warning: PRAGMA key failed — DB is NOT encrypted\n");
        }
        sqlite3_free(pragma);
      }
      if (sqlite3_prepare_v2(s->db, "PRAGMA cipher_version;", -1, &probe, NULL) == SQLITE_OK) {
        int has_cipher = (sqlite3_step(probe) == SQLITE_ROW &&
                          sqlite3_column_type(probe, 0) != SQLITE_NULL);
        sqlite3_finalize(probe);
        if (!has_cipher) {
          fprintf(stderr,
                  "graft: WARNING: GRAFT_DB_KEY is set but SQLCipher is not "
                  "linked — the database is stored UNENCRYPTED. Rebuild with "
                  "SQLCipher to enable encryption-at-rest, or unset the env "
                  "var to silence this warning.\n");
        }
      }
    }
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
  if (s->lock_init) {
    storage_mutex_destroy(&s->lock);
  }
  free(s);
}

sqlite3 *mg_storage_sqlite(mg_storage_t *s) {
  return s ? s->db : NULL;
}

void mg_storage_lock(mg_storage_t *s) {
  if (s) {
    storage_mutex_lock(&s->lock);
  }
}

void mg_storage_unlock(mg_storage_t *s) {
  if (s) {
    storage_mutex_unlock(&s->lock);
  }
}

static void apply_db_key(sqlite3 *db) {
  const char *key = getenv("GRAFT_DB_KEY");
  char *pragma;
  if (!key || !*key) {
    return;
  }
  pragma = sqlite3_mprintf("PRAGMA key=%Q;", key);
  if (pragma) {
    sqlite3_exec(db, pragma, NULL, NULL, NULL);
    sqlite3_free(pragma);
  }
}

/* Copy a database with the SQLite backup API instead of copying its main
 * file: the source is opened like any reader, so transactions committed to
 * its -wal but not yet checkpointed (a daemon that crashed) are included, and
 * the destination is written through a connection, so a stale -wal next to
 * it cannot be replayed over the new content. */
mg_err_t mg_storage_backup_file(const char *src_path, const char *dst_path) {
  sqlite3 *src = NULL;
  sqlite3 *dst = NULL;
  sqlite3_backup *bk;
  mg_err_t err = MG_ERR_STORAGE;

  if (!src_path || !dst_path) {
    return MG_ERR_INVALID_ARG;
  }
  if (sqlite3_open_v2(src_path, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
      sqlite3_open_v2(dst_path, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
    goto done;
  }
  apply_db_key(src);
  apply_db_key(dst);
  bk = sqlite3_backup_init(dst, "main", src, "main");
  if (bk) {
    int rc = sqlite3_backup_step(bk, -1);
    if (sqlite3_backup_finish(bk) == SQLITE_OK && rc == SQLITE_DONE) {
      err = MG_OK;
    }
  }
done:
  sqlite3_close(src);
  sqlite3_close(dst);
#ifndef _WIN32
  if (err == MG_OK) {
    chmod(dst_path, S_IRUSR | S_IWUSR);
  }
#endif
  return err;
}

/* Returns 1 if the legacy `summary` column exists on `nodes` (i.e. pre-rename
 * schema). Returns 0 if the table is absent or already on the new schema.
 * Returns -1 on error. */
static int has_table_column(sqlite3 *db, const char *table, const char *column) {
  sqlite3_stmt *stmt = NULL;
  int rc, found = 0;
  char sql[128];
  int n;
  if (!db || !table || !column) return -1;
  n = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
  if (n <= 0 || (size_t)n >= sizeof(sql)) return -1;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    return -1;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(stmt, 1);
    if (name && strcmp((const char *)name, column) == 0) {
      found = 1;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? found : -1;
}

static int has_legacy_summary_column(sqlite3 *db) {
  return has_table_column(db, "nodes", "summary");
}

static mg_err_t storage_apply_schema_unlocked(mg_storage_t *s) {
  mg_err_t err;
  int legacy;
  int has_origin;
  if (!s || !s->db) {
    return MG_ERR_INVALID_ARG;
  }
  /* One-shot rename migration for DBs created before the title/body
   * standardisation. No-op on fresh DBs. */
  legacy = has_legacy_summary_column(s->db);
  if (legacy < 0) {
    return MG_ERR_STORAGE;
  }
  if (legacy == 1) {
    err = exec_sql(s->db, mg_storage_migration_v2_sql());
    if (err != MG_OK) {
      return err;
    }
  }
  has_origin = has_table_column(s->db, "nodes", "origin");
  if (has_origin < 0) {
    return MG_ERR_STORAGE;
  }
  if (has_origin == 0 && has_table_column(s->db, "nodes", "id") == 1) {
    err = exec_sql(s->db, mg_storage_migration_v3_sql());
    if (err != MG_OK) {
      return err;
    }
  }
  err = exec_sql(s->db, mg_storage_schema_sql());
  if (err != MG_OK) {
    return err;
  }
  err = exec_sql(s->db, mg_storage_schema_v4_sql());
  if (err != MG_OK) {
    return err;
  }
  err = exec_sql(s->db, mg_storage_schema_v5_sql());
  if (err != MG_OK) {
    return err;
  }
  err = create_vec_table(s->db);
  if (err != MG_OK) {
    return err;
  }
  return mg_storage_prune_expired(s, NULL);
}

static mg_err_t storage_prune_expired_unlocked(mg_storage_t *s, int64_t *out_deleted) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  int rc;
  int changes;

  if (!s || !s->db) {
    return MG_ERR_INVALID_ARG;
  }
  if (out_deleted) {
    *out_deleted = 0;
  }

  err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    return err;
  }

  err = prepare(s->db,
    "DELETE FROM node_vec WHERE id IN ("
    "  SELECT id FROM nodes "
    "  WHERE expires_at IS NOT NULL "
    "    AND expires_at != 0 "
    "    AND expires_at <= (CAST(strftime('%s','now') AS INTEGER) * 1000)"
    ");",
    &stmt);
  if (err != MG_OK) goto rollback;
  err = step_done(stmt);
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (err != MG_OK && err != MG_ERR_DUPLICATE) goto rollback;

  err = prepare(s->db,
    "DELETE FROM nodes "
    "WHERE expires_at IS NOT NULL "
    "  AND expires_at != 0 "
    "  AND expires_at <= (CAST(strftime('%s','now') AS INTEGER) * 1000);",
    &stmt);
  if (err != MG_OK) goto rollback;
  rc = sqlite3_step(stmt);
  changes = sqlite3_changes(s->db);
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (rc != SQLITE_DONE) {
    err = MG_ERR_STORAGE;
    goto rollback;
  }

  err = exec_sql(s->db, "COMMIT;");
  if (err == MG_OK && out_deleted) {
    *out_deleted = changes;
  }
  return err;

rollback:
  if (stmt) sqlite3_finalize(stmt);
  (void)exec_sql(s->db, "ROLLBACK;");
  return err;
}

/* Upserts the source rows and links them to node_id. Runs inside the
 * caller's transaction. */
static mg_err_t link_sources(mg_storage_t *s, const mg_node_id_t node_id,
                             const mg_source_t *sources, size_t n_sources) {
  sqlite3_stmt *stmt = NULL;
  size_t i;
  mg_err_t err = MG_OK;
  for (i = 0; err == MG_OK && i < n_sources; ++i) {
    const mg_source_t *src = &sources[i];
    const char *project = src->project ? src->project : "";
    sqlite3_int64 source_id = 0;
    if (!src->kind || !src->kind[0] || !src->locator) {
      return MG_ERR_INVALID_ARG;
    }
    /* the source row keeps the newest observation of any node */
    err = prepare(s->db,
      "INSERT INTO sources AS t(project, kind, locator, fingerprint, observed_at) "
      "VALUES(?,?,?,?,?) "
      "ON CONFLICT(project, kind, locator) DO UPDATE SET "
      "  fingerprint = excluded.fingerprint, observed_at = excluded.observed_at "
      "WHERE excluded.observed_at >= t.observed_at;", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, src->kind, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 3, src->locator, -1, SQLITE_STATIC);
      if (src->fingerprint) {
        sqlite3_bind_text(stmt, 4, src->fingerprint, -1, SQLITE_STATIC);
      } else {
        sqlite3_bind_null(stmt, 4);
      }
      sqlite3_bind_int64(stmt, 5, src->observed_at);
      err = step_done(stmt);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (err != MG_OK) break;

    err = prepare(s->db, "SELECT id FROM sources WHERE project=? AND kind=? AND locator=?;", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, src->kind, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 3, src->locator, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        source_id = sqlite3_column_int64(stmt, 0);
      } else {
        err = MG_ERR_STORAGE;
      }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (err != MG_OK) break;

    /* re-attaching records the version the node now derives from */
    err = prepare(s->db,
      "INSERT INTO node_sources(node_id, source_id, role, fingerprint, observed_at) "
      "VALUES(?,?,?,?,?) "
      "ON CONFLICT(node_id, source_id) DO UPDATE SET "
      "  role = excluded.role, fingerprint = excluded.fingerprint, "
      "  observed_at = excluded.observed_at;", &stmt);
    if (err == MG_OK) {
      sqlite3_bind_blob(stmt, 1, node_id, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, source_id);
      sqlite3_bind_text(stmt, 3, src->role ? src->role : "primary", -1, SQLITE_STATIC);
      if (src->fingerprint) {
        sqlite3_bind_text(stmt, 4, src->fingerprint, -1, SQLITE_STATIC);
      } else {
        sqlite3_bind_null(stmt, 4);
      }
      sqlite3_bind_int64(stmt, 5, src->observed_at);
      err = step_done(stmt);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
  }
  return err;
}

static mg_err_t storage_attach_sources_unlocked(mg_storage_t *s, const mg_node_id_t node_id,
                                                const mg_source_t *sources, size_t n_sources) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  int rc;
  if (!s || !node_id || (!sources && n_sources > 0)) {
    return MG_ERR_INVALID_ARG;
  }
  err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    return err;
  }
  err = prepare(s->db, "SELECT 1 FROM nodes WHERE id=?;", &stmt);
  if (err == MG_OK) {
    sqlite3_bind_blob(stmt, 1, node_id, MG_NODE_ID_BYTES, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    err = rc == SQLITE_ROW ? MG_OK : (rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE);
  }
  sqlite3_finalize(stmt);
  if (err == MG_OK) {
    err = link_sources(s, node_id, sources, n_sources);
  }
  if (err == MG_OK) {
    err = exec_sql(s->db, "COMMIT;");
  } else {
    (void)exec_sql(s->db, "ROLLBACK;");
  }
  return err;
}

static char *column_dup(sqlite3_stmt *stmt, int col) {
  const unsigned char *t = sqlite3_column_text(stmt, col);
  return t ? mg_strdup((const char *)t) : NULL;
}

void mg_source_links_free(mg_source_link_t *links, size_t count) {
  size_t i;
  if (!links) {
    return;
  }
  for (i = 0; i < count; ++i) {
    free(links[i].source.project);
    free(links[i].source.kind);
    free(links[i].source.locator);
    free(links[i].source.fingerprint);
    free(links[i].source.role);
    free(links[i].title);
  }
  free(links);
}

static mg_err_t storage_source_links_unlocked(mg_storage_t *s, const char *project,
                                              const char *kind, const mg_node_id_t *node_id,
                                              mg_source_link_t **out, size_t *out_count) {
  sqlite3_stmt *stmt = NULL;
  mg_source_link_t *links = NULL;
  size_t n = 0, cap = 0;
  int rc;
  if (!s || !out || !out_count) {
    return MG_ERR_INVALID_ARG;
  }
  *out = NULL;
  *out_count = 0;
  if (prepare(s->db,
      "SELECT s.project, s.kind, s.locator, ns.fingerprint, ns.role, ns.observed_at, "
      "       n.id, n.title "
      "FROM node_sources AS ns "
      "JOIN sources AS s ON s.id = ns.source_id "
      "JOIN nodes AS n ON n.id = ns.node_id "
      "WHERE (?1 IS NULL OR s.project = ?1) "
      "  AND (?2 IS NULL OR s.kind = ?2) "
      "  AND (?3 IS NULL OR ns.node_id = ?3) "
      "  AND (?3 IS NOT NULL OR n.state IN (0,1)) "
      "ORDER BY s.project, s.kind, s.locator, n.id;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  if (project) sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
  if (kind) sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
  if (node_id) sqlite3_bind_blob(stmt, 3, *node_id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    mg_source_link_t *l;
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 16;
      mg_source_link_t *nb = (mg_source_link_t *)realloc(links, nc * sizeof(*links));
      if (!nb) {
        rc = SQLITE_NOMEM;
        break;
      }
      links = nb;
      cap = nc;
    }
    l = &links[n++];
    memset(l, 0, sizeof(*l));
    l->source.project = column_dup(stmt, 0);
    l->source.kind = column_dup(stmt, 1);
    l->source.locator = column_dup(stmt, 2);
    l->source.fingerprint = column_dup(stmt, 3);
    l->source.role = column_dup(stmt, 4);
    l->source.observed_at = sqlite3_column_int64(stmt, 5);
    if (sqlite3_column_bytes(stmt, 6) == MG_NODE_ID_BYTES) {
      memcpy(l->node_id, sqlite3_column_blob(stmt, 6), MG_NODE_ID_BYTES);
    }
    l->title = column_dup(stmt, 7);
    if (!l->source.project || !l->source.kind || !l->source.locator ||
        !l->source.role || !l->title) {
      rc = SQLITE_NOMEM;
      break;
    }
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    mg_source_links_free(links, n);
    return rc == SQLITE_NOMEM ? MG_ERR_OOM : MG_ERR_STORAGE;
  }
  *out = links;
  *out_count = n;
  return MG_OK;
}

static mg_err_t storage_refresh_source_unlocked(mg_storage_t *s, const mg_node_id_t node_id,
                                                const char *project, const char *kind,
                                                const char *locator, const char *fingerprint,
                                                int64_t observed_at, int64_t *updated) {
  sqlite3_stmt *stmt = NULL;
  mg_err_t err;
  int64_t changed = 0;
  if (!s || !node_id || !kind || !locator) {
    return MG_ERR_INVALID_ARG;
  }
  if (!project) project = "";
  if (updated) *updated = 0;
  err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    return err;
  }
  err = prepare(s->db,
    "UPDATE node_sources SET fingerprint = ?1, observed_at = ?2 "
    "WHERE node_id = ?3 AND source_id = ("
    "  SELECT id FROM sources WHERE project = ?4 AND kind = ?5 AND locator = ?6);", &stmt);
  if (err == MG_OK) {
    if (fingerprint) sqlite3_bind_text(stmt, 1, fingerprint, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, observed_at);
    sqlite3_bind_blob(stmt, 3, node_id, MG_NODE_ID_BYTES, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, project, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, locator, -1, SQLITE_STATIC);
    err = step_done(stmt);
    changed = (int64_t)sqlite3_changes(s->db);
  }
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (err == MG_OK && changed > 0) {
    err = prepare(s->db,
      "UPDATE sources SET fingerprint = ?1, observed_at = ?2 "
      "WHERE project = ?3 AND kind = ?4 AND locator = ?5 AND observed_at <= ?2;", &stmt);
    if (err == MG_OK) {
      if (fingerprint) sqlite3_bind_text(stmt, 1, fingerprint, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, observed_at);
      sqlite3_bind_text(stmt, 3, project, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 4, kind, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 5, locator, -1, SQLITE_STATIC);
      err = step_done(stmt);
    }
    sqlite3_finalize(stmt);
  }
  if (err == MG_OK) {
    err = exec_sql(s->db, "COMMIT;");
  } else {
    (void)exec_sql(s->db, "ROLLBACK;");
  }
  if (err == MG_OK && updated) *updated = changed;
  return err;
}

static mg_err_t storage_insert_node_with_edges_unlocked(
  mg_storage_t *s,
  const mg_node_t *node,
  const mg_embedding_t embedding,
  const mg_keyword_id_t *keyword_ids, size_t n_keywords,
  const mg_edge_t *edges, size_t n_edges,
  const mg_node_id_t *supersedes_id,
  const mg_source_t *sources, size_t n_sources
) {
  sqlite3_stmt *stmt = NULL;
  size_t i;
  mg_err_t err;
  if (!s || !node || !embedding || (!keyword_ids && n_keywords > 0) || (!edges && n_edges > 0) ||
      (!sources && n_sources > 0) || !node->title || !node->body) {
    return MG_ERR_INVALID_ARG;
  }

  (void)mg_storage_prune_expired(s, NULL);

  err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    return err;
  }

  err = prepare(s->db, "INSERT INTO nodes(id,content_hash,title,body,author,created_at,expires_at,last_access,access_count,state) VALUES(?,?,?,?,?,?,?,?,?,?);", &stmt);
  if (err == MG_OK) {
    sqlite3_bind_blob(stmt, 1, node->id, MG_NODE_ID_BYTES, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, node->content_hash, MG_HASH_BYTES, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, node->title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, node->body, -1, SQLITE_STATIC);
    if (node->author) {
      sqlite3_bind_text(stmt, 5, node->author, -1, SQLITE_STATIC);
    } else {
      sqlite3_bind_null(stmt, 5);
    }
    sqlite3_bind_int64(stmt, 6, node->created_at);
    sqlite3_bind_int64(stmt, 7, node->expires_at);
    sqlite3_bind_int64(stmt, 8, node->last_access);
    sqlite3_bind_int64(stmt, 9, node->access_count);
    sqlite3_bind_int(stmt, 10, (int)node->state);
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

  if (err == MG_OK && n_sources > 0) {
    err = link_sources(s, node->id, sources, n_sources);
  }

  /* Supersession (atomic with the insert): mark the old node SUPERSEDED and
   * add a SUPERSEDES edge new -> old. Both happen inside the same tx so an
   * outside reader never sees an "insert without supersede" intermediate. */
  if (err == MG_OK && supersedes_id) {
    err = prepare(s->db,
      "INSERT OR REPLACE INTO edges(src,dst,kind,keyword_id,weight) VALUES(?,?,?,NULL,1.0);",
      &stmt);
    if (err == MG_OK) {
      sqlite3_bind_blob(stmt, 1, node->id, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_blob(stmt, 2, *supersedes_id, MG_NODE_ID_BYTES, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 3, (int)MG_EDGE_SUPERSEDES);
      err = step_done(stmt);
    }
    sqlite3_finalize(stmt);

    if (err == MG_OK) {
      err = prepare(s->db, "UPDATE nodes SET state = ? WHERE id = ?;", &stmt);
      if (err == MG_OK) {
        sqlite3_bind_int(stmt, 1, (int)MG_NODE_SUPERSEDED);
        sqlite3_bind_blob(stmt, 2, *supersedes_id, MG_NODE_ID_BYTES, SQLITE_STATIC);
        err = step_done(stmt);
      }
      sqlite3_finalize(stmt);
    }
  }

  if (err == MG_OK) {
    err = exec_sql(s->db, "COMMIT;");
  } else {
    (void)exec_sql(s->db, "ROLLBACK;");
  }
  return err;
}

static mg_err_t storage_get_node_unlocked(mg_storage_t *s, const mg_node_id_t id, mg_node_t *out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !id || !out) {
    return MG_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  if (prepare(s->db, "SELECT id,content_hash,title,body,author,created_at,expires_at,last_access,access_count,state FROM nodes WHERE id=?;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char *author_text;
    memcpy(out->id, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    memcpy(out->content_hash, sqlite3_column_blob(stmt, 1), MG_HASH_BYTES);
    out->title = mg_strdup((const char *)sqlite3_column_text(stmt, 2));
    out->body  = mg_strdup((const char *)sqlite3_column_text(stmt, 3));
    author_text = sqlite3_column_text(stmt, 4);
    out->author = author_text ? mg_strdup((const char *)author_text) : NULL;
    out->created_at  = sqlite3_column_int64(stmt, 5);
    out->expires_at  = sqlite3_column_int64(stmt, 6);
    out->last_access = sqlite3_column_int64(stmt, 7);
    out->access_count = sqlite3_column_int64(stmt, 8);
    out->state = (mg_node_state_t)sqlite3_column_int(stmt, 9);
    sqlite3_finalize(stmt);
    if (!out->title || !out->body) {
      mg_node_free(out);
      return MG_ERR_OOM;
    }
    return MG_OK;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
}

static mg_err_t storage_node_id_by_hash_unlocked(mg_storage_t *s, const mg_hash_t h, mg_node_id_t out) {
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

/* The node whose SUPERSEDES edge points at `id`. Ids are UUIDv7, so the
 * highest src is the newest successor when several replaced the node. */
static mg_err_t storage_superseded_by_unlocked(mg_storage_t *s, const mg_node_id_t id, mg_node_id_t out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !id || !out) {
    return MG_ERR_INVALID_ARG;
  }
  if (prepare(s->db, "SELECT src FROM edges WHERE dst=? AND kind=? ORDER BY src DESC LIMIT 1;", &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, (int)MG_EDGE_SUPERSEDES);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    memcpy(out, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    sqlite3_finalize(stmt);
    return MG_OK;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_ERR_NOT_FOUND : MG_ERR_STORAGE;
}

static mg_err_t storage_touch_access_unlocked(mg_storage_t *s, const mg_node_id_t id) {
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

static mg_err_t storage_upsert_keyword_unlocked(mg_storage_t *s, const char *text, const float *opt_embedding, mg_keyword_id_t *out_id) {
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

static mg_err_t storage_get_keyword_text_unlocked(mg_storage_t *s, mg_keyword_id_t id, char **out) {
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
  /* Only ACTIVE / STALE nodes are searchable: superseded and retired ones
   * (states 2, 3) and expired ones are excluded. Expiration is
   * stored as Unix milliseconds; strftime('%s') is seconds, so scale it.
   * Read paths only filter expired rows: deleting them is consolidate's job,
   * so a query never needs a write transaction. */
  const char *sql_all = "SELECT v.id,v.embedding FROM node_vec v "
                        "JOIN nodes n ON n.id=v.id "
                        "WHERE n.state IN (0,1) "
                        "AND (n.expires_at IS NULL OR n.expires_at = 0 "
                        "OR n.expires_at > (CAST(strftime('%s','now') AS INTEGER) * 1000));";
  const char *sql_kw  = "SELECT v.id,v.embedding FROM node_vec v "
                        "JOIN node_keywords nk ON nk.node_id=v.id "
                        "JOIN nodes n ON n.id=v.id "
                        "WHERE nk.keyword_id=? AND n.state IN (0,1) "
                        "AND (n.expires_at IS NULL OR n.expires_at = 0 "
                        "OR n.expires_at > (CAST(strftime('%s','now') AS INTEGER) * 1000));";
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

static mg_err_t storage_vector_topk_unlocked(mg_storage_t *s, const mg_embedding_t query, int k, mg_node_score_t *out, int *out_count) {
  return topk_scan(s, query, k, 0, 0, out, out_count);
}

static mg_err_t storage_vector_topk_by_keyword_unlocked(mg_storage_t *s, const mg_embedding_t query, mg_keyword_id_t kw_id, int k, mg_node_score_t *out, int *out_count) {
  if (kw_id <= 0) {
    return MG_ERR_INVALID_ARG;
  }
  return topk_scan(s, query, k, kw_id, 1, out, out_count);
}

/* Build an FTS5 MATCH expression that constrains every token in `query_text`
 * to a single column (e.g. "title" or "body"). Returns a malloc'd string,
 * or NULL on OOM or if the input contains no usable tokens.
 *
 * Without this scoping, passing user input straight into the MATCH expression
 * lets a query like `a) OR body:secret OR title:(b` break out of a `title:(…)`
 * scope and search columns the caller did not authorize. We tokenize on
 * ASCII whitespace and wrap each token as `<col>:"<token>"`, doubling any
 * internal `"` per FTS5 quoting rules. Quoted phrases lose all special
 * meaning to FTS5, so `(`, `)`, `:`, and column-name keywords pass through
 * harmlessly as literal text to match. */
static char *build_scoped_fts_query(const char *col, const char *query_text) {
  size_t in_len = strlen(query_text);
  size_t col_len = strlen(col);
  size_t n_tok = 0;
  size_t i = 0;
  while (i < in_len) {
    while (i < in_len && isspace((unsigned char)query_text[i])) i++;
    if (i >= in_len) break;
    n_tok++;
    while (i < in_len && !isspace((unsigned char)query_text[i])) i++;
  }
  if (n_tok == 0) {
    return NULL;
  }
  /* Exact bound: every byte may double (a quote), plus per token the
   * `<col>:"` prefix, the closing `"` and a separating space. A looser
   * bound used to drop the tail of a query made of many short tokens. */
  size_t cap = 2 * in_len + n_tok * (col_len + 4) + 1;
  char *out = (char *)malloc(cap);
  if (!out) {
    return NULL;
  }
  size_t op = 0;
  i = 0;
  while (i < in_len) {
    while (i < in_len && isspace((unsigned char)query_text[i])) i++;
    if (i >= in_len) break;
    size_t tok_start = i;
    while (i < in_len && !isspace((unsigned char)query_text[i])) i++;
    if (op > 0) out[op++] = ' ';
    /* <col>:"<token with " doubled>" */
    memcpy(out + op, col, col_len); op += col_len;
    out[op++] = ':';
    out[op++] = '"';
    for (size_t j = tok_start; j < i; j++) {
      if (query_text[j] == '"') out[op++] = '"';
      out[op++] = query_text[j];
    }
    out[op++] = '"';
  }
  out[op] = '\0';
  return out;
}

static mg_err_t storage_fts_search_unlocked(mg_storage_t *s, const char *query_text, int k, bool match_title, bool match_body, mg_node_score_t *out, int *out_count) {
  sqlite3_stmt *stmt = NULL;
  char *match_query = NULL;
  const char *match_expr = query_text;
  const char *rank_expr = "bm25(node_fts)";
  int rc;
  if (!s || !query_text || k < 0 || !out || !out_count || (!match_title && !match_body)) {
    return MG_ERR_INVALID_ARG;
  }
  *out_count = 0;
  if (k == 0) {
    return MG_OK;
  }

  if (match_title && !match_body) {
    match_query = build_scoped_fts_query("title", query_text);
    if (!match_query) {
      /* No usable tokens (whitespace-only input) -> no results, not an error. */
      return MG_OK;
    }
    match_expr = match_query;
    rank_expr = "bm25(node_fts, 1.0, 0.0)";
  } else if (!match_title && match_body) {
    match_query = build_scoped_fts_query("body", query_text);
    if (!match_query) {
      return MG_OK;
    }
    match_expr = match_query;
    rank_expr = "bm25(node_fts, 0.0, 1.0)";
  }

  char sql[384];
  snprintf(sql, sizeof(sql),
           "SELECT nodes.id, -%s FROM node_fts "
           "JOIN nodes ON nodes.rowid=node_fts.rowid "
           "WHERE node_fts MATCH ? AND nodes.state IN (0,1) "
           "AND (nodes.expires_at IS NULL OR nodes.expires_at = 0 "
           "OR nodes.expires_at > (CAST(strftime('%%s','now') AS INTEGER) * 1000)) "
           "ORDER BY %s LIMIT ?;",
           rank_expr, rank_expr);
  if (prepare(s->db, sql, &stmt) != MG_OK) {
    free(match_query);
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_text(stmt, 1, match_expr, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, k);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    memcpy(out[*out_count].id, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
    out[*out_count].score = (float)sqlite3_column_double(stmt, 1);
    ++(*out_count);
  }
  sqlite3_finalize(stmt);
  free(match_query);
  return rc == SQLITE_DONE ? MG_OK : MG_ERR_STORAGE;
}

static mg_err_t storage_neighbors_unlocked(mg_storage_t *s, const mg_node_id_t src, int kind_filter, const mg_keyword_id_t *kw_filter, size_t n_kw, mg_edge_t *out, int max_out, int *out_count) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !src || !out || !out_count || max_out < 0 || (!kw_filter && n_kw > 0)) {
    return MG_ERR_INVALID_ARG;
  }
  *out_count = 0;
  if (prepare(s->db,
      "SELECT src,dst,kind,COALESCE(keyword_id,0),weight FROM ("
      "  SELECT e.src,e.dst,e.kind,e.keyword_id,e.weight FROM edges e "
      "  JOIN nodes dst ON dst.id=e.dst "
      "  WHERE e.src=? AND (?=-1 OR e.kind=?) "
      "    AND dst.state IN (0,1) "
      "    AND (dst.expires_at IS NULL OR dst.expires_at = 0 "
      "         OR dst.expires_at > (CAST(strftime('%s','now') AS INTEGER) * 1000)) "
      "  UNION ALL "
      "  SELECT e.dst AS src,e.src AS dst,e.kind,e.keyword_id,e.weight FROM edges e "
      "  JOIN nodes dst ON dst.id=e.src "
      "  WHERE e.dst=? AND e.kind IN (0,1) AND (?=-1 OR e.kind=?)"
      "    AND dst.state IN (0,1) "
      "    AND (dst.expires_at IS NULL OR dst.expires_at = 0 "
      "         OR dst.expires_at > (CAST(strftime('%s','now') AS INTEGER) * 1000)) "
      ") ORDER BY weight DESC;",
      &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_blob(stmt, 1, src, MG_NODE_ID_BYTES, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, kind_filter);
  sqlite3_bind_int(stmt, 3, kind_filter);
  sqlite3_bind_blob(stmt, 4, src, MG_NODE_ID_BYTES, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 5, kind_filter);
  sqlite3_bind_int(stmt, 6, kind_filter);
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
      const void *dst = sqlite3_column_blob(stmt, 1);
      mg_edge_kind_t kind = (mg_edge_kind_t)sqlite3_column_int(stmt, 2);
      int seen = 0;
      for (int j = 0; j < *out_count; ++j) {
        if (out[j].kind == kind &&
            out[j].keyword_id == kw &&
            memcmp(out[j].dst, dst, MG_NODE_ID_BYTES) == 0) {
          seen = 1;
          break;
        }
      }
      if (seen) {
        continue;
      }
      memcpy(out[*out_count].src, sqlite3_column_blob(stmt, 0), MG_NODE_ID_BYTES);
      memcpy(out[*out_count].dst, dst, MG_NODE_ID_BYTES);
      out[*out_count].kind = kind;
      out[*out_count].keyword_id = kw;
      out[*out_count].weight = (float)sqlite3_column_double(stmt, 4);
      ++(*out_count);
    }
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || *out_count == max_out ? MG_OK : MG_ERR_STORAGE;
}

static mg_err_t storage_record_sample_unlocked(mg_storage_t *s, int kind, float cosine) {
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

static mg_err_t storage_distribution_percentiles_unlocked(mg_storage_t *s, int kind, float out[6]) {
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

static mg_err_t storage_get_embedding_unlocked(mg_storage_t *s, const mg_node_id_t id, mg_embedding_t out) {
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

static mg_err_t storage_count_unlocked(mg_storage_t *s, int kind, int64_t *out) {
  static const char *const sqls[] = {
    "SELECT COUNT(*) FROM nodes;",
    "SELECT COUNT(*) FROM edges;",
    "SELECT COUNT(*) FROM keywords;",
  };
  if (!s || !out) return MG_ERR_INVALID_ARG;
  if (kind < 0 || kind >= (int)(sizeof(sqls) / sizeof(*sqls)))
    return MG_ERR_INVALID_ARG;

  sqlite3_stmt *stmt = NULL;
  mg_err_t err = prepare(s->db, sqls[kind], &stmt);
  if (err != MG_OK) return err;

  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out = (int64_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return MG_OK;
  }
  sqlite3_finalize(stmt);
  return MG_ERR_STORAGE;
}

static mg_err_t scalar_i64(mg_storage_t *s, const char *sql, int64_t *out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  if (!s || !sql || !out) {
    return MG_ERR_INVALID_ARG;
  }
  *out = 0;
  if (prepare(s->db, sql, &stmt) != MG_OK) {
    return MG_ERR_STORAGE;
  }
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out = (int64_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return MG_OK;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MG_OK : MG_ERR_STORAGE;
}

static mg_err_t exec_count_changes(mg_storage_t *s, const char *sql, int64_t *out_changes) {
  mg_err_t err;
  if (!s || !sql || !out_changes) {
    return MG_ERR_INVALID_ARG;
  }
  *out_changes = 0;
  err = exec_sql(s->db, sql);
  if (err != MG_OK) {
    return err;
  }
  *out_changes = (int64_t)sqlite3_changes(s->db);
  return MG_OK;
}

static mg_err_t storage_consolidate_unlocked(mg_storage_t *s, mg_storage_consolidate_report_t *out) {
  mg_err_t err;

  if (!s || !out) {
    return MG_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));

  err = mg_storage_prune_expired(s, &out->expired_deleted);
  if (err != MG_OK) {
    return err;
  }

  err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    return err;
  }

  err = exec_count_changes(s,
    "DELETE FROM node_keywords "
    "WHERE NOT EXISTS (SELECT 1 FROM nodes WHERE id = node_keywords.node_id) "
    "   OR NOT EXISTS (SELECT 1 FROM keywords WHERE id = node_keywords.keyword_id);",
    &out->orphan_node_keywords_deleted);
  if (err != MG_OK) goto rollback;

  err = exec_count_changes(s,
    "DELETE FROM edges "
    "WHERE NOT EXISTS (SELECT 1 FROM nodes WHERE id = edges.src) "
    "   OR NOT EXISTS (SELECT 1 FROM nodes WHERE id = edges.dst) "
    "   OR (keyword_id IS NOT NULL AND NOT EXISTS "
    "       (SELECT 1 FROM keywords WHERE id = edges.keyword_id));",
    &out->orphan_edges_deleted);
  if (err != MG_OK) goto rollback;

  err = exec_count_changes(s,
    "DELETE FROM edges "
    "WHERE rowid NOT IN ("
    "  SELECT MIN(rowid) FROM edges "
    "  GROUP BY src, dst, kind, COALESCE(keyword_id, -1)"
    ");",
    &out->duplicate_edges_deleted);
  if (err != MG_OK) goto rollback;

  err = exec_count_changes(s,
    "DELETE FROM edges WHERE weight IS NULL OR weight <= 0.0;",
    &out->invalid_edges_deleted);
  if (err != MG_OK) goto rollback;

  /* Provenance rows no node links to any more (their nodes were deleted). */
  err = exec_sql(s->db,
    "DELETE FROM node_sources "
    "WHERE NOT EXISTS (SELECT 1 FROM nodes WHERE id = node_sources.node_id);");
  if (err != MG_OK) goto rollback;
  err = exec_count_changes(s,
    "DELETE FROM sources "
    "WHERE NOT EXISTS (SELECT 1 FROM node_sources WHERE source_id = sources.id);",
    &out->orphan_sources_deleted);
  if (err != MG_OK) goto rollback;

  err = exec_sql(s->db, "COMMIT;");
  if (err != MG_OK) {
    return err;
  }

  (void)exec_sql(s->db, "ANALYZE;");
  (void)exec_sql(s->db, "PRAGMA optimize;");

  (void)mg_storage_count(s, MG_STORAGE_COUNT_NODES, &out->n_nodes);
  (void)mg_storage_count(s, MG_STORAGE_COUNT_EDGES, &out->n_edges);
  (void)mg_storage_count(s, MG_STORAGE_COUNT_KEYWORDS, &out->n_keywords);

  (void)scalar_i64(s,
    "SELECT COUNT(*) FROM nodes n "
    "WHERE n.state IN (0,1) "
    "  AND NOT EXISTS (SELECT 1 FROM edges e WHERE e.src = n.id OR e.dst = n.id) "
    "  AND NOT EXISTS (SELECT 1 FROM node_keywords nk WHERE nk.node_id = n.id);",
    &out->isolated_nodes);

  (void)scalar_i64(s,
    "SELECT COUNT(*) FROM edges e "
    "WHERE e.kind IN (0,1) "
    "  AND e.src < e.dst "
    "  AND EXISTS ("
    "    SELECT 1 FROM edges r "
    "    WHERE r.src = e.dst AND r.dst = e.src "
    "      AND r.kind = e.kind "
    "      AND COALESCE(r.keyword_id, -1) = COALESCE(e.keyword_id, -1)"
    "  );",
    &out->physical_bidirectional_pairs);

  (void)scalar_i64(s,
    "SELECT COUNT(*) FROM edges WHERE kind = 2;",
    &out->contradictions_found);

  return MG_OK;

rollback:
  (void)exec_sql(s->db, "ROLLBACK;");
  return err;
}

static mg_err_t storage_delete_node_unlocked(mg_storage_t *s, const mg_node_id_t id) {
  if (!s || !id) return MG_ERR_INVALID_ARG;

  mg_err_t err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) return err;

  /* node_vec uses sqlite-vec's vec0 virtual table, which does not honor
   * SQL foreign keys — clean it up explicitly first. */
  sqlite3_stmt *stmt = NULL;
  err = prepare(s->db, "DELETE FROM node_vec WHERE id = ?;", &stmt);
  if (err != MG_OK) goto rollback;
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  err = step_done(stmt);
  sqlite3_finalize(stmt);
  if (err != MG_OK && err != MG_ERR_DUPLICATE) goto rollback;

  /* nodes: deleting cascades node_keywords + edges; the nodes_ad trigger
   * removes the FTS row. */
  err = prepare(s->db, "DELETE FROM nodes WHERE id = ?;", &stmt);
  if (err != MG_OK) goto rollback;
  sqlite3_bind_blob(stmt, 1, id, MG_NODE_ID_BYTES, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(s->db);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    err = MG_ERR_STORAGE;
    goto rollback;
  }
  if (changes == 0) {
    /* nothing to delete — surface NOT_FOUND but still commit (node_vec
     * cleanup above is harmless on a non-existent id). */
    (void)exec_sql(s->db, "COMMIT;");
    return MG_ERR_NOT_FOUND;
  }
  return exec_sql(s->db, "COMMIT;");

rollback:
  (void)exec_sql(s->db, "ROLLBACK;");
  return err;
}

/* 1 when the attached database `schema` has `table`, 0 when not, -1 on error.
 * Profile files written before the provenance schema have no sources
 * tables; syncing with one simply carries no provenance. */
static int attached_has_table(sqlite3 *db, const char *schema, const char *table) {
  sqlite3_stmt *stmt = NULL;
  char sql[160];
  int rc;
  snprintf(sql, sizeof(sql),
           "SELECT 1 FROM %s.sqlite_master WHERE type='table' AND name=?;", schema);
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 1 : (rc == SQLITE_DONE ? 0 : -1);
}

/* Copies provenance from schema `from` into `to` for the links selected by
 * `link_join` (a JOIN/WHERE fragment over `ns`, the from-side node_sources,
 * that yields `node_id_expr` as the to-side node id). Source rows are
 * matched by (project, kind, locator), never by id, so the auto-increment
 * ids of one file never leak into the other. On a conflict the newer
 * observation wins, for the source row and for each link: a node that was
 * revalidated on one side keeps its refreshed fingerprint after any sync. */
static mg_err_t sync_provenance(sqlite3 *db, const char *from, const char *to,
                                const char *node_id_expr, const char *link_join) {
  char sql[2048];
  mg_err_t err;
  int has;

  has = attached_has_table(db, from, "node_sources");
  if (has == 0) return MG_OK;
  if (has < 0) return MG_ERR_STORAGE;
  has = attached_has_table(db, to, "node_sources");
  if (has == 0) return MG_OK;
  if (has < 0) return MG_ERR_STORAGE;

  if (snprintf(sql, sizeof(sql),
      "INSERT INTO %s.sources AS t(project, kind, locator, fingerprint, observed_at) "
      "SELECT ss.project, ss.kind, ss.locator, ss.fingerprint, ss.observed_at "
      "FROM %s.sources AS ss "
      "WHERE EXISTS (SELECT 1 FROM %s.node_sources AS ns %s AND ns.source_id = ss.id) "
      "ON CONFLICT(project, kind, locator) DO UPDATE SET "
      "  fingerprint = excluded.fingerprint, observed_at = excluded.observed_at "
      "WHERE excluded.observed_at > t.observed_at;",
      to, from, from, link_join) >= (int)sizeof(sql)) {
    return MG_ERR_INTERNAL;
  }
  err = exec_sql(db, sql);
  if (err != MG_OK) return err;

  if (snprintf(sql, sizeof(sql),
      "INSERT INTO %s.node_sources AS t(node_id, source_id, role, fingerprint, observed_at) "
      "SELECT %s, ts.id, ns.role, ns.fingerprint, ns.observed_at "
      "FROM %s.node_sources AS ns "
      "JOIN %s.sources AS ss ON ss.id = ns.source_id "
      "JOIN %s.sources AS ts ON ts.project = ss.project AND ts.kind = ss.kind "
      "                     AND ts.locator = ss.locator "
      "%s "
      "ON CONFLICT(node_id, source_id) DO UPDATE SET "
      "  role = excluded.role, fingerprint = excluded.fingerprint, "
      "  observed_at = excluded.observed_at "
      "WHERE excluded.observed_at > t.observed_at;",
      to, node_id_expr, from, from, to, link_join) >= (int)sizeof(sql)) {
    return MG_ERR_INTERNAL;
  }
  return exec_sql(db, sql);
}

static mg_err_t storage_merge_from_unlocked(mg_storage_t *s, const char *source_path,
                                           int overwrite) {
  /* Strategy: ATTACH the source DB read-only to our existing connection (so
   * sqlite-vec is already loaded), copy in dependency order with SQL set-
   * operations, then DETACH. The whole thing runs in one transaction so a
   * failure leaves the target untouched.
   *
   * Node identity: a target row is never deleted or re-keyed. Each source
   * node is first resolved to a target id in temp.merge_map:
   *   kind 1  same content_hash in the target -> that target id
   *   kind 2  no hash match, same id in the target -> that id
   *   kind 0  neither -> inserted under the source id
   * node_vec, node_keywords and edges are then imported through the map, so
   * a source edge to a node the target already holds lands on the target id.
   * INSERT OR REPLACE on nodes would delete the conflicting target row and
   * cascade away its edges and keyword links (issue #11).
   *
   * Overwrite semantics: content_hash covers title+body+keywords, so for a
   * kind 1 match "overwrite" only matters for the metadata (author,
   * created_at, expires_at, last_access, access_count, state). overwrite=1
   * adopts the source's values, overwrite=0 keeps the target's. A kind 2
   * match is the same node with diverged content: overwrite=1 adopts the
   * source content, keyword links and embedding too. The target's id and
   * origin are always kept. Keyword ids are remapped by text since
   * keywords.text is UNIQUE COLLATE NOCASE. */
  if (!s || !source_path) return MG_ERR_INVALID_ARG;

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(s->db, "ATTACH DATABASE ? AS src", -1, &stmt, NULL)
      != SQLITE_OK) return MG_ERR_STORAGE;
  sqlite3_bind_text(stmt, 1, source_path, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return MG_ERR_STORAGE;

  mg_err_t err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    (void)exec_sql(s->db, "DETACH DATABASE src;");
    return err;
  }

  /* 1. resolve every source node to the target id it maps onto. dst_id is
   * UNIQUE: a target row claimed by a hash match cannot also be claimed by
   * an id match. A source row whose id collides with an already-claimed
   * target row is left out (the INSERT OR IGNORE of the last step). */
  err = exec_sql(s->db,
    "DROP TABLE IF EXISTS temp.merge_map;"
    "CREATE TEMP TABLE merge_map("
    "  src_id BLOB PRIMARY KEY,"
    "  dst_id BLOB NOT NULL UNIQUE,"
    "  kind INTEGER NOT NULL);"
    "INSERT INTO temp.merge_map(src_id, dst_id, kind) "
    "SELECT sn.id, mn.id, 1 FROM src.nodes AS sn "
    "JOIN main.nodes AS mn ON mn.content_hash = sn.content_hash;"
    "INSERT OR IGNORE INTO temp.merge_map(src_id, dst_id, kind) "
    "SELECT sn.id, mn.id, 2 FROM src.nodes AS sn "
    "JOIN main.nodes AS mn ON mn.id = sn.id;"
    "INSERT OR IGNORE INTO temp.merge_map(src_id, dst_id, kind) "
    "SELECT sn.id, sn.id, 0 FROM src.nodes AS sn "
    "WHERE NOT EXISTS (SELECT 1 FROM main.nodes WHERE id = sn.id);");
  if (err != MG_OK) goto rollback;

  /* 2. nodes: insert the unmatched ones; never touch a target row's id */
  err = exec_sql(s->db,
    "INSERT INTO main.nodes(id, content_hash, title, body, author, "
    "                       created_at, expires_at, last_access, access_count, state, origin) "
    "SELECT sn.id, sn.content_hash, sn.title, sn.body, sn.author, "
    "       sn.created_at, sn.expires_at, sn.last_access, sn.access_count, sn.state, sn.origin "
    "FROM src.nodes AS sn "
    "JOIN temp.merge_map AS mm ON mm.src_id = sn.id AND mm.kind = 0;");
  if (err != MG_OK) goto rollback;

  if (overwrite) {
    /* adopt source metadata on every matched row */
    err = exec_sql(s->db,
      "UPDATE main.nodes SET "
      "  (author, created_at, expires_at, last_access, access_count, state) = ("
      "    SELECT sn.author, sn.created_at, sn.expires_at, sn.last_access, "
      "           sn.access_count, sn.state "
      "    FROM temp.merge_map AS mm JOIN src.nodes AS sn ON sn.id = mm.src_id "
      "    WHERE mm.dst_id = main.nodes.id) "
      "WHERE id IN (SELECT dst_id FROM temp.merge_map WHERE kind != 0);");
    if (err != MG_OK) goto rollback;

    /* same id, diverged content: adopt the source content. The new hash is
     * absent from the target, otherwise the row would be a kind 1 match.
     * Stale keyword links and embedding go so the source's replace them. */
    err = exec_sql(s->db,
      "UPDATE main.nodes SET (content_hash, title, body) = ("
      "    SELECT sn.content_hash, sn.title, sn.body FROM src.nodes AS sn "
      "    WHERE sn.id = main.nodes.id) "
      "WHERE id IN (SELECT dst_id FROM temp.merge_map WHERE kind = 2);"
      "DELETE FROM main.node_keywords "
      "WHERE node_id IN (SELECT dst_id FROM temp.merge_map WHERE kind = 2);");
    if (err != MG_OK) goto rollback;

    /* node_vec may be a vec0 virtual table with no foreign keys: delete
     * explicitly, and only where the source has a replacement. */
    err = exec_sql(s->db,
      "DELETE FROM main.node_vec WHERE id IN ("
      "  SELECT mm.dst_id FROM temp.merge_map AS mm "
      "  JOIN src.node_vec AS sv ON sv.id = mm.src_id "
      "  WHERE mm.kind = 2);");
    if (err != MG_OK) goto rollback;
  }

  /* 3. node_vec, keyed by the retained target id */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.node_vec(id, embedding) "
    "SELECT mm.dst_id, sv.embedding FROM src.node_vec AS sv "
    "JOIN temp.merge_map AS mm ON mm.src_id = sv.id "
    "WHERE NOT EXISTS (SELECT 1 FROM main.node_vec WHERE id = mm.dst_id);");
  if (err != MG_OK) goto rollback;

  /* 4. keywords (UNIQUE on text COLLATE NOCASE -> idempotent) */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.keywords(text, embedding) "
    "SELECT text, embedding FROM src.keywords;");
  if (err != MG_OK) goto rollback;

  /* 5. node_keywords with node id remap via merge_map, keyword id via text */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.node_keywords(node_id, keyword_id) "
    "SELECT mm.dst_id, m_kw.id "
    "FROM src.node_keywords AS nk "
    "JOIN temp.merge_map AS mm ON mm.src_id = nk.node_id "
    "JOIN src.keywords AS s_kw ON s_kw.id = nk.keyword_id "
    "JOIN main.keywords AS m_kw ON m_kw.text = s_kw.text COLLATE NOCASE;");
  if (err != MG_OK) goto rollback;

  /* 6. edges with both endpoints remapped and optional keyword_id remap.
   * REPLACE here only rewrites the weight of an identical edge: nothing
   * references edges, so it cannot cascade. */
  char sql[1024];
  snprintf(sql, sizeof(sql),
    "%s INTO main.edges(src, dst, kind, keyword_id, weight) "
    "SELECT ms.dst_id, md.dst_id, e.kind, "
    "       CASE WHEN e.keyword_id IS NULL THEN NULL ELSE m_kw.id END, "
    "       e.weight "
    "FROM src.edges AS e "
    "JOIN temp.merge_map AS ms ON ms.src_id = e.src "
    "JOIN temp.merge_map AS md ON md.src_id = e.dst "
    "LEFT JOIN src.keywords AS s_kw ON s_kw.id = e.keyword_id "
    "LEFT JOIN main.keywords AS m_kw ON m_kw.text = s_kw.text COLLATE NOCASE;",
    overwrite ? "INSERT OR REPLACE" : "INSERT OR IGNORE");
  err = exec_sql(s->db, sql);
  if (err != MG_OK) goto rollback;

  /* 7. provenance, node ids remapped like the edges above */
  err = sync_provenance(s->db, "src", "main", "mm.dst_id",
    "JOIN temp.merge_map AS mm ON mm.src_id = ns.node_id WHERE 1");
  if (err != MG_OK) goto rollback;

  err = exec_sql(s->db, "DROP TABLE temp.merge_map;");
  if (err != MG_OK) goto rollback;
  err = exec_sql(s->db, "COMMIT;");
  if (err != MG_OK) goto rollback;
  (void)exec_sql(s->db, "DETACH DATABASE src;");
  return MG_OK;

rollback:
  (void)exec_sql(s->db, "ROLLBACK;");
  (void)exec_sql(s->db, "DROP TABLE IF EXISTS temp.merge_map;");
  (void)exec_sql(s->db, "DETACH DATABASE src;");
  return err;
}

static mg_err_t storage_pull_remote_file_unlocked(mg_storage_t *s, const char *source_path,
                                                 int64_t *inserted, int64_t *deleted) {
  if (!s || !source_path) return MG_ERR_INVALID_ARG;
  if (inserted) *inserted = 0;
  if (deleted) *deleted = 0;

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(s->db, "ATTACH DATABASE ? AS src", -1, &stmt, NULL)
      != SQLITE_OK) return MG_ERR_STORAGE;
  sqlite3_bind_text(stmt, 1, source_path, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return MG_ERR_STORAGE;

  mg_err_t err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) {
    (void)exec_sql(s->db, "DETACH DATABASE src;");
    return err;
  }

  err = exec_sql(s->db,
    "DELETE FROM main.nodes "
    "WHERE origin != 0 "
    "  AND NOT EXISTS (SELECT 1 FROM src.nodes WHERE src.nodes.id = main.nodes.id);");
  if (err != MG_OK) goto rollback;
  if (deleted) *deleted = sqlite3_changes64(s->db);

  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.keywords(text, embedding) "
    "SELECT text, embedding FROM src.keywords;");
  if (err != MG_OK) goto rollback;

  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.nodes(id, content_hash, title, body, author, "
    "  created_at, expires_at, last_access, access_count, state, origin) "
    "SELECT id, content_hash, title, body, author, "
    "  created_at, expires_at, last_access, access_count, state, 1 "
    "FROM src.nodes;");
  if (err != MG_OK) goto rollback;
  if (inserted) *inserted = sqlite3_changes64(s->db);

  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.node_vec(id, embedding) "
    "SELECT id, embedding FROM src.node_vec "
    "WHERE id IN (SELECT id FROM main.nodes);");
  if (err != MG_OK) goto rollback;

  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.node_keywords(node_id, keyword_id) "
    "SELECT nk.node_id, m_kw.id "
    "FROM src.node_keywords AS nk "
    "JOIN src.keywords AS s_kw ON s_kw.id = nk.keyword_id "
    "JOIN main.keywords AS m_kw ON m_kw.text = s_kw.text COLLATE NOCASE "
    "WHERE EXISTS (SELECT 1 FROM main.nodes WHERE id = nk.node_id);");
  if (err != MG_OK) goto rollback;

  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO main.edges(src, dst, kind, keyword_id, weight) "
    "SELECT e.src, e.dst, e.kind, "
    "       CASE WHEN e.keyword_id IS NULL THEN NULL ELSE m_kw.id END, "
    "       e.weight "
    "FROM src.edges AS e "
    "LEFT JOIN src.keywords AS s_kw ON s_kw.id = e.keyword_id "
    "LEFT JOIN main.keywords AS m_kw ON m_kw.text = s_kw.text COLLATE NOCASE "
    "WHERE EXISTS (SELECT 1 FROM main.nodes WHERE id = e.src) "
    "  AND EXISTS (SELECT 1 FROM main.nodes WHERE id = e.dst) "
    "  AND NOT EXISTS (SELECT 1 FROM main.nodes WHERE id = e.src AND origin = 0) "
    "  AND NOT EXISTS (SELECT 1 FROM main.nodes WHERE id = e.dst AND origin = 0);");
  if (err != MG_OK) goto rollback;

  /* Provenance of every node both sides hold (ids are shared); newer
   * observations win, so a refresh done elsewhere comes back. */
  err = sync_provenance(s->db, "src", "main", "ns.node_id",
    "JOIN main.nodes AS ln ON ln.id = ns.node_id WHERE 1");
  if (err != MG_OK) goto rollback;

  err = exec_sql(s->db, "COMMIT;");
  if (err != MG_OK) goto rollback;
  (void)exec_sql(s->db, "DETACH DATABASE src;");
  return MG_OK;

rollback:
  (void)exec_sql(s->db, "ROLLBACK;");
  (void)exec_sql(s->db, "DETACH DATABASE src;");
  return err;
}

static mg_err_t storage_mark_local_pushed_unlocked(mg_storage_t *s, int64_t *updated) {
  if (!s) return MG_ERR_INVALID_ARG;
  if (updated) *updated = 0;
  mg_err_t err = exec_sql(s->db, "UPDATE nodes SET origin = 2 WHERE origin = 0;");
  if (err == MG_OK && updated) *updated = sqlite3_changes64(s->db);
  return err;
}

static mg_err_t storage_push_to_remote_file_unlocked(mg_storage_t *s, const char *dest_path,
                                                    int64_t *pushed) {
  /* Strategy: ATTACH the remote file as "dest" on the daemon's existing
   * connection. This avoids opening a second connection to the local WAL,
   * which was the source of SQLITE_BUSY failures under concurrent inserts.
   *
   * The whole copy + mark runs in one BEGIN IMMEDIATE transaction:
   *   - only origin=0 (LOCAL) nodes are pushed, not already-remote ones
   *   - origin is flipped to 2 (PUSHED) in the same transaction, so there
   *     is no window where a new insert gets marked PUSHED without being sent
   */
  if (!s || !dest_path) return MG_ERR_INVALID_ARG;
  if (pushed) *pushed = 0;

  sqlite3_busy_timeout(s->db, 5000);

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(s->db, "ATTACH DATABASE ? AS dest", -1, &stmt, NULL)
      != SQLITE_OK) {
    sqlite3_busy_timeout(s->db, 0);
    return MG_ERR_STORAGE;
  }
  sqlite3_bind_text(stmt, 1, dest_path, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    sqlite3_busy_timeout(s->db, 0);
    return MG_ERR_STORAGE;
  }

  mg_err_t err = exec_sql(s->db, "BEGIN IMMEDIATE;");
  if (err != MG_OK) goto detach;

  /* 1. keywords referenced by local nodes */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO dest.keywords(text, embedding) "
    "SELECT DISTINCT k.text, k.embedding "
    "FROM main.keywords AS k "
    "JOIN main.node_keywords AS nk ON nk.keyword_id = k.id "
    "JOIN main.nodes AS n ON n.id = nk.node_id "
    "WHERE n.origin = 0;");
  if (err != MG_OK) goto rollback;

  /* 2. nodes (LOCAL only) */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO dest.nodes"
    "(id, content_hash, title, body, author,"
    " created_at, expires_at, last_access, access_count, state, origin) "
    "SELECT id, content_hash, title, body, author,"
    "       created_at, expires_at, last_access, access_count, state, 0 "
    "FROM main.nodes WHERE origin = 0;");
  if (err != MG_OK) goto rollback;
  if (pushed) *pushed = sqlite3_changes64(s->db);

  /* 3. embeddings for pushed nodes */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO dest.node_vec(id, embedding) "
    "SELECT id, embedding FROM main.node_vec "
    "WHERE id IN (SELECT id FROM main.nodes WHERE origin = 0);");
  if (err != MG_OK) goto rollback;

  /* 4. node_keywords with keyword id remap via text */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO dest.node_keywords(node_id, keyword_id) "
    "SELECT nk.node_id, dk.id "
    "FROM main.node_keywords AS nk "
    "JOIN main.keywords AS mk ON mk.id = nk.keyword_id "
    "JOIN dest.keywords  AS dk ON dk.text = mk.text COLLATE NOCASE "
    "WHERE EXISTS (SELECT 1 FROM dest.nodes WHERE id = nk.node_id);");
  if (err != MG_OK) goto rollback;

  /* 5. edges where both endpoints are in dest */
  err = exec_sql(s->db,
    "INSERT OR IGNORE INTO dest.edges(src, dst, kind, keyword_id, weight) "
    "SELECT e.src, e.dst, e.kind, "
    "       CASE WHEN e.keyword_id IS NULL THEN NULL ELSE dk.id END, "
    "       e.weight "
    "FROM main.edges AS e "
    "LEFT JOIN main.keywords AS mk ON mk.id = e.keyword_id "
    "LEFT JOIN dest.keywords  AS dk ON dk.text = mk.text COLLATE NOCASE "
    "WHERE EXISTS (SELECT 1 FROM dest.nodes WHERE id = e.src) "
    "  AND EXISTS (SELECT 1 FROM dest.nodes WHERE id = e.dst);");
  if (err != MG_OK) goto rollback;

  /* 5b. provenance of every node the remote holds, pushed now or earlier:
   * a later local refresh of an already pushed node propagates too. */
  err = sync_provenance(s->db, "main", "dest", "ns.node_id",
    "JOIN dest.nodes AS dn ON dn.id = ns.node_id WHERE 1");
  if (err != MG_OK) goto rollback;

  /* 6. flip origin to PUSHED — atomic with the copy above */
  err = exec_sql(s->db, "UPDATE main.nodes SET origin = 2 WHERE origin = 0;");
  if (err != MG_OK) goto rollback;

  err = exec_sql(s->db, "COMMIT;");
  if (err != MG_OK) goto rollback;
  goto detach;

rollback:
  (void)exec_sql(s->db, "ROLLBACK;");
  if (pushed) *pushed = 0;
detach:
  (void)exec_sql(s->db, "DETACH DATABASE dest;");
  sqlite3_busy_timeout(s->db, 0);
  return err;
}

static mg_err_t storage_node_keywords_unlocked(mg_storage_t *s,
                                               const mg_node_id_t node_id,
                                               mg_keyword_id_t *out_ids,
                                               int max_out, int *out_count) {
  if (!s || !node_id || !out_ids || !out_count || max_out <= 0) return MG_ERR_INVALID_ARG;

  sqlite3_stmt *stmt = NULL;
  mg_err_t err = prepare(s->db,
    "SELECT keyword_id FROM node_keywords WHERE node_id = ? ORDER BY keyword_id;",
    &stmt);
  if (err != MG_OK) return err;

  sqlite3_bind_blob(stmt, 1, node_id, MG_NODE_ID_BYTES, SQLITE_STATIC);

  int n = 0;
  while (n < max_out) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out_ids[n++] = (mg_keyword_id_t)sqlite3_column_int64(stmt, 0);
    } else if (rc == SQLITE_DONE) {
      break;
    } else {
      sqlite3_finalize(stmt);
      return MG_ERR_STORAGE;
    }
  }
  sqlite3_finalize(stmt);
  *out_count = n;
  return MG_OK;
}

/* === Public entry points ===
 * Each one holds the storage mutex for the whole call, so a transaction (and
 * any sqlite3_changes() read after a statement) belongs to a single caller. */

#define STORAGE_LOCKED(s, call) do { \
  mg_err_t locked_err_; \
  if (!(s)) return MG_ERR_INVALID_ARG; \
  storage_mutex_lock(&(s)->lock); \
  locked_err_ = (call); \
  storage_mutex_unlock(&(s)->lock); \
  return locked_err_; \
} while (0)

mg_err_t mg_storage_apply_schema(mg_storage_t *s) {
  STORAGE_LOCKED(s, storage_apply_schema_unlocked(s));
}

mg_err_t mg_storage_prune_expired(mg_storage_t *s, int64_t *out_deleted) {
  STORAGE_LOCKED(s, storage_prune_expired_unlocked(s, out_deleted));
}

mg_err_t mg_storage_insert_node_with_edges(
  mg_storage_t *s,
  const mg_node_t *node,
  const mg_embedding_t embedding,
  const mg_keyword_id_t *keyword_ids, size_t n_keywords,
  const mg_edge_t *edges, size_t n_edges,
  const mg_node_id_t *supersedes_id
) {
  STORAGE_LOCKED(s, storage_insert_node_with_edges_unlocked(
    s, node, embedding, keyword_ids, n_keywords, edges, n_edges, supersedes_id, NULL, 0));
}

mg_err_t mg_storage_insert_node_with_sources(
  mg_storage_t *s,
  const mg_node_t *node,
  const mg_embedding_t embedding,
  const mg_keyword_id_t *keyword_ids, size_t n_keywords,
  const mg_edge_t *edges, size_t n_edges,
  const mg_node_id_t *supersedes_id,
  const mg_source_t *sources, size_t n_sources
) {
  STORAGE_LOCKED(s, storage_insert_node_with_edges_unlocked(
    s, node, embedding, keyword_ids, n_keywords, edges, n_edges, supersedes_id,
    sources, n_sources));
}

mg_err_t mg_storage_attach_sources(mg_storage_t *s, const mg_node_id_t node_id,
                                   const mg_source_t *sources, size_t n_sources) {
  STORAGE_LOCKED(s, storage_attach_sources_unlocked(s, node_id, sources, n_sources));
}

mg_err_t mg_storage_source_links(mg_storage_t *s, const char *project,
                                 const char *kind, const mg_node_id_t *node_id,
                                 mg_source_link_t **out, size_t *out_count) {
  STORAGE_LOCKED(s, storage_source_links_unlocked(s, project, kind, node_id, out, out_count));
}

mg_err_t mg_storage_refresh_source(mg_storage_t *s, const mg_node_id_t node_id,
                                   const char *project, const char *kind,
                                   const char *locator, const char *fingerprint,
                                   int64_t observed_at, int64_t *updated) {
  STORAGE_LOCKED(s, storage_refresh_source_unlocked(s, node_id, project, kind, locator,
                                                    fingerprint, observed_at, updated));
}

mg_err_t mg_storage_get_node(mg_storage_t *s, const mg_node_id_t id, mg_node_t *out) {
  STORAGE_LOCKED(s, storage_get_node_unlocked(s, id, out));
}

mg_err_t mg_storage_node_id_by_hash(mg_storage_t *s, const mg_hash_t h, mg_node_id_t out) {
  STORAGE_LOCKED(s, storage_node_id_by_hash_unlocked(s, h, out));
}

mg_err_t mg_storage_superseded_by(mg_storage_t *s, const mg_node_id_t id, mg_node_id_t out) {
  STORAGE_LOCKED(s, storage_superseded_by_unlocked(s, id, out));
}

mg_err_t mg_storage_touch_access(mg_storage_t *s, const mg_node_id_t id) {
  STORAGE_LOCKED(s, storage_touch_access_unlocked(s, id));
}

mg_err_t mg_storage_upsert_keyword(mg_storage_t *s, const char *text, const float *opt_embedding, mg_keyword_id_t *out_id) {
  STORAGE_LOCKED(s, storage_upsert_keyword_unlocked(s, text, opt_embedding, out_id));
}

mg_err_t mg_storage_get_keyword_text(mg_storage_t *s, mg_keyword_id_t id, char **out) {
  STORAGE_LOCKED(s, storage_get_keyword_text_unlocked(s, id, out));
}

mg_err_t mg_storage_vector_topk(mg_storage_t *s, const mg_embedding_t query, int k, mg_node_score_t *out, int *out_count) {
  STORAGE_LOCKED(s, storage_vector_topk_unlocked(s, query, k, out, out_count));
}

mg_err_t mg_storage_vector_topk_by_keyword(mg_storage_t *s, const mg_embedding_t query, mg_keyword_id_t kw_id, int k, mg_node_score_t *out, int *out_count) {
  STORAGE_LOCKED(s, storage_vector_topk_by_keyword_unlocked(s, query, kw_id, k, out, out_count));
}

mg_err_t mg_storage_fts_search(mg_storage_t *s, const char *query_text, int k, bool match_title, bool match_body, mg_node_score_t *out, int *out_count) {
  STORAGE_LOCKED(s, storage_fts_search_unlocked(s, query_text, k, match_title, match_body, out, out_count));
}

mg_err_t mg_storage_neighbors(mg_storage_t *s, const mg_node_id_t src, int kind_filter, const mg_keyword_id_t *kw_filter, size_t n_kw, mg_edge_t *out, int max_out, int *out_count) {
  STORAGE_LOCKED(s, storage_neighbors_unlocked(s, src, kind_filter, kw_filter, n_kw, out, max_out, out_count));
}

mg_err_t mg_storage_record_sample(mg_storage_t *s, int kind, float cosine) {
  STORAGE_LOCKED(s, storage_record_sample_unlocked(s, kind, cosine));
}

mg_err_t mg_storage_distribution_percentiles(mg_storage_t *s, int kind, float out[6]) {
  STORAGE_LOCKED(s, storage_distribution_percentiles_unlocked(s, kind, out));
}

mg_err_t mg_storage_get_embedding(mg_storage_t *s, const mg_node_id_t id, mg_embedding_t out) {
  STORAGE_LOCKED(s, storage_get_embedding_unlocked(s, id, out));
}

mg_err_t mg_storage_count(mg_storage_t *s, int kind, int64_t *out) {
  STORAGE_LOCKED(s, storage_count_unlocked(s, kind, out));
}

mg_err_t mg_storage_consolidate(mg_storage_t *s, mg_storage_consolidate_report_t *out) {
  STORAGE_LOCKED(s, storage_consolidate_unlocked(s, out));
}

mg_err_t mg_storage_delete_node(mg_storage_t *s, const mg_node_id_t id) {
  STORAGE_LOCKED(s, storage_delete_node_unlocked(s, id));
}

mg_err_t mg_storage_merge_from(mg_storage_t *s, const char *source_path,
                               int overwrite) {
  STORAGE_LOCKED(s, storage_merge_from_unlocked(s, source_path, overwrite));
}

mg_err_t mg_storage_pull_remote_file(mg_storage_t *s, const char *source_path,
                                     int64_t *inserted, int64_t *deleted) {
  STORAGE_LOCKED(s, storage_pull_remote_file_unlocked(s, source_path, inserted, deleted));
}

mg_err_t mg_storage_mark_local_pushed(mg_storage_t *s, int64_t *updated) {
  STORAGE_LOCKED(s, storage_mark_local_pushed_unlocked(s, updated));
}

mg_err_t mg_storage_push_to_remote_file(mg_storage_t *s, const char *dest_path,
                                        int64_t *pushed) {
  STORAGE_LOCKED(s, storage_push_to_remote_file_unlocked(s, dest_path, pushed));
}

mg_err_t mg_storage_node_keywords(mg_storage_t *s,
                                  const mg_node_id_t node_id,
                                  mg_keyword_id_t *out_ids,
                                  int max_out, int *out_count) {
  STORAGE_LOCKED(s, storage_node_keywords_unlocked(s, node_id, out_ids, max_out, out_count));
}
