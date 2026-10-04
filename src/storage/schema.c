#include <stddef.h>

const char *mg_storage_schema_sql(void) {
  return
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous = NORMAL;"
    "PRAGMA foreign_keys = ON;"
    "PRAGMA mmap_size = 268435456;"
    "CREATE TABLE IF NOT EXISTS nodes ("
    "  id BLOB PRIMARY KEY,"
    "  content_hash BLOB NOT NULL UNIQUE,"
    "  title TEXT NOT NULL,"
    "  body TEXT NOT NULL,"
    "  author TEXT,"
    "  created_at INTEGER NOT NULL,"
    "  expires_at INTEGER NOT NULL DEFAULT 0,"
    "  last_access INTEGER NOT NULL,"
    "  access_count INTEGER NOT NULL DEFAULT 0,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  origin INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_nodes_hash ON nodes(content_hash);"
    "CREATE INDEX IF NOT EXISTS idx_nodes_state ON nodes(state);"
    "CREATE INDEX IF NOT EXISTS idx_nodes_expires_at ON nodes(expires_at);"
    "CREATE INDEX IF NOT EXISTS idx_nodes_origin ON nodes(origin);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS node_fts USING fts5("
    "  title, body,"
    "  content='nodes', content_rowid='rowid',"
    "  tokenize='unicode61 remove_diacritics 2'"
    ");"
    "CREATE TRIGGER IF NOT EXISTS nodes_ai AFTER INSERT ON nodes BEGIN "
    "  INSERT INTO node_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS nodes_au AFTER UPDATE ON nodes BEGIN "
    "  INSERT INTO node_fts(node_fts, rowid, title, body) VALUES ('delete', old.rowid, old.title, old.body);"
    "  INSERT INTO node_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS nodes_ad AFTER DELETE ON nodes BEGIN "
    "  INSERT INTO node_fts(node_fts, rowid, title, body) VALUES ('delete', old.rowid, old.title, old.body);"
    "END;"
    "CREATE TABLE IF NOT EXISTS keywords ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  text TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    "  canonical_id INTEGER REFERENCES keywords(id),"
    "  embedding BLOB"
    ");"
    "CREATE TABLE IF NOT EXISTS node_keywords ("
    "  node_id BLOB NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,"
    "  keyword_id INTEGER NOT NULL REFERENCES keywords(id),"
    "  PRIMARY KEY (node_id, keyword_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_nk_kw ON node_keywords(keyword_id);"
    "CREATE TABLE IF NOT EXISTS edges ("
    "  src BLOB NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,"
    "  dst BLOB NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,"
    "  kind INTEGER NOT NULL,"
    "  keyword_id INTEGER,"
    "  weight REAL NOT NULL"
    ");"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_edges_unique ON edges(src, dst, kind, COALESCE(keyword_id, -1));"
    "CREATE INDEX IF NOT EXISTS idx_edges_src ON edges(src, kind);"
    "CREATE INDEX IF NOT EXISTS idx_edges_dst ON edges(dst, kind);"
    "CREATE TABLE IF NOT EXISTS similarity_samples ("
    "  ts INTEGER NOT NULL,"
    "  kind INTEGER NOT NULL,"
    "  cosine REAL NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_samples_kind ON similarity_samples(kind);";
}

/* Pre-CREATE migration. Detects pre-rename schema (columns `summary` / `detail`)
 * and brings the DB up to the current names plus the new author/expires_at
 * columns. FTS5 mirrors nodes' columns by name, so we drop and recreate it
 * (and its triggers) and re-populate from the renamed nodes. Idempotent: a
 * fresh DB has no `summary` column, so this is a no-op. Returns the SQL
 * script to execute, or NULL if no migration is needed. */
const char *mg_storage_migration_v2_sql(void) {
  return
    "BEGIN IMMEDIATE;"
    /* Triggers and FTS5 reference the old column names — drop them first. */
    "DROP TRIGGER IF EXISTS nodes_ai;"
    "DROP TRIGGER IF EXISTS nodes_au;"
    "DROP TRIGGER IF EXISTS nodes_ad;"
    "DROP TABLE IF EXISTS node_fts;"
    /* Rename columns. RENAME COLUMN supported since SQLite 3.25 (2018). */
    "ALTER TABLE nodes RENAME COLUMN summary TO title;"
    "ALTER TABLE nodes RENAME COLUMN detail TO body;"
    /* New columns. NULL author for legacy rows; expires_at=0 means none. */
    "ALTER TABLE nodes ADD COLUMN author TEXT;"
    "ALTER TABLE nodes ADD COLUMN expires_at INTEGER NOT NULL DEFAULT 0;"
    /* Recreate FTS5 table and triggers with new column names. */
    "CREATE VIRTUAL TABLE node_fts USING fts5("
    "  title, body,"
    "  content='nodes', content_rowid='rowid',"
    "  tokenize='unicode61 remove_diacritics 2'"
    ");"
    "INSERT INTO node_fts(rowid, title, body) "
    "  SELECT rowid, title, body FROM nodes;"
    "CREATE TRIGGER nodes_ai AFTER INSERT ON nodes BEGIN "
    "  INSERT INTO node_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);"
    "END;"
    "CREATE TRIGGER nodes_au AFTER UPDATE ON nodes BEGIN "
    "  INSERT INTO node_fts(node_fts, rowid, title, body) VALUES ('delete', old.rowid, old.title, old.body);"
    "  INSERT INTO node_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);"
    "END;"
    "CREATE TRIGGER nodes_ad AFTER DELETE ON nodes BEGIN "
    "  INSERT INTO node_fts(node_fts, rowid, title, body) VALUES ('delete', old.rowid, old.title, old.body);"
    "END;"
    "COMMIT;";
}

const char *mg_storage_migration_v3_sql(void) {
  return
    "BEGIN IMMEDIATE;"
    "ALTER TABLE nodes ADD COLUMN origin INTEGER NOT NULL DEFAULT 0;"
    "CREATE INDEX IF NOT EXISTS idx_nodes_origin ON nodes(origin);"
    "COMMIT;";
}

/* v4: provenance (issue #4). Additive and idempotent, so it runs on every
 * open right after the base schema: an older DB just gains two empty tables,
 * no rebuild. One source row per (project, kind, locator); a node links to
 * any number of sources and a source supports any number of nodes. The
 * link's fingerprint/observed_at record the source version the node was
 * derived from (or last revalidated against), while the source row keeps
 * the latest version seen through any node. project is '' for kinds that
 * have none (url, conversation, manual). */
const char *mg_storage_schema_v4_sql(void) {
  return
    "CREATE TABLE IF NOT EXISTS sources ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  project TEXT NOT NULL DEFAULT '',"
    "  kind TEXT NOT NULL,"
    "  locator TEXT NOT NULL,"
    "  fingerprint TEXT,"
    "  observed_at INTEGER NOT NULL,"
    "  UNIQUE(project, kind, locator)"
    ");"
    "CREATE TABLE IF NOT EXISTS node_sources ("
    "  node_id BLOB NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,"
    "  source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,"
    "  role TEXT NOT NULL DEFAULT 'primary',"
    "  fingerprint TEXT,"
    "  observed_at INTEGER NOT NULL,"
    "  PRIMARY KEY (node_id, source_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_ns_source ON node_sources(source_id);";
}

/* v5: maintenance (issue #5). Additive like v4. maintenance_meta holds
 * counters and timestamps (last apply-safe, last scan); maintenance_retired
 * records when a node was retired (state 3) and the state it had, so
 * apply-safe can purge it after the retention window; maintenance_candidates
 * caches the last scan's pending candidates (each one's emitted map as
 * MessagePack) so status and resolve need no re-scan;
 * maintenance_dismissals remembers `keep` decisions per candidate and
 * evidence; maintenance_log is the audit trail of every resolution and
 * apply-safe run. idx_nodes_created makes "inserts since" a range count. */
const char *mg_storage_schema_v5_sql(void) {
  return
    "CREATE INDEX IF NOT EXISTS idx_nodes_created ON nodes(created_at);"
    "CREATE TABLE IF NOT EXISTS maintenance_meta ("
    "  key TEXT PRIMARY KEY,"
    "  value INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS maintenance_retired ("
    "  node_id BLOB PRIMARY KEY REFERENCES nodes(id) ON DELETE CASCADE,"
    "  retired_at INTEGER NOT NULL,"
    "  prev_state INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS maintenance_candidates ("
    "  id TEXT PRIMARY KEY,"
    "  kind TEXT NOT NULL,"
    "  priority INTEGER NOT NULL,"
    "  score REAL NOT NULL,"
    "  nodes TEXT NOT NULL,"
    "  evidence TEXT NOT NULL,"
    "  payload BLOB NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS maintenance_dismissals ("
    "  candidate_id TEXT PRIMARY KEY,"
    "  kind TEXT NOT NULL,"
    "  evidence TEXT NOT NULL,"
    "  dismissed_at INTEGER NOT NULL,"
    "  note TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS maintenance_log ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts INTEGER NOT NULL,"
    "  actor TEXT NOT NULL,"
    "  action TEXT NOT NULL,"
    "  candidate_id TEXT,"
    "  kind TEXT,"
    "  nodes TEXT NOT NULL DEFAULT '',"
    "  detail TEXT,"
    "  note TEXT"
    ");";
}
