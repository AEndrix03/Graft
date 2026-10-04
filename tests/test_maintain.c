/* Autonomous maintenance (issue #5): scan candidates of every kind,
 * resolve actions and their reversibility, dismissal suppression, the
 * retention purge, exact-duplicate collapse, the audit log, the status
 * counters, convergence, and retired nodes vanishing from the retrieval
 * paths. No embedding model: nodes go straight through storage with
 * hand-made embeddings, the ops through mg_dispatch. */

#include "graft/maintain.h"
#include "graft/ops.h"
#include "graft/source.h"
#include "graft/storage.h"
#include "graft/types.h"
#include "../src/cli/sources.h"
#include "mpack.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    fprintf(stderr, "test_maintain:%d: %s\n", __LINE__, msg); \
    g_fail++; \
  } \
} while (0)

#define HOUR_MS (3600LL * 1000LL)
#define DAY_MS  (24LL * HOUR_MS)

static int64_t now_ms(void) {
  return (int64_t)time(NULL) * 1000;
}

/* ------------------------------------------------------------- fixtures */

static void cleanup_db(const char *path) {
  char p[512];
  remove(path);
  snprintf(p, sizeof(p), "%s-wal", path);
  remove(p);
  snprintf(p, sizeof(p), "%s-shm", path);
  remove(p);
}

static mg_storage_t *open_db(const char *path) {
  mg_storage_t *s = NULL;
  cleanup_db(path);
  if (mg_storage_open(path, &s) != MG_OK || mg_storage_apply_schema(s) != MG_OK) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  return s;
}

/* Block embedding: 1.0 over dims [64*block, 64*block+64), so different
 * blocks are orthogonal; `tilt` leaks a little weight into the next block
 * (cosine ~0.99 against the untilted vector). */
static void block_emb(mg_embedding_t e, int block, float tilt) {
  memset(e, 0, sizeof(mg_embedding_t));
  for (int i = 0; i < 64; ++i) e[block * 64 + i] = 1.0f;
  if (tilt > 0.0f) {
    for (int i = 0; i < 64; ++i) e[((block + 1) % 16) * 64 + i] = tilt;
  }
}

static void hex(const mg_node_id_t id, char out[33]) {
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < MG_NODE_ID_BYTES; ++i) {
    out[2 * i] = hx[id[i] >> 4];
    out[2 * i + 1] = hx[id[i] & 15];
  }
  out[32] = '\0';
}

/* Inserts an ACTIVE node; hash input = title + body + salt so identical
 * text can still be two rows (different keyword sets in real life). */
static void add_node(mg_storage_t *s, mg_node_id_t id, const char *title, const char *body,
                     const char *salt, int block, float tilt, int64_t created_at,
                     const mg_keyword_id_t *kws, size_t n_kws,
                     const mg_source_t *src, size_t n_src) {
  mg_node_t n;
  mg_embedding_t e;
  char hash_in[512];
  memset(&n, 0, sizeof(n));
  mg_uuidv7(n.id);
  snprintf(hash_in, sizeof(hash_in), "%s|%s|%s", title, body, salt ? salt : "");
  mg_blake3((const uint8_t *)hash_in, strlen(hash_in), n.content_hash);
  n.title = (char *)title;
  n.body = (char *)body;
  n.created_at = created_at;
  n.last_access = created_at;
  n.state = MG_NODE_ACTIVE;
  block_emb(e, block, tilt);
  if (mg_storage_insert_node_with_sources(s, &n, e, kws, n_kws, NULL, 0, NULL, src, n_src) != MG_OK) {
    fprintf(stderr, "insert %s failed\n", title);
    exit(1);
  }
  memcpy(id, n.id, MG_NODE_ID_BYTES);
}

static mg_keyword_id_t kw(mg_storage_t *s, const char *text) {
  mg_keyword_id_t id = 0;
  if (mg_storage_upsert_keyword(s, text, NULL, &id) != MG_OK) exit(1);
  return id;
}

static int node_state(mg_storage_t *s, const mg_node_id_t id) {
  mg_node_t n;
  int st;
  if (mg_storage_get_node(s, id, &n) != MG_OK) return -1;
  st = (int)n.state;
  mg_node_free(&n);
  return st;
}

/* 1 when id is among the vector top-k for the given block */
static int in_vector_search(mg_storage_t *s, const mg_node_id_t id, int block) {
  mg_embedding_t q;
  mg_node_score_t out[16];
  int n = 0;
  block_emb(q, block, 0.0f);
  if (mg_storage_vector_topk(s, q, 16, out, &n) != MG_OK) return 0;
  for (int i = 0; i < n; ++i) {
    if (memcmp(out[i].id, id, MG_NODE_ID_BYTES) == 0) return 1;
  }
  return 0;
}

static int in_fts(mg_storage_t *s, const mg_node_id_t id, const char *text) {
  mg_node_score_t out[16];
  int n = 0;
  if (mg_storage_fts_search(s, text, 16, true, true, out, &n) != MG_OK) return 0;
  for (int i = 0; i < n; ++i) {
    if (memcmp(out[i].id, id, MG_NODE_ID_BYTES) == 0) return 1;
  }
  return 0;
}

/* ------------------------------------------------------------- dispatch */

static mpack_tree_t g_tree;
static void *g_resp;

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
  if (args) mpack_write_object_bytes(&w, args, args_len);
  else { mpack_start_map(&w, 0); mpack_finish_map(&w); }
  mpack_finish_map(&w);
  mpack_writer_destroy(&w);
  mg_dispatch(ctx, req, req_len, &g_resp, &resp_len);
  free(req);
  mpack_tree_init_data(&g_tree, (const char *)g_resp, resp_len);
  mpack_tree_parse(&g_tree);
  mpack_node_t root = mpack_tree_root(&g_tree);
  *status = (int)mpack_node_int(mpack_node_map_cstr_optional(root, "status"));
  return mpack_node_map_cstr_optional(root, "result");
}

/* {op} with string pairs k1, v1, k2, v2, ..., NULL */
static mpack_node_t call(mg_ctx_t *ctx, const char *op, int *status, ...) {
  char *args = NULL;
  size_t len = 0;
  mpack_writer_t w;
  va_list ap;
  const char *k;
  mpack_node_t r;
  mpack_writer_init_growable(&w, &args, &len);
  mpack_build_map(&w);
  va_start(ap, status);
  while ((k = va_arg(ap, const char *)) != NULL) {
    const char *v = va_arg(ap, const char *);
    mpack_write_cstr(&w, k);
    mpack_write_cstr(&w, v);
  }
  va_end(ap);
  mpack_complete_map(&w);
  mpack_writer_destroy(&w);
  r = dispatch(ctx, op, args, len, status);
  free(args);
  return r;
}

static mpack_node_t scan(mg_ctx_t *ctx, int *status) {
  return call(ctx, "maintain_scan", status, NULL);
}

static int str_eq(mpack_node_t n, const char *want) {
  return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(want) &&
         memcmp(mpack_node_str(n), want, strlen(want)) == 0;
}

static int64_t map_int(mpack_node_t map, const char *key) {
  mpack_node_t n = mpack_node_map_cstr_optional(map, key);
  return mpack_node_is_missing(n) ? -1 : mpack_node_i64(n);
}

/* The candidate of `kind` naming node `id` (or any of that kind when id is
 * NULL); copies its id into cid. Returns 1 when found. */
static int find_candidate(mpack_node_t res, const char *kind, const mg_node_id_t id,
                          char cid[MG_MAINT_ID_HEX + 1], mpack_node_t *out) {
  mpack_node_t arr = mpack_node_map_cstr_optional(res, "candidates");
  char want[33];
  if (id) hex(id, want);
  if (mpack_node_type(arr) != mpack_type_array) return 0;
  for (size_t i = 0; i < mpack_node_array_length(arr); ++i) {
    mpack_node_t c = mpack_node_array_at(arr, i);
    mpack_node_t nodes = mpack_node_map_cstr_optional(c, "nodes");
    int match = id == NULL;
    if (!str_eq(mpack_node_map_cstr_optional(c, "kind"), kind)) continue;
    for (size_t j = 0; !match && j < mpack_node_array_length(nodes); ++j) {
      match = str_eq(mpack_node_map_cstr_optional(mpack_node_array_at(nodes, j), "id_hex"), want);
    }
    if (!match) continue;
    if (cid) {
      mpack_node_t idn = mpack_node_map_cstr_optional(c, "id");
      size_t l = mpack_node_strlen(idn);
      if (l > MG_MAINT_ID_HEX) l = MG_MAINT_ID_HEX;
      memcpy(cid, mpack_node_str(idn), l);
      cid[l] = '\0';
    }
    if (out) *out = c;
    return 1;
  }
  return 0;
}

static int64_t pending_of(mpack_node_t scan_res, const char *kind) {
  return map_int(mpack_node_map_cstr_optional(
                   mpack_node_map_cstr_optional(scan_res, "summary"), "by_kind"), kind);
}

static void init_ctx(mg_ctx_t *ctx, mg_storage_t *s, mg_config_t *cfg) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->storage = s;
  ctx->config = cfg;
}

/* ---------------------------------------------------------------- tests */

static void test_pairs_and_resolve(void) {
  const char *path = "test_maintain_pairs.db";
  mg_storage_t *s = open_db(path);
  mg_ctx_t ctx;
  mg_node_id_t a, b, c, d, e, f;
  char ha[33], hb[33], hc[33], cid[MG_MAINT_ID_HEX + 1], cid2[MG_MAINT_ID_HEX + 1];
  int64_t now = now_ms();
  int st;
  mpack_node_t r, cand;
  mg_keyword_id_t k_auth = kw(s, "auth");
  init_ctx(&ctx, s, NULL);

  /* a/b: same vector, no shared keyword -> near_duplicate */
  add_node(s, a, "jwt rotation", "rotate keys", "a", 1, 0.0f, now, NULL, 0, NULL, 0);
  add_node(s, b, "jwt key rotation", "rotate keys weekly", "b", 1, 0.0f, now, NULL, 0, NULL, 0);
  /* c (old) / d (new): near vectors + shared keyword + 2 h apart */
  add_node(s, c, "auth uses sessions", "old", "c", 3, 0.0f, now - 2 * HOUR_MS, &k_auth, 1, NULL, 0);
  add_node(s, d, "auth uses jwt", "new", "d", 3, 0.05f, now, &k_auth, 1, NULL, 0);
  /* e/f: unrelated vectors, a CONTRADICTS edge between them */
  add_node(s, e, "cache ttl is 5m", "x", "e", 6, 0.0f, now, NULL, 0, NULL, 0);
  {
    mg_node_t n;
    mg_embedding_t emb;
    mg_edge_t edge;
    memset(&n, 0, sizeof(n));
    mg_uuidv7(n.id);
    mg_blake3((const uint8_t *)"f", 1, n.content_hash);
    n.title = (char *)"cache ttl is 1h";
    n.body = (char *)"y";
    n.created_at = now;
    n.last_access = now;
    block_emb(emb, 8, 0.0f);
    memset(&edge, 0, sizeof(edge));
    memcpy(edge.src, n.id, MG_NODE_ID_BYTES);
    memcpy(edge.dst, e, MG_NODE_ID_BYTES);
    edge.kind = MG_EDGE_CONTRADICTS;
    edge.weight = 0.8f;
    CHECK(mg_storage_insert_node_with_edges(s, &n, emb, NULL, 0, &edge, 1, NULL) == MG_OK, "insert f");
    memcpy(f, n.id, MG_NODE_ID_BYTES);
  }
  hex(a, ha);
  hex(b, hb);
  hex(c, hc);

  /* scan changes nothing in the graph */
  r = scan(&ctx, &st);
  CHECK(st == 0, "scan ok");
  CHECK(find_candidate(r, "near_duplicate", a, cid, &cand), "near_duplicate a/b");
  CHECK(find_candidate(r, "possible_supersession", c, NULL, &cand), "possible_supersession c/d");
  {
    mpack_node_t nodes = mpack_node_map_cstr_optional(cand, "nodes");
    mpack_node_t sig = mpack_node_map_cstr_optional(cand, "signals");
    CHECK(str_eq(mpack_node_map_cstr_optional(mpack_node_array_at(nodes, 0), "id_hex"), hc),
          "older node is a");
    CHECK(mpack_node_array_length(mpack_node_map_cstr_optional(sig, "shared_keywords")) == 1,
          "shared keyword reported");
  }
  CHECK(!find_candidate(r, "near_duplicate", c, NULL, NULL), "c/d not also a near_duplicate");
  CHECK(find_candidate(r, "contradiction", e, NULL, &cand), "contradiction e/f");
  CHECK(!find_candidate(r, "near_duplicate", e, NULL, NULL), "orthogonal nodes are no duplicates");
  CHECK(node_state(s, a) == MG_NODE_ACTIVE && node_state(s, c) == MG_NODE_ACTIVE, "scan is read-only");
  CHECK(pending_of(r, "contradiction") == 1, "pending by kind");

  /* the same finding keeps its id */
  r = scan(&ctx, &st);
  CHECK(find_candidate(r, "near_duplicate", a, cid2, NULL) && !strcmp(cid, cid2), "deterministic id");

  /* keep dismisses until the evidence changes */
  r = call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "keep", "note", "distinct", NULL);
  CHECK(st == 0, "keep ok");
  r = scan(&ctx, &st);
  CHECK(!find_candidate(r, "near_duplicate", a, NULL, NULL), "dismissed candidate stays hidden");
  CHECK(map_int(mpack_node_map_cstr_optional(r, "summary"), "dismissed") == 1, "dismissed counted");

  /* supersede_a: the older one is superseded by the newer, with a SUPERSEDES edge */
  CHECK(find_candidate(r, "possible_supersession", c, cid, NULL), "supersession still pending");
  r = call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "supersede_a", NULL);
  CHECK(st == 0, "supersede_a ok");
  CHECK(node_state(s, c) == MG_NODE_SUPERSEDED && node_state(s, d) == MG_NODE_ACTIVE, "c superseded by d");
  CHECK(!in_vector_search(s, c, 3), "superseded node leaves vector search");

  /* contradiction: stale one side by node id */
  r = scan(&ctx, &st);
  CHECK(!find_candidate(r, "possible_supersession", c, NULL, NULL), "resolved supersession is gone");
  CHECK(find_candidate(r, "contradiction", e, cid, NULL), "contradiction pending");
  {
    char he[33];
    hex(e, he);
    r = call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "stale", "node", he, NULL);
    CHECK(st == 0, "stale ok");
    CHECK(node_state(s, e) == MG_NODE_STALE, "e stale");
    CHECK(in_vector_search(s, e, 6), "stale nodes stay searchable");
    r = scan(&ctx, &st);
  }
  CHECK(!find_candidate(r, "contradiction", e, NULL, NULL), "contradiction resolved");

  /* converged: only the dismissed near-duplicate existed among a/b, nothing else pending */
  CHECK(map_int(mpack_node_map_cstr_optional(r, "summary"), "pending") == 0, "scan converges");

  /* restore brings the superseded node back and drops the SUPERSEDES edge */
  r = call(&ctx, "maintain_resolve", &st, "node", hc, "action", "restore", NULL);
  CHECK(st == 0, "restore ok");
  CHECK(node_state(s, c) == MG_NODE_ACTIVE && in_vector_search(s, c, 3), "c restored");

  /* invalid: unknown action, missing --by, supersede by itself */
  call(&ctx, "maintain_resolve", &st, "node", ha, "action", "explode", NULL);
  CHECK(st != 0, "unknown action rejected");
  call(&ctx, "maintain_resolve", &st, "node", ha, "action", "supersede", NULL);
  CHECK(st != 0, "supersede without by rejected");
  call(&ctx, "maintain_resolve", &st, "node", ha, "action", "supersede", "by", ha, NULL);
  CHECK(st != 0, "self supersession rejected");
  call(&ctx, "maintain_resolve", &st, "candidate_id", "0000000000000000", "action", "keep", NULL);
  CHECK(st == MG_ERR_NOT_FOUND, "unknown candidate");

  /* the agent wrote a better note g: supersede a and b by node id */
  {
    mg_node_id_t g;
    char hg[33];
    add_node(s, g, "jwt rotation (merged)", "rotate keys weekly", "g", 12, 0.0f, now, NULL, 0, NULL, 0);
    hex(g, hg);
    /* the a/b candidate is dismissed: act on node ids directly */
    r = call(&ctx, "maintain_resolve", &st, "node", ha, "action", "supersede", "by", hg, NULL);
    CHECK(st == 0 && node_state(s, a) == MG_NODE_SUPERSEDED, "supersede by node id");
    r = call(&ctx, "maintain_resolve", &st, "node", hb, "action", "supersede", "by", hg, NULL);
    CHECK(st == 0 && node_state(s, b) == MG_NODE_SUPERSEDED, "supersede b");
  }

  if (g_resp) { mpack_tree_destroy(&g_tree); free(g_resp); g_resp = NULL; }
  mg_storage_close(s);
  cleanup_db(path);
}

static void test_merge_action(void) {
  const char *path = "test_maintain_merge.db";
  mg_storage_t *s = open_db(path);
  mg_ctx_t ctx;
  mg_node_id_t a, b, m;
  char hm[33], cid[MG_MAINT_ID_HEX + 1];
  int64_t now = now_ms();
  int st;
  mpack_node_t r;
  init_ctx(&ctx, s, NULL);
  add_node(s, a, "deploy with make", "a", "a", 2, 0.0f, now, NULL, 0, NULL, 0);
  add_node(s, b, "deploy via make", "b", "b", 2, 0.0f, now, NULL, 0, NULL, 0);
  r = scan(&ctx, &st);
  CHECK(find_candidate(r, "near_duplicate", a, cid, NULL), "pair found");
  add_node(s, m, "deploy: make deploy", "merged", "m", 9, 0.0f, now, NULL, 0, NULL, 0);
  hex(m, hm);
  r = call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "merge", "by", hm, NULL);
  CHECK(st == 0, "merge ok");
  CHECK(node_state(s, a) == MG_NODE_SUPERSEDED && node_state(s, b) == MG_NODE_SUPERSEDED &&
        node_state(s, m) == MG_NODE_ACTIVE, "merge supersedes both");
  r = call(&ctx, "maintain_status", &st, NULL);
  CHECK(map_int(mpack_node_map_cstr_optional(r, "pending"), "total") == 0, "resolved candidate leaves the backlog");
  if (g_resp) { mpack_tree_destroy(&g_tree); free(g_resp); g_resp = NULL; }
  mg_storage_close(s);
  cleanup_db(path);
}

static void test_retire_restore_purge(void) {
  const char *path = "test_maintain_retire.db";
  mg_storage_t *s = open_db(path);
  mg_ctx_t ctx;
  mg_config_t cfg;
  mg_node_id_t a, b;
  char ha[33];
  int64_t now = now_ms();
  int st;
  mpack_node_t r;
  mg_keyword_id_t k = kw(s, "infra");
  mg_config_defaults(&cfg);
  init_ctx(&ctx, s, &cfg);

  add_node(s, a, "terraform state lives in s3", "bucket xyz", "a", 4, 0.0f, now, &k, 1, NULL, 0);
  {
    /* b links to a so neighbors() has something to hide */
    mg_node_t n;
    mg_embedding_t emb;
    mg_edge_t edge;
    memset(&n, 0, sizeof(n));
    mg_uuidv7(n.id);
    mg_blake3((const uint8_t *)"b", 1, n.content_hash);
    n.title = (char *)"terraform workspaces per env";
    n.body = (char *)"w";
    n.created_at = now;
    n.last_access = now;
    block_emb(emb, 5, 0.0f);
    memset(&edge, 0, sizeof(edge));
    memcpy(edge.src, n.id, MG_NODE_ID_BYTES);
    memcpy(edge.dst, a, MG_NODE_ID_BYTES);
    edge.kind = MG_EDGE_KEYWORD;
    edge.keyword_id = k;
    edge.weight = 0.7f;
    CHECK(mg_storage_insert_node_with_edges(s, &n, emb, &k, 1, &edge, 1, NULL) == MG_OK, "insert b");
    memcpy(b, n.id, MG_NODE_ID_BYTES);
  }
  hex(a, ha);
  CHECK(in_vector_search(s, a, 4) && in_fts(s, a, "terraform"), "visible before retire");

  r = call(&ctx, "maintain_resolve", &st, "node", ha, "action", "retire", "note", "obsolete", NULL);
  CHECK(st == 0, "retire ok");
  CHECK(node_state(s, a) == MG_NODE_RETIRED, "retired state");
  CHECK(!in_vector_search(s, a, 4), "retired leaves vector search");
  CHECK(!in_fts(s, a, "terraform state"), "retired leaves full-text search");
  {
    mg_edge_t out[8];
    int n = 0, seen = 0;
    CHECK(mg_storage_neighbors(s, b, -1, NULL, 0, out, 8, &n) == MG_OK, "neighbors ok");
    for (int i = 0; i < n; ++i) seen |= memcmp(out[i].dst, a, MG_NODE_ID_BYTES) == 0;
    CHECK(!seen, "retired leaves graph traversal");
  }
  /* get still answers by id, and says it is retired */
  r = call(&ctx, "get", &st, "id_hex", ha, NULL);
  CHECK(st == 0 && str_eq(mpack_node_map_cstr_optional(r, "state"), "retired"), "get shows retired");

  /* reversible */
  r = call(&ctx, "maintain_resolve", &st, "node", ha, "action", "restore", NULL);
  CHECK(st == 0 && node_state(s, a) == MG_NODE_ACTIVE, "restore ok");
  CHECK(in_vector_search(s, a, 4) && in_fts(s, a, "terraform"), "visible again after restore");

  /* within the retention window apply-safe keeps it */
  call(&ctx, "maintain_resolve", &st, "node", ha, "action", "retire", NULL);
  r = call(&ctx, "maintain_status", &st, NULL);
  CHECK(map_int(mpack_node_map_cstr_optional(r, "nodes"), "retired") == 1, "status counts retired");
  CHECK(map_int(mpack_node_map_cstr_optional(r, "nodes"), "retired_due") == 0, "not due yet");
  r = call(&ctx, "maintain_apply_safe", &st, NULL);
  CHECK(st == 0 && map_int(r, "purged_retired") == 0, "kept inside the window");
  CHECK(node_state(s, a) == MG_NODE_RETIRED, "still retired");

  /* retention 0: the window has elapsed, apply-safe purges physically */
  cfg.maint_retention_days = 0;
  r = call(&ctx, "maintain_status", &st, NULL);
  CHECK(map_int(mpack_node_map_cstr_optional(r, "nodes"), "retired_due") == 1, "due with retention 0");
  r = call(&ctx, "maintain_apply_safe", &st, NULL);
  CHECK(st == 0 && map_int(r, "purged_retired") == 1, "purged after the window");
  CHECK(node_state(s, a) == -1, "row gone");
  CHECK(node_state(s, b) == MG_NODE_ACTIVE, "neighbor untouched");

  /* audit log: retire, restore, retire, purge, apply_safe x2 */
  r = call(&ctx, "maintain_log", &st, "node", ha, NULL);
  {
    mpack_node_t entries = mpack_node_map_cstr_optional(r, "entries");
    size_t n = mpack_node_array_length(entries);
    CHECK(st == 0 && n == 4, "four entries name the node");
    CHECK(n > 0 && str_eq(mpack_node_map_cstr_optional(mpack_node_array_at(entries, 0), "action"), "purge"),
          "newest first");
    CHECK(n == 4 && str_eq(mpack_node_map_cstr_optional(mpack_node_array_at(entries, 3), "note"), "obsolete"),
          "note recorded");
    CHECK(n == 4 && str_eq(mpack_node_map_cstr_optional(mpack_node_array_at(entries, 3), "actor"), "agent"),
          "default actor");
  }
  r = call(&ctx, "maintain_log", &st, NULL);
  CHECK(mpack_node_array_length(mpack_node_map_cstr_optional(r, "entries")) == 6, "apply_safe runs logged");

  if (g_resp) { mpack_tree_destroy(&g_tree); free(g_resp); g_resp = NULL; }
  mg_config_free(&cfg);
  mg_storage_close(s);
  cleanup_db(path);
}

static void test_apply_safe_and_status(void) {
  const char *path = "test_maintain_safe.db";
  mg_storage_t *s = open_db(path);
  mg_ctx_t ctx;
  mg_node_id_t a, b, c;
  int64_t now = now_ms();
  int st;
  mpack_node_t r;
  mg_source_t src;
  init_ctx(&ctx, s, NULL);

  r = call(&ctx, "maintain_status", &st, NULL);
  CHECK(st == 0 && map_int(r, "last_apply_safe_at") == 0 && map_int(r, "last_scan_at") == 0, "fresh status");

  /* a and b: identical text, different hashes (keyword sets) -> exact duplicate */
  memset(&src, 0, sizeof(src));
  src.kind = (char *)"url";
  src.locator = (char *)"https://example.com/doc";
  src.observed_at = now;
  add_node(s, a, "redis runs on 6380", "see compose file", "kw1", 7, 0.0f, now - 3 * HOUR_MS, NULL, 0, NULL, 0);
  add_node(s, b, "redis runs on 6380", "see compose file", "kw2", 7, 0.0f, now - HOUR_MS, NULL, 0, &src, 1);
  add_node(s, c, "unrelated", "z", "c", 10, 0.0f, now, NULL, 0, NULL, 0);

  r = call(&ctx, "maintain_status", &st, NULL);
  CHECK(map_int(r, "inserts_since_apply_safe") == 3, "inserts since never");
  CHECK(mpack_node_array_length(mpack_node_map_cstr_optional(r, "recommended")) >= 1, "something recommended");

  r = call(&ctx, "maintain_apply_safe", &st, NULL);
  CHECK(st == 0, "apply-safe ok");
  CHECK(map_int(r, "collapsed_duplicates") == 1, "one exact duplicate collapsed");
  CHECK(map_int(r, "inserts_since_last") == 3, "reports inserts since last run");
  CHECK(map_int(mpack_node_map_cstr_optional(r, "consolidate"), "expired_deleted") == 0, "consolidate ran");
  CHECK(node_state(s, a) == MG_NODE_ACTIVE && node_state(s, b) == MG_NODE_SUPERSEDED, "oldest kept");
  {
    mg_source_link_t *links = NULL;
    size_t n = 0;
    CHECK(mg_storage_source_links(s, NULL, NULL, (const mg_node_id_t *)&a, &links, &n) == MG_OK && n == 1,
          "keeper inherits provenance");
    mg_source_links_free(links, n);
  }
  /* idempotent */
  r = call(&ctx, "maintain_apply_safe", &st, NULL);
  CHECK(map_int(r, "collapsed_duplicates") == 0, "second run collapses nothing");

  r = call(&ctx, "maintain_status", &st, NULL);
  CHECK(map_int(r, "last_apply_safe_at") > 0, "last run recorded");
  CHECK(map_int(r, "inserts_since_apply_safe") == 0, "counter reset");
  CHECK(map_int(mpack_node_map_cstr_optional(r, "nodes"), "superseded") == 1, "state counts");
  {
    mg_node_id_t d;
    add_node(s, d, "new fact", "n", "d", 11, 0.0f, now + 2000, NULL, 0, NULL, 0);
    r = call(&ctx, "maintain_status", &st, NULL);
    CHECK(map_int(r, "inserts_since_apply_safe") == 1, "new insert counted");
  }
  /* restore undoes the collapse */
  {
    char hb[33];
    hex(b, hb);
    call(&ctx, "maintain_resolve", &st, "node", hb, "action", "restore", NULL);
    CHECK(st == 0 && node_state(s, b) == MG_NODE_ACTIVE, "collapse is reversible");
  }

  if (g_resp) { mpack_tree_destroy(&g_tree); free(g_resp); g_resp = NULL; }
  mg_storage_close(s);
  cleanup_db(path);
}

static void test_low_value_and_keywords(void) {
  const char *path = "test_maintain_kw.db";
  mg_storage_t *s = open_db(path);
  mg_ctx_t ctx;
  mg_node_id_t old, x, y, z;
  char cid[MG_MAINT_ID_HEX + 1];
  int64_t now = now_ms();
  int st;
  mpack_node_t r, cand;
  mg_keyword_id_t k1 = kw(s, "jwt-token"), k2 = kw(s, "JWT_Tokens");
  init_ctx(&ctx, s, NULL);

  add_node(s, old, "forgotten note", "o", "o", 13, 0.0f, now - 90 * DAY_MS, NULL, 0, NULL, 0);
  add_node(s, x, "token ttl", "x", "x", 0, 0.0f, now, &k1, 1, NULL, 0);
  add_node(s, y, "token signing", "y", "y", 14, 0.0f, now, &k1, 1, NULL, 0);
  add_node(s, z, "token refresh", "z", "z", 15, 0.0f, now, &k2, 1, NULL, 0);

  r = scan(&ctx, &st);
  CHECK(find_candidate(r, "isolated_low_value", old, cid, NULL), "old isolated node");
  CHECK(!find_candidate(r, "isolated_low_value", x, NULL, NULL), "recent nodes are not low value");
  CHECK(find_candidate(r, "keyword_fragmentation", z, NULL, &cand), "fragment keyword on z");
  {
    mpack_node_t sig = mpack_node_map_cstr_optional(cand, "signals");
    CHECK(str_eq(mpack_node_map_cstr_optional(sig, "keyword"), "JWT_Tokens") &&
          str_eq(mpack_node_map_cstr_optional(sig, "similar_keyword"), "jwt-token"), "keyword pair");
  }
  CHECK(!find_candidate(r, "keyword_fragmentation", x, NULL, NULL), "established spelling is fine");

  /* retire through the candidate (single node: no --node needed) */
  r = call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "retire", NULL);
  CHECK(st == 0 && node_state(s, old) == MG_NODE_RETIRED, "retire via candidate");
  r = scan(&ctx, &st);
  CHECK(!find_candidate(r, "isolated_low_value", old, NULL, NULL), "retired node leaves the scan");

  if (g_resp) { mpack_tree_destroy(&g_tree); free(g_resp); g_resp = NULL; }
  mg_storage_close(s);
  cleanup_db(path);
}

static void write_file(const char *path, const char *content) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "cannot write %s\n", path);
    exit(1);
  }
  fputs(content, fp);
  fclose(fp);
}

/* sources_list for project, re-hashed under root like `maintain scan`, then
 * maintain_scan with that evidence */
static mpack_node_t scan_with_sources(mg_ctx_t *ctx, const char *root, const char *project, int *st) {
  char *args = NULL;
  size_t len = 0;
  mpack_writer_t w;
  mpack_node_t list = call(ctx, "sources_list", st, "project", project, "kind", "file", NULL);
  mpack_node_t r;
  /* the list result stays valid until the next dispatch */
  mpack_writer_init_growable(&w, &args, &len);
  mpack_build_map(&w);
  mpack_write_cstr(&w, "project");
  mpack_write_cstr(&w, project);
  mpack_write_cstr(&w, "sources");
  (void)mg_sources_write_stale_links(list, root, &w);
  mpack_complete_map(&w);
  mpack_writer_destroy(&w);
  r = dispatch(ctx, "maintain_scan", args, len, st);
  free(args);
  return r;
}

static void test_sources(void) {
  const char *path = "test_maintain_src.db";
  mg_storage_t *s = open_db(path);
  mg_ctx_t ctx;
  mg_node_id_t a, b, c;
  char cid[MG_MAINT_ID_HEX + 1], root[4096], project[4096], err[512];
  int64_t now = now_ms();
  int st;
  mpack_node_t r;
  mg_source_spec_t sa, sb;
  init_ctx(&ctx, s, NULL);

  mk_dir("maint_repo");
  mk_dir("maint_repo/.git");
  write_file("maint_repo/a.md", "alpha");
  write_file("maint_repo/b.md", "beta");
  if (mg_source_parse("file:maint_repo/a.md", 1, &sa, err, sizeof(err)) != 0 ||
      mg_source_parse("file:maint_repo/b.md", 1, &sb, err, sizeof(err)) != 0 ||
      mg_source_resolve_project("maint_repo", root, sizeof(root), project, sizeof(project)) != 0) {
    fprintf(stderr, "source setup: %s\n", err);
    exit(1);
  }
  {
    mg_source_t la, lb;
    memset(&la, 0, sizeof(la));
    la.project = sa.project; la.kind = sa.kind; la.locator = sa.locator;
    la.fingerprint = sa.fingerprint; la.observed_at = now;
    lb = la;
    lb.locator = sb.locator; lb.fingerprint = sb.fingerprint;
    add_node(s, a, "alpha doc", "a", "a", 1, 0.0f, now, NULL, 0, &la, 1);
    add_node(s, b, "beta doc", "b", "b", 2, 0.0f, now, NULL, 0, &lb, 1);
    add_node(s, c, "no sources", "c", "c", 3, 0.0f, now, NULL, 0, NULL, 0);
  }

  r = scan_with_sources(&ctx, root, project, &st);
  CHECK(st == 0, "scan with sources ok");
  CHECK(mpack_node_type(mpack_node_map_cstr_optional(mpack_node_map_cstr_optional(r, "summary"),
                                                      "sources_checked")) == mpack_type_bool,
        "sources_checked reported");
  CHECK(!find_candidate(r, "source_changed", NULL, NULL, NULL), "nothing changed yet");

  write_file("maint_repo/a.md", "alpha v2");
  remove("maint_repo/b.md");
  r = scan_with_sources(&ctx, root, project, &st);
  CHECK(find_candidate(r, "source_changed", a, cid, NULL), "a changed");
  CHECK(find_candidate(r, "source_removed", b, NULL, NULL), "b removed");
  CHECK(!find_candidate(r, "source_changed", c, NULL, NULL), "no sources, no candidate");

  /* refresh records the observed fingerprint: the candidate converges */
  r = call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "refresh", NULL);
  CHECK(st == 0 && map_int(r, "refreshed") == 1, "refresh updated the link");
  r = scan_with_sources(&ctx, root, project, &st);
  CHECK(!find_candidate(r, "source_changed", a, NULL, NULL), "refreshed node is current");

  /* keep on a changed source hides it until the file changes again */
  write_file("maint_repo/a.md", "alpha v3");
  r = scan_with_sources(&ctx, root, project, &st);
  CHECK(find_candidate(r, "source_changed", a, cid, NULL), "a changed again");
  call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "keep", NULL);
  r = scan_with_sources(&ctx, root, project, &st);
  CHECK(!find_candidate(r, "source_changed", a, NULL, NULL), "kept while the file is the same");
  write_file("maint_repo/a.md", "alpha v4");
  r = scan_with_sources(&ctx, root, project, &st);
  CHECK(find_candidate(r, "source_changed", a, NULL, NULL), "new fingerprint brings it back");

  /* removed source -> stale through the candidate */
  CHECK(find_candidate(r, "source_removed", b, cid, NULL), "b still removed");
  call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "stale", NULL);
  CHECK(st == 0 && node_state(s, b) == MG_NODE_STALE, "b stale");
  r = scan_with_sources(&ctx, root, project, &st);
  CHECK(!find_candidate(r, "source_removed", b, NULL, NULL), "stale node leaves the scan");

  /* refresh is only for source_changed candidates */
  CHECK(find_candidate(r, "source_changed", a, cid, NULL), "a pending");
  {
    char hc[33];
    hex(c, hc);
    call(&ctx, "maintain_resolve", &st, "candidate_id", cid, "action", "retire", "node", hc, NULL);
    CHECK(st != 0, "a node outside the candidate is refused");
  }

  mg_source_spec_free(&sa);
  mg_source_spec_free(&sb);
  remove("maint_repo/a.md");
  if (g_resp) { mpack_tree_destroy(&g_tree); free(g_resp); g_resp = NULL; }
  mg_storage_close(s);
  cleanup_db(path);
}

int main(void) {
  test_pairs_and_resolve();
  test_merge_action();
  test_retire_restore_purge();
  test_apply_safe_and_status();
  test_low_value_and_keywords();
  test_sources();
  if (g_fail) {
    fprintf(stderr, "test_maintain: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("test_maintain: ok\n");
  return 0;
}
