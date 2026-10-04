/* Bootstrap coverage state (issue #3): the `graft project` state file
 * (path, load / save / mark round trip, sanitizing, drop), the provenance
 * counts read straight from the DB, and the status report. No embedding
 * model and no daemon: nodes are inserted straight through storage. */

#include "graft/storage.h"
#include "graft/types.h"
#include "../src/cli/project.h"
#include "mpack.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "test_project:%d: %s\n", __LINE__, msg); \
    g_fail++; \
  } \
} while (0)

static const char *DB = "project_test.db";
static const char *PROJECT = "github.com/Owner/Repo";

static void cleanup_db(const char *path) {
  char p[512];
  remove(path);
  snprintf(p, sizeof(p), "%s-wal", path);
  remove(p);
  snprintf(p, sizeof(p), "%s-shm", path);
  remove(p);
}

static const mg_project_topic_t *find(const mg_project_state_t *st, const char *name) {
  for (size_t i = 0; i < st->n_topics; i++) {
    if (!strcmp(st->topics[i].name, name)) return &st->topics[i];
  }
  return NULL;
}

static void test_state_path(void) {
  char a[512], b[512], c[512];
  CHECK(mg_project_state_path("/home/u/.graft/profiles/default/graft.db", PROJECT, a, sizeof(a)) == 0,
        "state path");
  CHECK(strncmp(a, "/home/u/.graft/profiles/default/projects/", 41) == 0, "state dir next to the DB");
  CHECK(strlen(a) == 41 + 32 + 4 && !strcmp(a + strlen(a) - 4, ".tsv"), "32 hex + .tsv");
  CHECK(mg_project_state_path("C:\\g\\graft.db", PROJECT, b, sizeof(b)) == 0 &&
        strncmp(b, "C:\\g/projects/", 14) == 0, "windows separators");
  CHECK(mg_project_state_path("/x/graft.db", "github.com/Owner/Other", c, sizeof(c)) == 0 &&
        strcmp(a + 41, c + 12) != 0, "one file per project");
  CHECK(mg_project_state_path("graft.db", PROJECT, c, sizeof(c)) == 0 &&
        strncmp(c, "projects/", 9) == 0, "bare db name");
}

static void test_state_roundtrip(void) {
  char path[512];
  mg_project_state_t st, back;
  CHECK(mg_project_state_path(DB, PROJECT, path, sizeof(path)) == 0, "path");
  remove(path);

  CHECK(mg_project_state_load(path, &st) == 0, "missing file loads");
  CHECK(st.n_topics == 0 && st.runs == 0 && st.last_run_at == 0, "missing file is empty");

  CHECK(mg_project_state_mark(&st, "storage engine", "covered", 0, NULL, 10) == 0, "mark covered");
  CHECK(mg_project_state_mark(&st, "  http\tviewer\n", "pending", 3, "web/ and src/http", 11) == 0,
        "mark pending low");
  CHECK(mg_project_state_mark(&st, "daemon protocol", "pending", 1, "src/daemon", 12) == 0,
        "mark pending high");
  CHECK(mg_project_state_mark(&st, "vendored deps", "skipped", 0, "third_party: not ours", 13) == 0,
        "mark skipped");
  CHECK(mg_project_state_mark(&st, "x", "bogus", 0, NULL, 14) == -1, "bad state rejected");
  CHECK(mg_project_state_mark(&st, " \t ", "covered", 0, NULL, 14) == -1, "empty name rejected");
  CHECK(st.n_topics == 4, "4 topics");

  const mg_project_topic_t *t = find(&st, "http viewer");
  CHECK(t != NULL, "name sanitized and trimmed");
  CHECK(t && t->priority == 3 && !strcmp(t->state, "pending"), "priority low kept");

  /* update: keeps note and priority when not given */
  CHECK(mg_project_state_mark(&st, "daemon protocol", "covered", 0, NULL, 20) == 0, "update");
  t = find(&st, "daemon protocol");
  CHECK(t && !strcmp(t->state, "covered") && t->priority == 1 && !strcmp(t->note, "src/daemon") &&
        t->updated_at == 20, "update keeps note/priority");
  CHECK(mg_project_state_mark(&st, "nothing", "drop", 0, NULL, 21) == 0 && st.n_topics == 4,
        "drop of unknown topic is a no-op");

  st.runs = 2;
  st.last_run_at = 1700000000000LL;
  CHECK(mg_project_state_save(path, PROJECT, &st) == 0, "save");

  CHECK(mg_project_state_load(path, &back) == 0, "reload");
  CHECK(back.n_topics == 4 && back.runs == 2 && back.last_run_at == 1700000000000LL, "reload header");
  for (size_t i = 0; i < st.n_topics && i < back.n_topics; i++) {
    CHECK(!strcmp(st.topics[i].name, back.topics[i].name), "order preserved");
    CHECK(!strcmp(st.topics[i].state, back.topics[i].state), "state preserved");
    CHECK(!strcmp(st.topics[i].note, back.topics[i].note), "note preserved");
    CHECK(st.topics[i].priority == back.topics[i].priority, "priority preserved");
    CHECK(st.topics[i].updated_at == back.topics[i].updated_at, "updated_at preserved");
  }

  CHECK(mg_project_state_mark(&back, "storage engine", "drop", 0, NULL, 30) == 0, "drop");
  CHECK(back.n_topics == 3 && !find(&back, "storage engine"), "dropped");
  CHECK(!strcmp(back.topics[0].name, "http viewer"), "drop keeps order");

  mg_project_state_free(&st);
  mg_project_state_free(&back);

  /* garbage and unknown lines are ignored, not fatal */
  FILE *fp = fopen(path, "ab");
  fputs("future-field\tsomething\ntopic\tweird\t2\t1\tbad\t\ntopic\tpending\t2\t5\tnew one\n", fp);
  fclose(fp);
  CHECK(mg_project_state_load(path, &back) == 0, "load with extra lines");
  CHECK(back.n_topics == 5 && find(&back, "new one") && !find(&back, "bad"), "lenient parse");
  CHECK(find(&back, "new one") && !strcmp(find(&back, "new one")->note, ""), "note optional");
  mg_project_state_free(&back);
  remove(path);
}

static void fill_node(mg_node_t *n, const char *title) {
  memset(n, 0, sizeof(*n));
  mg_uuidv7(n->id);
  mg_blake3((const uint8_t *)title, strlen(title), n->content_hash);
  n->title = (char *)title;
  n->body = (char *)"body";
  n->created_at = 1;
  n->last_access = 1;
  n->state = MG_NODE_ACTIVE;
}

static mg_source_t src(const char *project, const char *kind, const char *locator) {
  mg_source_t s;
  memset(&s, 0, sizeof(s));
  s.project = (char *)project;
  s.kind = (char *)kind;
  s.locator = (char *)locator;
  s.fingerprint = (char *)"ff";
  s.observed_at = 1;
  return s;
}

static void test_provenance(void) {
  int64_t files = -1, nodes = -1;
  char err[256];
  mg_storage_t *s = NULL;
  static mg_embedding_t emb;
  mg_node_t a, b, c, d;

  cleanup_db(DB);
  CHECK(mg_project_provenance(DB, PROJECT, &files, &nodes, err, sizeof(err)) == 1 && files == 0 && nodes == 0,
        "no DB yet");

  CHECK(mg_storage_open(DB, &s) == MG_OK && mg_storage_apply_schema(s) == MG_OK, "open");
  for (int i = 0; i < MG_EMBEDDING_DIM; ++i) emb[i] = 0.001f * (float)(i % 7);

  mg_source_t s_a[2] = { src(PROJECT, "file", "README.md"), src(PROJECT, "file", "docs/x.md") };
  mg_source_t s_b[1] = { src(PROJECT, "file", "README.md") };
  mg_source_t s_c[2] = { src("github.com/Owner/Other", "file", "README.md"), src("", "url", "https://e.x") };
  fill_node(&a, "node a");
  fill_node(&b, "node b");
  fill_node(&c, "node c");
  CHECK(mg_storage_insert_node_with_sources(s, &a, emb, NULL, 0, NULL, 0, NULL, s_a, 2) == MG_OK, "a");
  CHECK(mg_storage_insert_node_with_sources(s, &b, emb, NULL, 0, NULL, 0, NULL, s_b, 1) == MG_OK, "b");
  CHECK(mg_storage_insert_node_with_sources(s, &c, emb, NULL, 0, NULL, 0, NULL, s_c, 2) == MG_OK, "c");

  /* read while the writer connection is still open, like beside a daemon */
  CHECK(mg_project_provenance(DB, PROJECT, &files, &nodes, err, sizeof(err)) == 0, "counts");
  CHECK(files == 2 && nodes == 2, "2 files back 2 nodes of this project");

  /* superseding b with d (no sources) drops b from the counts */
  fill_node(&d, "node d");
  CHECK(mg_storage_insert_node_with_sources(s, &d, emb, NULL, 0, NULL, 0,
                                            (const mg_node_id_t *)&b.id, NULL, 0) == MG_OK, "supersede");
  CHECK(mg_project_provenance(DB, PROJECT, &files, &nodes, err, sizeof(err)) == 0 && files == 2 && nodes == 1,
        "superseded node not counted");
  CHECK(mg_storage_delete_node(s, a.id) == MG_OK, "delete a");
  CHECK(mg_project_provenance(DB, PROJECT, &files, &nodes, err, sizeof(err)) == 0 && files == 0 && nodes == 0,
        "sources without live nodes not counted");
  CHECK(mg_project_provenance(DB, "github.com/Owner/Other", &files, &nodes, err, sizeof(err)) == 0 && files == 1 &&
        nodes == 1, "other project");
  mg_storage_close(s);
  cleanup_db(DB);

  /* a DB written by a graft that predates provenance */
  sqlite3 *raw = NULL;
  CHECK(sqlite3_open(DB, &raw) == SQLITE_OK &&
        sqlite3_exec(raw, "CREATE TABLE nodes(id BLOB PRIMARY KEY, state INTEGER);", NULL, NULL, NULL) ==
        SQLITE_OK, "old db");
  sqlite3_close(raw);
  CHECK(mg_project_provenance(DB, PROJECT, &files, &nodes, err, sizeof(err)) == -1, "old db unreadable");
  CHECK(strstr(err, "no provenance tables") != NULL, "old db reason");
  cleanup_db(DB);
}

static mpack_node_t map_get(mpack_node_t m, const char *k) {
  return mpack_node_map_cstr_optional(m, k);
}

static int str_is(mpack_node_t n, const char *s) {
  return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
         memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

static void test_report(void) {
  mg_project_state_t st;
  char *out = NULL;
  size_t len = 0;
  memset(&st, 0, sizeof(st));
  mg_project_state_mark(&st, "low one", "pending", 3, "later", 1);
  mg_project_state_mark(&st, "normal one", "pending", 0, NULL, 2);
  mg_project_state_mark(&st, "high one", "pending", 1, "now", 3);
  mg_project_state_mark(&st, "done", "covered", 0, NULL, 4);
  mg_project_state_mark(&st, "noise", "skipped", 0, NULL, 5);

  CHECK(mg_project_status_report(PROJECT, "/r", "default", "/r/s.tsv", &st, 0, "", 7, 9, &out, &len) == 0,
        "report");
  mpack_tree_t tree;
  mpack_tree_init_data(&tree, out, len);
  mpack_tree_parse(&tree);
  CHECK(mpack_tree_error(&tree) == mpack_ok, "report parses");
  mpack_node_t r = map_get(mpack_tree_root(&tree), "result");
  CHECK(str_is(map_get(r, "project"), PROJECT), "project");
  CHECK(mpack_node_type(map_get(r, "bootstrapped")) == mpack_type_bool &&
        !mpack_node_bool(map_get(r, "bootstrapped")), "not bootstrapped before a run");
  CHECK(mpack_node_is_nil(map_get(r, "last_run_at")), "no last run");
  CHECK(mpack_node_i64(map_get(map_get(r, "provenance"), "files")) == 7, "provenance files");
  CHECK(mpack_node_i64(map_get(map_get(r, "topics"), "pending")) == 3, "3 pending");
  CHECK(mpack_node_i64(map_get(map_get(r, "topics"), "covered")) == 1, "1 covered");
  CHECK(mpack_node_i64(map_get(map_get(r, "topics"), "skipped")) == 1, "1 skipped");
  mpack_node_t pend = map_get(r, "pending");
  CHECK(mpack_node_array_length(pend) == 3, "pending list");
  if (mpack_node_array_length(pend) == 3) {
    CHECK(str_is(map_get(mpack_node_array_at(pend, 0), "topic"), "high one"), "high first");
    CHECK(str_is(map_get(mpack_node_array_at(pend, 0), "priority"), "high"), "priority name");
    CHECK(str_is(map_get(mpack_node_array_at(pend, 1), "topic"), "normal one"), "normal second");
    CHECK(str_is(map_get(mpack_node_array_at(pend, 2), "topic"), "low one"), "low last");
  }
  CHECK(mpack_node_array_length(map_get(r, "covered")) == 1 &&
        str_is(mpack_node_array_at(map_get(r, "covered"), 0), "done"), "covered names");
  mpack_tree_destroy(&tree);
  free(out);

  st.runs = 1;
  st.last_run_at = 42;
  CHECK(mg_project_status_report(PROJECT, "/r", "default", "/r/s.tsv", &st, -1, "no provenance tables yet", 0, 0, &out, &len) == 0,
        "report 2");
  mpack_tree_init_data(&tree, out, len);
  mpack_tree_parse(&tree);
  r = map_get(mpack_tree_root(&tree), "result");
  CHECK(mpack_node_bool(map_get(r, "bootstrapped")), "bootstrapped after a run");
  CHECK(mpack_node_i64(map_get(r, "last_run_at")) == 42, "last run");
  CHECK(mpack_node_is_nil(map_get(r, "provenance")), "unreadable provenance is null");
  CHECK(str_is(map_get(r, "provenance_error"), "no provenance tables yet"), "provenance error");
  mpack_tree_destroy(&tree);
  free(out);
  mg_project_state_free(&st);
}

int main(void) {
  test_state_path();
  test_state_roundtrip();
  test_provenance();
  test_report();
  if (g_fail) {
    fprintf(stderr, "test_project: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("ok project\n");
  return 0;
}
