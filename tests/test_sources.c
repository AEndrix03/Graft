/* Provenance (issue #4): locator parsing and project identity, storage of
 * node<->source links, the sources ops, the `sources diff` classification,
 * and provenance surviving merge / remote push / pull. No embedding model:
 * nodes are inserted straight through storage. */

#include "graft/ops.h"
#include "graft/source.h"
#include "graft/storage.h"
#include "graft/types.h"
#include "../src/cli/sources.h"
#include "mpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  define mk_dir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define mk_dir(p) mkdir((p), 0755)
#endif

static int g_fail = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "test_sources:%d: %s\n", __LINE__, msg); \
    g_fail++; \
  } \
} while (0)

static void write_file(const char *path, const char *content) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "cannot write %s\n", path);
    exit(1);
  }
  fputs(content, fp);
  fclose(fp);
}

static void cleanup_db(const char *path) {
  char p[512];
  remove(path);
  snprintf(p, sizeof(p), "%s-wal", path);
  remove(p);
  snprintf(p, sizeof(p), "%s-shm", path);
  remove(p);
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

static mg_err_t insert_plain(mg_storage_t *s, mg_node_t *n, const mg_source_t *src, size_t n_src) {
  static mg_embedding_t emb;
  for (int i = 0; i < MG_EMBEDDING_DIM; ++i) emb[i] = 0.001f * (float)(i % 7);
  return mg_storage_insert_node_with_sources(s, n, emb, NULL, 0, NULL, 0, NULL, src, n_src);
}

static mg_source_t file_source(const mg_source_spec_t *spec, int64_t observed_at) {
  mg_source_t s;
  memset(&s, 0, sizeof(s));
  s.project = spec->project;
  s.kind = (char *)spec->kind;
  s.locator = spec->locator;
  s.fingerprint = (char *)spec->fingerprint;
  s.observed_at = observed_at;
  return s;
}

static size_t count_links(mg_storage_t *s, const char *project, const mg_node_id_t *node) {
  mg_source_link_t *links = NULL;
  size_t n = 0;
  if (mg_storage_source_links(s, project, NULL, node, &links, &n) != MG_OK) return (size_t)-1;
  mg_source_links_free(links, n);
  return n;
}

/* fingerprint recorded on node's link to locator, "" if none */
static void link_fp(mg_storage_t *s, const mg_node_id_t node, const char *locator, char out[65]) {
  mg_source_link_t *links = NULL;
  size_t n = 0;
  out[0] = '\0';
  if (mg_storage_source_links(s, NULL, NULL, (const mg_node_id_t *)node, &links, &n) != MG_OK) return;
  for (size_t i = 0; i < n; i++) {
    if (!strcmp(links[i].source.locator, locator) && links[i].source.fingerprint) {
      snprintf(out, 65, "%s", links[i].source.fingerprint);
    }
  }
  mg_source_links_free(links, n);
}

/* ---------- locators and project identity ---------- */

static void test_normalize_remote(void) {
  static const char *const cases[][2] = {
    { "https://user:tok@GitHub.com/Owner/Repo.git", "github.com/Owner/Repo" },
    { "git@github.com:Owner/Repo.git",              "github.com/Owner/Repo" },
    { "ssh://git@GitHub.com:22/Owner/Repo/",        "github.com/Owner/Repo" },
    { "http://gitlab.local:8080/a/b",               "gitlab.local:8080/a/b" },
    { "/srv/git/repo.git",                          "/srv/git/repo" },
  };
  char out[256];
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    int rc = mg_source_normalize_remote(cases[i][0], out, sizeof(out));
    if (rc != 0 || strcmp(out, cases[i][1]) != 0) {
      fprintf(stderr, "normalize '%s' -> '%s' (want '%s')\n", cases[i][0],
              rc == 0 ? out : "<error>", cases[i][1]);
      g_fail++;
    }
  }
}

static void make_repo(void) {
  mk_dir("prov_repo");
  mk_dir("prov_repo/.git");
  mk_dir("prov_repo/.git/worktrees");
  mk_dir("prov_repo/.git/worktrees/wt");
  mk_dir("prov_repo/src");
  write_file("prov_repo/.git/config",
             "[core]\n\tbare = false\n"
             "[remote \"upstream\"]\n\turl = https://example.com/other.git\n"
             "[remote \"origin\"]\n\turl = https://user:tok@GitHub.com/Owner/Repo.git\n");
  write_file("prov_repo/.git/worktrees/wt/commondir", "../..\n");
  write_file("prov_repo/src/a.txt", "alpha");
  write_file("prov_repo/src/b.txt", "beta");
  write_file("prov_repo/src/c.txt", "gamma");
  /* a linked worktree: .git is a file pointing into the main repo */
  mk_dir("prov_wt");
  write_file("prov_wt/.git", "gitdir: ../prov_repo/.git/worktrees/wt\n");
  write_file("prov_wt/a.txt", "alpha");
}

static void test_parse(void) {
  mg_source_spec_t spec;
  char err[512];
  char hex[65];

  CHECK(mg_source_parse("file:prov_repo/src/a.txt", 1, &spec, err, sizeof(err)) == 0, err);
  CHECK(!strcmp(spec.kind, "file"), "file kind");
  CHECK(spec.project && !strcmp(spec.project, "github.com/Owner/Repo"), "project from origin remote");
  CHECK(spec.locator && !strcmp(spec.locator, "src/a.txt"), "locator relative to the repo root");
  CHECK(mg_source_hash_file("prov_repo/src/a.txt", hex) == 0 && !strcmp(hex, spec.fingerprint),
        "fingerprint is BLAKE3 of the bytes");
  mg_source_spec_free(&spec);

  /* the same file content in a linked worktree resolves to the same project */
  CHECK(mg_source_parse("file:prov_wt/a.txt", 1, &spec, err, sizeof(err)) == 0, err);
  CHECK(spec.project && !strcmp(spec.project, "github.com/Owner/Repo"), "worktree shares the project");
  CHECK(spec.locator && !strcmp(spec.locator, "a.txt"), "worktree locator");
  mg_source_spec_free(&spec);

  CHECK(mg_source_parse("file:prov_repo/src/missing.txt", 1, &spec, err, sizeof(err)) != 0,
        "missing file is an error");
  CHECK(strstr(err, "not found") != NULL, "missing file message");
  CHECK(mg_source_parse("file:prov_repo/src", 1, &spec, err, sizeof(err)) != 0, "a directory is rejected");
  CHECK(mg_source_parse("file:prov_repo/src/a.txt", 0, &spec, err, sizeof(err)) != 0,
        "relative path rejected when not allowed");
  CHECK(mg_source_parse("bogus:x", 1, &spec, err, sizeof(err)) != 0, "unknown kind");
  CHECK(mg_source_parse("url:", 1, &spec, err, sizeof(err)) != 0, "empty url");

  CHECK(mg_source_parse("url:https://example.com/doc", 1, &spec, err, sizeof(err)) == 0, err);
  CHECK(!strcmp(spec.kind, "url") && !strcmp(spec.locator, "https://example.com/doc") &&
        !spec.project[0] && !spec.fingerprint[0], "url source");
  mg_source_spec_free(&spec);
  CHECK(mg_source_parse("conversation", 1, &spec, err, sizeof(err)) == 0, err);
  CHECK(!strcmp(spec.kind, "conversation") && !spec.locator[0], "conversation source");
  mg_source_spec_free(&spec);
  CHECK(mg_source_parse("manual:notes", 1, &spec, err, sizeof(err)) == 0, err);
  CHECK(!strcmp(spec.kind, "manual") && !strcmp(spec.locator, "notes"), "tagged manual source");
  mg_source_spec_free(&spec);

  CHECK(mg_source_hash_file("prov_repo/src/missing.txt", hex) == 1, "hash of a missing file");
}

static void test_project_without_git(void) {
  const char *tmp = getenv("TMPDIR");
  char dir[1024], root[4096], project[4096];
  if (!tmp || !*tmp) tmp = getenv("TEMP");
  if (!tmp || !*tmp) tmp = "/tmp";
  snprintf(dir, sizeof(dir), "%s/graft_prov_nogit", tmp);
  mk_dir(dir);
  CHECK(mg_source_resolve_project(dir, root, sizeof(root), project, sizeof(project)) == 0,
        "resolve a non-git directory");
  CHECK(!strcmp(root, project), "without a remote the project id is the root path");
  CHECK(strchr(root, '\\') == NULL, "root uses '/' separators");
  CHECK(mg_source_resolve_project("prov_repo/src", root, sizeof(root), project, sizeof(project)) == 0 &&
        !strcmp(project, "github.com/Owner/Repo"), "a subdirectory resolves to its repository");
}

/* ---------- storage ---------- */

static void test_storage_links(void) {
  mg_storage_t *s = NULL;
  mg_source_spec_t a, b;
  mg_node_t n1, n2;
  char err[256], fp[65];
  int64_t updated = 0;
  mg_storage_consolidate_report_t rep;

  if (open_schema(":memory:", &s) != 0) { g_fail++; return; }
  CHECK(mg_source_parse("file:prov_repo/src/a.txt", 1, &a, err, sizeof(err)) == 0, err);
  CHECK(mg_source_parse("file:prov_repo/src/b.txt", 1, &b, err, sizeof(err)) == 0, err);

  mg_source_t srcs[2] = { file_source(&a, 1000), file_source(&b, 1000) };
  mg_source_t conv;
  memset(&conv, 0, sizeof(conv));
  conv.kind = (char *)"conversation";
  conv.locator = (char *)"";
  conv.observed_at = 1000;

  fill_node(&n1, "with sources");
  fill_node(&n2, "no sources");
  CHECK(insert_plain(s, &n1, srcs, 2) == MG_OK, "insert with sources");
  CHECK(insert_plain(s, &n2, NULL, 0) == MG_OK, "insert without sources");
  CHECK(count_links(s, NULL, (const mg_node_id_t *)&n1.id) == 2, "two links");
  CHECK(count_links(s, NULL, (const mg_node_id_t *)&n2.id) == 0, "a node without sources has none");
  CHECK(count_links(s, a.project, NULL) == 2, "links by project");
  CHECK(count_links(s, "github.com/other/repo", NULL) == 0, "other project is empty");

  /* attaching again: the conversation source is added, a's link takes the
   * newer fingerprint, nothing is duplicated */
  mg_source_t again[2] = { file_source(&a, 2000), conv };
  again[0].fingerprint = (char *)"0000000000000000000000000000000000000000000000000000000000000000";
  CHECK(mg_storage_attach_sources(s, n1.id, again, 2) == MG_OK, "attach to existing node");
  CHECK(count_links(s, NULL, (const mg_node_id_t *)&n1.id) == 3, "attach accumulates");
  link_fp(s, n1.id, "src/a.txt", fp);
  CHECK(fp[0] == '0' && fp[1] == '0', "re-attach updates the link fingerprint");

  mg_node_id_t ghost;
  memset(ghost, 0x7f, sizeof(ghost));
  CHECK(mg_storage_attach_sources(s, ghost, again, 1) == MG_ERR_NOT_FOUND, "attach to a missing node");

  CHECK(mg_storage_refresh_source(s, n1.id, a.project, "file", "src/a.txt", a.fingerprint,
                                  3000, &updated) == MG_OK && updated == 1, "refresh one link");
  link_fp(s, n1.id, "src/a.txt", fp);
  CHECK(!strcmp(fp, a.fingerprint), "refresh stores the new fingerprint");
  CHECK(mg_storage_refresh_source(s, n2.id, a.project, "file", "src/a.txt", a.fingerprint,
                                  3000, &updated) == MG_OK && updated == 0,
        "refresh of an unlinked source changes nothing");

  /* delete cascades the links; consolidate prunes the orphan source rows */
  CHECK(mg_storage_delete_node(s, n1.id) == MG_OK, "delete node");
  CHECK(count_links(s, NULL, NULL) == 0, "links cascade with the node");
  CHECK(mg_storage_consolidate(s, &rep) == MG_OK, "consolidate");
  CHECK(rep.orphan_sources_deleted == 3, "orphan sources pruned");

  mg_source_spec_free(&a);
  mg_source_spec_free(&b);
  mg_storage_close(s);
}

/* ---------- ops + diff report ---------- */

static mpack_tree_t g_tree;
static void *g_resp;

/* dispatch {op, args} and return the result map (valid until the next call) */
static mpack_node_t dispatch(mg_ctx_t *ctx, const char *op, const char *args, size_t args_len,
                             int *status) {
  char *req = NULL;
  size_t req_len = 0, resp_len = 0;
  mpack_writer_t w;
  if (g_resp) {
    mpack_tree_destroy(&g_tree);
    free(g_resp);
    g_resp = NULL;
  }
  mpack_writer_init_growable(&w, &req, &req_len);
  mpack_start_map(&w, 2);
  mpack_write_cstr(&w, "op");
  mpack_write_cstr(&w, op);
  mpack_write_cstr(&w, "args");
  mpack_write_object_bytes(&w, args, args_len);
  mpack_finish_map(&w);
  mpack_writer_destroy(&w);
  mg_dispatch(ctx, req, req_len, &g_resp, &resp_len);
  free(req);
  mpack_tree_init_data(&g_tree, (const char *)g_resp, resp_len);
  mpack_tree_parse(&g_tree);
  mpack_node_t root = mpack_tree_root(&g_tree);
  *status = (int)mpack_node_int(mpack_node_map_cstr(root, "status"));
  return mpack_node_map_cstr(root, "result");
}

static int64_t map_int(mpack_node_t map, const char *key) {
  return mpack_node_i64(mpack_node_map_cstr(map, key));
}

static int map_str_eq(mpack_node_t map, const char *key, const char *want) {
  mpack_node_t n = mpack_node_map_cstr_optional(map, key);
  return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(want) &&
         memcmp(mpack_node_str(n), want, strlen(want)) == 0;
}

/* the report entry for locator; the first entry when absent (the state
 * checks below then fail) */
static mpack_node_t find_source(mpack_node_t sources, const char *locator) {
  for (size_t i = 0; i < mpack_node_array_length(sources); i++) {
    mpack_node_t s = mpack_node_array_at(sources, i);
    if (map_str_eq(s, "locator", locator)) return s;
  }
  fprintf(stderr, "test_sources: no report entry for %s\n", locator);
  g_fail++;
  return mpack_node_array_at(sources, 0);
}

/* Run sources_list for the project and the diff report over it; returns the
 * parsed report tree's result map in *tree (caller destroys tree + buf). */
static mpack_node_t diff(mg_ctx_t *ctx, const char *root, const char *project,
                         mpack_tree_t *tree, char **buf) {
  char *args = NULL;
  size_t args_len = 0, out_len = 0;
  int status = -1;
  mpack_writer_t w;
  mpack_writer_init_growable(&w, &args, &args_len);
  mpack_start_map(&w, 2);
  mpack_write_cstr(&w, "project"); mpack_write_cstr(&w, project);
  mpack_write_cstr(&w, "kind");    mpack_write_cstr(&w, "file");
  mpack_finish_map(&w);
  mpack_writer_destroy(&w);
  mpack_node_t list = dispatch(ctx, "sources_list", args, args_len, &status);
  free(args);
  CHECK(status == 0, "sources_list ok");
  *buf = NULL;
  CHECK(mg_sources_diff_report(list, root, project, 0, buf, &out_len) == 0, "diff report");
  mpack_tree_init_data(tree, *buf, out_len);
  mpack_tree_parse(tree);
  return mpack_node_map_cstr(mpack_tree_root(tree), "result");
}

static void test_ops_and_diff(void) {
  mg_storage_t *s = NULL;
  mg_ctx_t ctx;
  mg_source_spec_t a, b, c;
  mg_node_t n1, n2, n3;
  char err[256], root[4096], project[4096];
  char *buf = NULL;
  mpack_tree_t tree;
  int status = -1;

  if (open_schema(":memory:", &s) != 0) { g_fail++; return; }
  memset(&ctx, 0, sizeof(ctx));
  ctx.storage = s;

  write_file("prov_repo/src/b.txt", "beta");
  write_file("prov_repo/src/c.txt", "gamma");
  CHECK(mg_source_parse("file:prov_repo/src/a.txt", 1, &a, err, sizeof(err)) == 0, err);
  CHECK(mg_source_parse("file:prov_repo/src/b.txt", 1, &b, err, sizeof(err)) == 0, err);
  CHECK(mg_source_parse("file:prov_repo/src/c.txt", 1, &c, err, sizeof(err)) == 0, err);
  CHECK(mg_source_resolve_project("prov_repo", root, sizeof(root), project, sizeof(project)) == 0,
        "resolve repo");

  mg_source_t s1[2] = { file_source(&a, 1000), file_source(&b, 1000) };
  mg_source_t s2[1] = { file_source(&c, 1000) };
  fill_node(&n1, "derived from a and b");
  fill_node(&n2, "derived from c");
  fill_node(&n3, "plain");
  CHECK(insert_plain(s, &n1, s1, 2) == MG_OK, "insert n1");
  CHECK(insert_plain(s, &n2, s2, 1) == MG_OK, "insert n2");
  CHECK(insert_plain(s, &n3, NULL, 0) == MG_OK, "insert n3");

  /* get shows the sources */
  {
    char id_hex[33], *args = NULL;
    size_t args_len = 0;
    mpack_writer_t w;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { id_hex[2 * i] = hx[n1.id[i] >> 4]; id_hex[2 * i + 1] = hx[n1.id[i] & 15]; }
    id_hex[32] = '\0';
    mpack_writer_init_growable(&w, &args, &args_len);
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "id_hex"); mpack_write_cstr(&w, id_hex);
    mpack_finish_map(&w);
    mpack_writer_destroy(&w);
    mpack_node_t got = dispatch(&ctx, "get", args, args_len, &status);
    CHECK(status == 0, "get ok");
    mpack_node_t srcs = mpack_node_map_cstr(got, "sources");
    CHECK(mpack_node_array_length(srcs) == 2, "get lists two sources");
    CHECK(map_str_eq(mpack_node_array_at(srcs, 0), "kind", "file") &&
          map_str_eq(mpack_node_array_at(srcs, 0), "locator", "src/a.txt"), "get source fields");
    free(args);
  }

  /* everything fresh */
  mpack_node_t rep = diff(&ctx, root, project, &tree, &buf);
  mpack_node_t sum = mpack_node_map_cstr(rep, "summary");
  CHECK(map_int(sum, "sources") == 3 && map_int(sum, "unchanged") == 3 &&
        map_int(sum, "nodes_to_revalidate") == 0, "fresh diff");
  mpack_tree_destroy(&tree);
  free(buf);

  /* b edited, c deleted */
  write_file("prov_repo/src/b.txt", "beta, edited");
  remove("prov_repo/src/c.txt");
  rep = diff(&ctx, root, project, &tree, &buf);
  sum = mpack_node_map_cstr(rep, "summary");
  CHECK(map_int(sum, "unchanged") == 1 && map_int(sum, "changed") == 1 &&
        map_int(sum, "removed") == 1 && map_int(sum, "nodes_to_revalidate") == 2, "diff after edits");
  mpack_node_t list = mpack_node_map_cstr(rep, "sources");
  mpack_node_t sb = find_source(list, "src/b.txt");
  CHECK(map_str_eq(sb, "state", "changed"), "b changed");
  CHECK(map_str_eq(mpack_node_array_at(mpack_node_map_cstr(sb, "nodes"), 0), "title",
                   "derived from a and b"), "b names its node");
  CHECK(map_str_eq(find_source(list, "src/c.txt"), "state", "removed"), "c removed");
  CHECK(map_str_eq(find_source(list, "src/a.txt"), "state", "unchanged"), "a unchanged");
  mpack_tree_destroy(&tree);
  free(buf);

  /* refresh n1 against the edited b */
  {
    char id_hex[33], fp[65], *args = NULL;
    size_t args_len = 0;
    mpack_writer_t w;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { id_hex[2 * i] = hx[n1.id[i] >> 4]; id_hex[2 * i + 1] = hx[n1.id[i] & 15]; }
    id_hex[32] = '\0';
    CHECK(mg_source_hash_file("prov_repo/src/b.txt", fp) == 0, "hash edited b");
    mpack_writer_init_growable(&w, &args, &args_len);
    mpack_start_map(&w, 2);
    mpack_write_cstr(&w, "id_hex"); mpack_write_cstr(&w, id_hex);
    mpack_write_cstr(&w, "updates");
    mpack_start_array(&w, 1);
    mpack_start_map(&w, 4);
    mpack_write_cstr(&w, "project");     mpack_write_cstr(&w, project);
    mpack_write_cstr(&w, "kind");        mpack_write_cstr(&w, "file");
    mpack_write_cstr(&w, "locator");     mpack_write_cstr(&w, "src/b.txt");
    mpack_write_cstr(&w, "fingerprint"); mpack_write_cstr(&w, fp);
    mpack_finish_map(&w);
    mpack_finish_array(&w);
    mpack_finish_map(&w);
    mpack_writer_destroy(&w);
    mpack_node_t res = dispatch(&ctx, "sources_refresh", args, args_len, &status);
    CHECK(status == 0 && map_int(res, "updated") == 1, "sources_refresh updates one link");
    free(args);
  }
  rep = diff(&ctx, root, project, &tree, &buf);
  sum = mpack_node_map_cstr(rep, "summary");
  CHECK(map_int(sum, "changed") == 0 && map_int(sum, "removed") == 1 &&
        map_int(sum, "nodes_to_revalidate") == 1, "diff after refresh");
  mpack_tree_destroy(&tree);
  free(buf);

  /* the HTTP-style string form and validation of the insert payload */
  {
    char *args = NULL;
    size_t args_len = 0;
    mpack_writer_t w;
    mpack_tree_t t;
    mg_source_t *parsed = NULL;
    size_t n_parsed = 0;
    mpack_writer_init_growable(&w, &args, &args_len);
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "sources");
    mpack_start_array(&w, 2);
    mpack_write_cstr(&w, "url:https://example.com/x");
    mpack_write_cstr(&w, "conversation");
    mpack_finish_array(&w);
    mpack_finish_map(&w);
    mpack_writer_destroy(&w);
    mpack_tree_init_data(&t, args, args_len);
    mpack_tree_parse(&t);
    CHECK(mg_op_parse_sources(mpack_tree_root(&t), 42, &parsed, &n_parsed) == MG_OK && n_parsed == 2 &&
          !strcmp(parsed[0].kind, "url") && parsed[1].observed_at == 42, "string sources parse");
    mg_op_sources_free(parsed, n_parsed);
    mpack_tree_destroy(&t);
    free(args);

    args = NULL;
    mpack_writer_init_growable(&w, &args, &args_len);
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "sources");
    mpack_start_array(&w, 1);
    mpack_start_map(&w, 4);
    mpack_write_cstr(&w, "kind");        mpack_write_cstr(&w, "file");
    mpack_write_cstr(&w, "locator");     mpack_write_cstr(&w, "x.c");
    mpack_write_cstr(&w, "project");     mpack_write_cstr(&w, "p");
    mpack_write_cstr(&w, "fingerprint"); mpack_write_cstr(&w, "not-hex");
    mpack_finish_map(&w);
    mpack_finish_array(&w);
    mpack_finish_map(&w);
    mpack_writer_destroy(&w);
    mpack_tree_init_data(&t, args, args_len);
    mpack_tree_parse(&t);
    CHECK(mg_op_parse_sources(mpack_tree_root(&t), 42, &parsed, &n_parsed) == MG_ERR_INVALID_ARG,
          "bad fingerprint rejected");
    mpack_tree_destroy(&t);
    free(args);
  }

  if (g_resp) {
    mpack_tree_destroy(&g_tree);
    free(g_resp);
    g_resp = NULL;
  }
  write_file("prov_repo/src/b.txt", "beta");
  write_file("prov_repo/src/c.txt", "gamma");
  mg_source_spec_free(&a);
  mg_source_spec_free(&b);
  mg_source_spec_free(&c);
  mg_storage_close(s);
}

/* ---------- merge / push / pull ---------- */

static void test_merge(void) {
  const char *src_path = "./test_sources_src.db";
  const char *dst_path = "./test_sources_dst.db";
  mg_storage_t *src = NULL, *dst = NULL;
  mg_source_spec_t a, b;
  mg_node_t s_same, s_new, d_same;
  char err[256], fp[65];

  cleanup_db(src_path);
  cleanup_db(dst_path);
  CHECK(mg_source_parse("file:prov_repo/src/a.txt", 1, &a, err, sizeof(err)) == 0, err);
  CHECK(mg_source_parse("file:prov_repo/src/b.txt", 1, &b, err, sizeof(err)) == 0, err);

  if (open_schema(src_path, &src) != 0 || open_schema(dst_path, &dst) != 0) { g_fail++; return; }
  mg_source_t sa[1] = { file_source(&a, 5000) };
  mg_source_t sb[1] = { file_source(&b, 5000) };
  mg_source_t da[1] = { file_source(&a, 1000) };
  da[0].fingerprint = (char *)"1111111111111111111111111111111111111111111111111111111111111111";

  /* same content (hash) under different ids on both sides */
  fill_node(&s_same, "shared node");
  fill_node(&d_same, "shared node");
  fill_node(&s_new, "only in source");
  CHECK(insert_plain(src, &s_same, sa, 1) == MG_OK, "src shared");
  CHECK(insert_plain(src, &s_new, sb, 1) == MG_OK, "src new");
  CHECK(insert_plain(dst, &d_same, da, 1) == MG_OK, "dst shared");
  mg_storage_close(src);

  CHECK(mg_storage_merge_from(dst, src_path, 0) == MG_OK, "merge");
  link_fp(dst, d_same.id, "src/a.txt", fp);
  CHECK(!strcmp(fp, a.fingerprint), "newer source link lands on the retained target id");
  CHECK(count_links(dst, NULL, (const mg_node_id_t *)&s_new.id) == 1, "new node keeps its source");
  CHECK(count_links(dst, a.project, NULL) == 2, "no duplicated links");
  CHECK(mg_storage_merge_from(dst, src_path, 1) == MG_OK && count_links(dst, a.project, NULL) == 2,
        "merge is idempotent");
  mg_storage_close(dst);

  cleanup_db(src_path);
  cleanup_db(dst_path);
  mg_source_spec_free(&a);
  mg_source_spec_free(&b);
}

static void test_push_pull(void) {
  const char *local_path = "./test_sources_local.db";
  const char *remote_path = "./test_sources_remote.db";
  mg_storage_t *local = NULL, *remote = NULL;
  mg_source_spec_t a;
  mg_node_t n;
  char err[256], fp[65];
  int64_t pushed = 0, pulled = 0, deleted = 0, updated = 0;
  const char *newer = "2222222222222222222222222222222222222222222222222222222222222222";

  cleanup_db(local_path);
  cleanup_db(remote_path);
  CHECK(mg_source_parse("file:prov_repo/src/a.txt", 1, &a, err, sizeof(err)) == 0, err);
  if (open_schema(remote_path, &remote) != 0) { g_fail++; return; }
  mg_storage_close(remote);
  if (open_schema(local_path, &local) != 0) { g_fail++; return; }

  mg_source_t sa[1] = { file_source(&a, 1000) };
  fill_node(&n, "synced node");
  CHECK(insert_plain(local, &n, sa, 1) == MG_OK, "local insert");
  CHECK(mg_storage_push_to_remote_file(local, remote_path, &pushed) == MG_OK && pushed == 1, "push");

  if (open_schema(remote_path, &remote) != 0) { g_fail++; return; }
  link_fp(remote, n.id, "src/a.txt", fp);
  CHECK(!strcmp(fp, a.fingerprint), "push carries provenance");
  /* revalidated elsewhere, later */
  CHECK(mg_storage_refresh_source(remote, n.id, a.project, "file", "src/a.txt", newer, 9000,
                                  &updated) == MG_OK && updated == 1, "remote refresh");
  mg_storage_close(remote);

  CHECK(mg_storage_pull_remote_file(local, remote_path, &pulled, &deleted) == MG_OK, "pull");
  link_fp(local, n.id, "src/a.txt", fp);
  CHECK(!strcmp(fp, newer), "pull brings back the newer fingerprint");
  mg_storage_close(local);

  cleanup_db(local_path);
  cleanup_db(remote_path);
  mg_source_spec_free(&a);
}

int main(void) {
  test_normalize_remote();
  make_repo();
  test_parse();
  test_project_without_git();
  test_storage_links();
  test_ops_and_diff();
  test_merge();
  test_push_pull();
  if (g_fail) {
    fprintf(stderr, "test_sources: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("ok sources\n");
  return 0;
}
