/* Maintenance ops (issue #5): the daemon half of `graft maintain`.
 *
 * maintain_scan args (all optional):
 *   { "limit": int,                      candidates returned (default 20)
 *     "project": str,                    project the source evidence is for
 *     "sources": [ { "id_hex", "locator", "state": "changed"|"removed",
 *                    "recorded_fingerprint"|nil, "fingerprint"|nil } ] }
 * The CLI re-hashes the project's files (like `sources diff`) and sends the
 * links that no longer match; the daemon never reads source files. Nothing in
 * the graph changes: the scan replaces the candidate cache (what status
 * counts and resolve looks candidates up in) and returns
 *   { "summary": { "pending", "by_kind", "dismissed", "truncated",
 *                  "scanned_nodes", "sources_checked" },
 *     "candidates": [ { "id", "kind", "score", "nodes": [ {...} ],
 *                       "signals": {...}, "suggested_actions": [...] } ] }
 * Candidate ids are deterministic (kind + node ids), so the same finding
 * keeps its id across scans; a `keep` dismissal is stored with an evidence
 * digest (content hashes, current source fingerprints) and hides the
 * candidate until that evidence changes.
 *
 * maintain_resolve args:
 *   { "action": str, "candidate_id"?: str, "node"?: hex, "by"?: hex,
 *     "note"?: str, "actor"?: str }
 * maintain_apply_safe args: { "actor"?: str }
 * maintain_log args: { "limit"?: int, "node"?: hex }
 * maintain_status args: none
 */

#include "graft/maintain.h"
#include "graft/ops.h"
#include "graft/error.h"
#include "../retrieve/internal.h"
#include "mpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAINT_DEFAULT_LIMIT     20
#define MAINT_MAX_KW            32
#define MAINT_MAX_SOURCES       4096
#define MAINT_SUPERSESSION_GAP  (3600LL * 1000LL)   /* 1 h between the two nodes */
#define MAINT_DAY_MS            (86400LL * 1000LL)
#define HEX_ID_LEN              (2 * MG_NODE_ID_BYTES)

typedef struct {
  int    kind;
  double score;
  char   id[MG_MAINT_ID_HEX + 1];
  char   evidence[MG_MAINT_ID_HEX + 1];
  char   nodes[2 * (HEX_ID_LEN + 1)];
  char  *payload;
  size_t payload_len;
} cand_t;

typedef struct {
  cand_t *v;
  size_t  n;
  size_t  cap;
  int64_t dismissed;
} cand_list_t;

/* One source link of the evidence the CLI sent. */
typedef struct {
  mg_node_id_t node;
  char         hex[HEX_ID_LEN + 1];
  char        *locator;
  char        *state;
  char        *recorded;
  char        *current;
} src_ev_t;

static int64_t now_ms(void) {
  return (int64_t)time(NULL) * 1000;
}

static void digest16(const char *text, char out[MG_MAINT_ID_HEX + 1]) {
  mg_hash_t h;
  mg_blake3((const uint8_t *)text, strlen(text), h);
  mg_retrieve_hex_encode(h, MG_MAINT_ID_HEX / 2, out);
}

static void hex_of(const mg_node_id_t id, char out[HEX_ID_LEN + 1]) {
  mg_retrieve_hex_encode(id, MG_NODE_ID_BYTES, out);
}

static const char *state_name(int s) {
  switch (s) {
    case MG_NODE_ACTIVE:     return "active";
    case MG_NODE_STALE:      return "stale";
    case MG_NODE_SUPERSEDED: return "superseded";
    case MG_NODE_RETIRED:    return "retired";
    default:                 return "unknown";
  }
}

static const mg_config_t *cfg_or_defaults(const mg_ctx_t *ctx, mg_config_t *fallback) {
  if (ctx->config) return ctx->config;
  mg_config_defaults(fallback);
  return fallback;
}

static int opt_int(mpack_node_t args, const char *key, int64_t *out) {
  mpack_node_t n;
  if (mpack_node_is_missing(args) || mpack_node_is_nil(args)) return 0;
  n = mpack_node_map_cstr_optional(args, key);
  if (mpack_node_is_missing(n) || mpack_node_is_nil(n)) return 0;
  if (mpack_node_type(n) != mpack_type_int && mpack_node_type(n) != mpack_type_uint) return -1;
  *out = mpack_node_i64(n);
  return 1;
}

/* malloc'd copy of a string field, NULL when absent; *bad set on a non-string. */
static char *opt_str(mpack_node_t args, const char *key, int *bad) {
  mpack_node_t n;
  if (mpack_node_is_missing(args) || mpack_node_is_nil(args)) return NULL;
  n = mpack_node_map_cstr_optional(args, key);
  if (mpack_node_is_missing(n) || mpack_node_is_nil(n)) return NULL;
  if (mpack_node_type(n) != mpack_type_str) {
    *bad = 1;
    return NULL;
  }
  return mg_retrieve_node_str_dup(n);
}

static int decode_id(const char *hex, mg_node_id_t out) {
  return hex && strlen(hex) == HEX_ID_LEN &&
         mg_retrieve_hex_decode(hex, HEX_ID_LEN, out, MG_NODE_ID_BYTES) == 0;
}

/* --------------------------------------------------------------- writers */

static void write_node(mpack_writer_t *w, mg_storage_t *s, const mg_node_id_t id) {
  mg_node_t node;
  char hex[HEX_ID_LEN + 1];
  hex_of(id, hex);
  mpack_build_map(w);
  mpack_write_cstr(w, "id_hex"); mpack_write_cstr(w, hex);
  if (mg_storage_get_node(s, id, &node) == MG_OK) {
    mpack_write_cstr(w, "title");        mpack_write_cstr(w, node.title);
    mpack_write_cstr(w, "state");        mpack_write_cstr(w, state_name(node.state));
    mpack_write_cstr(w, "created_at");   mpack_write_int(w, node.created_at);
    mpack_write_cstr(w, "access_count"); mpack_write_int(w, node.access_count);
    mg_node_free(&node);
  }
  mpack_complete_map(w);
}

static void write_actions(mpack_writer_t *w, const char *const *actions) {
  mpack_write_cstr(w, "suggested_actions");
  mpack_build_array(w);
  for (; *actions; ++actions) mpack_write_cstr(w, *actions);
  mpack_complete_array(w);
}

/* Rounds to 4 decimals so the printed JSON stays readable. */
static void write_score(mpack_writer_t *w, double v) {
  mpack_write_double(w, (double)(long long)(v * 10000.0 + (v >= 0 ? 0.5 : -0.5)) / 10000.0);
}

/* ------------------------------------------------------------ candidates */

static mg_err_t cand_push(cand_list_t *l, mg_storage_t *s, int kind, double score,
                          const char *id_input, const char *evidence_input,
                          const char *nodes_csv, char *payload, size_t payload_len) {
  cand_t *c;
  int dismissed = 0;
  char id[MG_MAINT_ID_HEX + 1], ev[MG_MAINT_ID_HEX + 1];
  digest16(id_input, id);
  digest16(evidence_input, ev);
  if (mg_storage_maint_is_dismissed(s, id, ev, &dismissed) == MG_OK && dismissed) {
    l->dismissed++;
    free(payload);
    return MG_OK;
  }
  if (l->n == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 32;
    cand_t *nv = (cand_t *)realloc(l->v, nc * sizeof(*nv));
    if (!nv) {
      free(payload);
      return MG_ERR_OOM;
    }
    l->v = nv;
    l->cap = nc;
  }
  c = &l->v[l->n++];
  memset(c, 0, sizeof(*c));
  c->kind = kind;
  c->score = score;
  memcpy(c->id, id, sizeof(id));
  memcpy(c->evidence, ev, sizeof(ev));
  snprintf(c->nodes, sizeof(c->nodes), "%s", nodes_csv);
  c->payload = payload;
  c->payload_len = payload_len;
  return MG_OK;
}

static void cand_list_free(cand_list_t *l) {
  size_t i;
  for (i = 0; i < l->n; ++i) free(l->v[i].payload);
  free(l->v);
  memset(l, 0, sizeof(*l));
}

/* Opens a candidate map: id, kind, score, nodes. The caller adds signals
 * and suggested_actions, then calls cand_finish. */
static void cand_begin(mpack_writer_t *w, char **buf, size_t *len, mg_storage_t *s,
                       const char *id_input, int kind, double score,
                       const mg_node_id_t *nodes, size_t n_nodes) {
  char id[MG_MAINT_ID_HEX + 1];
  size_t i;
  digest16(id_input, id);
  *buf = NULL;
  *len = 0;
  mpack_writer_init_growable(w, buf, len);
  mpack_build_map(w);
  mpack_write_cstr(w, "id");    mpack_write_cstr(w, id);
  mpack_write_cstr(w, "kind");  mpack_write_cstr(w, mg_maint_kind_name((mg_maint_kind_t)kind));
  mpack_write_cstr(w, "score"); write_score(w, score);
  mpack_write_cstr(w, "nodes");
  mpack_build_array(w);
  for (i = 0; i < n_nodes; ++i) write_node(w, s, nodes[i]);
  mpack_complete_array(w);
}

static mg_err_t cand_finish(mpack_writer_t *w, char **buf, size_t *len) {
  mpack_complete_map(w);
  if (mpack_writer_destroy(w) != mpack_ok) {
    free(*buf);
    *buf = NULL;
    return MG_ERR_OOM;
  }
  return MG_OK;
}

static void nodes_csv(const mg_node_id_t *ids, size_t n, char *out, size_t cap) {
  size_t i, off = 0;
  out[0] = '\0';
  for (i = 0; i < n && off + HEX_ID_LEN + 2 <= cap; ++i) {
    if (i) out[off++] = ',';
    hex_of(ids[i], out + off);
    off += HEX_ID_LEN;
  }
}

/* id input: kind|ids|extra; evidence input: content hashes + extra */
static void id_input(int kind, const char *csv, const char *extra, char *out, size_t cap) {
  snprintf(out, cap, "%s|%s|%s", mg_maint_kind_name((mg_maint_kind_t)kind), csv, extra ? extra : "");
}

static void evidence_input(mg_storage_t *s, const mg_node_id_t *ids, size_t n, const char *extra,
                           char *out, size_t cap) {
  size_t i, off = 0;
  out[0] = '\0';
  for (i = 0; i < n; ++i) {
    mg_node_t node;
    char h[2 * MG_HASH_BYTES + 1];
    if (mg_storage_get_node(s, ids[i], &node) != MG_OK) continue;
    mg_retrieve_hex_encode(node.content_hash, MG_HASH_BYTES, h);
    mg_node_free(&node);
    off += (size_t)snprintf(out + off, off < cap ? cap - off : 0, "%s;", h);
    if (off >= cap) return;
  }
  if (extra) snprintf(out + off, off < cap ? cap - off : 0, "%s", extra);
}

/* Shared keywords of two nodes, written as an array of texts. */
static size_t write_shared_keywords(mpack_writer_t *w, mg_storage_t *s,
                                    const mg_node_id_t a, const mg_node_id_t b) {
  mg_keyword_id_t ka[MAINT_MAX_KW], kb[MAINT_MAX_KW];
  int na = 0, nb = 0, i, j;
  size_t shared = 0;
  (void)mg_storage_node_keywords(s, a, ka, MAINT_MAX_KW, &na);
  (void)mg_storage_node_keywords(s, b, kb, MAINT_MAX_KW, &nb);
  mpack_build_array(w);
  for (i = 0; i < na; ++i) {
    for (j = 0; j < nb; ++j) {
      char *text = NULL;
      if (ka[i] != kb[j]) continue;
      if (mg_storage_get_keyword_text(s, ka[i], &text) == MG_OK && text) {
        mpack_write_cstr(w, text);
        free(text);
        shared++;
      }
      break;
    }
  }
  mpack_complete_array(w);
  return shared;
}

static size_t count_shared_keywords(mg_storage_t *s, const mg_node_id_t a, const mg_node_id_t b) {
  mg_keyword_id_t ka[MAINT_MAX_KW], kb[MAINT_MAX_KW];
  int na = 0, nb = 0, i, j;
  size_t shared = 0;
  (void)mg_storage_node_keywords(s, a, ka, MAINT_MAX_KW, &na);
  (void)mg_storage_node_keywords(s, b, kb, MAINT_MAX_KW, &nb);
  for (i = 0; i < na; ++i) {
    for (j = 0; j < nb; ++j) {
      if (ka[i] == kb[j]) {
        shared++;
        break;
      }
    }
  }
  return shared;
}

static int has_source_evidence(const src_ev_t *ev, size_t n, const mg_node_id_t id) {
  size_t i;
  for (i = 0; i < n; ++i) {
    if (memcmp(ev[i].node, id, MG_NODE_ID_BYTES) == 0) return 1;
  }
  return 0;
}

static int64_t node_created(mg_storage_t *s, const mg_node_id_t id) {
  mg_node_t node;
  int64_t t = 0;
  if (mg_storage_get_node(s, id, &node) == MG_OK) {
    t = node.created_at;
    mg_node_free(&node);
  }
  return t;
}

/* A pair candidate (near_duplicate / possible_supersession / contradiction),
 * nodes ordered older first so "a" is the one a newer note would replace. */
static mg_err_t pair_candidate(cand_list_t *l, mg_storage_t *s, int kind, double score,
                               const mg_node_id_t x, const mg_node_id_t y,
                               const src_ev_t *ev, size_t n_ev) {
  static const char *const dup_actions[] = { "merge", "supersede_a", "supersede_b", "keep_both", NULL };
  static const char *const sup_actions[] = { "supersede_a", "supersede_b", "keep_both", "merge", NULL };
  static const char *const con_actions[] = { "supersede_a", "supersede_b", "stale", "keep_both", NULL };
  mg_node_id_t ids[2];
  int64_t ta = node_created(s, x), tb = node_created(s, y);
  char csv[2 * (HEX_ID_LEN + 1)], idin[256], evin[512];
  mpack_writer_t w;
  char *buf;
  size_t len;
  mg_err_t err;
  int swap = ta > tb || (ta == tb && memcmp(x, y, MG_NODE_ID_BYTES) > 0);
  memcpy(ids[0], swap ? y : x, MG_NODE_ID_BYTES);
  memcpy(ids[1], swap ? x : y, MG_NODE_ID_BYTES);
  if (swap) {
    int64_t t = ta;
    ta = tb;
    tb = t;
  }
  if (kind == MG_MAINT_NEAR_DUPLICATE && tb - ta >= MAINT_SUPERSESSION_GAP &&
      count_shared_keywords(s, ids[0], ids[1]) > 0) {
    kind = MG_MAINT_POSSIBLE_SUPERSESSION;
  }
  nodes_csv((const mg_node_id_t *)ids, 2, csv, sizeof(csv));
  id_input(kind, csv, NULL, idin, sizeof(idin));
  evidence_input(s, (const mg_node_id_t *)ids, 2, NULL, evin, sizeof(evin));

  cand_begin(&w, &buf, &len, s, idin, kind, score, (const mg_node_id_t *)ids, 2);
  mpack_write_cstr(&w, "signals");
  mpack_build_map(&w);
  mpack_write_cstr(&w, kind == MG_MAINT_CONTRADICTION ? "edge_weight" : "semantic_similarity");
  write_score(&w, score);
  mpack_write_cstr(&w, "shared_keywords");
  (void)write_shared_keywords(&w, s, ids[0], ids[1]);
  mpack_write_cstr(&w, "newer");          mpack_write_cstr(&w, tb > ta ? "b" : "same_time");
  mpack_write_cstr(&w, "age_gap_hours");  mpack_write_int(&w, (tb - ta) / 3600000LL);
  mpack_write_cstr(&w, "source_changed");
  mpack_write_bool(&w, has_source_evidence(ev, n_ev, ids[0]) || has_source_evidence(ev, n_ev, ids[1]));
  mpack_complete_map(&w);
  write_actions(&w, kind == MG_MAINT_CONTRADICTION ? con_actions
                  : kind == MG_MAINT_POSSIBLE_SUPERSESSION ? sup_actions : dup_actions);
  err = cand_finish(&w, &buf, &len);
  if (err != MG_OK) return err;
  return cand_push(l, s, kind, score, idin, evin, csv, buf, len);
}

/* Near-duplicates among the newest ACTIVE nodes: pairwise cosine over the
 * loaded (normalized) vectors, at most `per_node` partners per node. */
static mg_err_t scan_near_duplicates(cand_list_t *l, mg_storage_t *s, const mg_config_t *cfg,
                                     const src_ev_t *ev, size_t n_ev, size_t *scanned) {
  mg_maint_vec_t *v = NULL;
  size_t n = 0, i, j;
  int *partners;
  int per_node = cfg->maint_scan_neighbors > 0 ? cfg->maint_scan_neighbors : 5;
  float thr = cfg->maint_near_duplicate_min;
  mg_err_t err = mg_storage_maint_active_vectors(
      s, cfg->maint_scan_max_nodes > 0 ? (size_t)cfg->maint_scan_max_nodes : 2000, &v, &n);
  *scanned = n;
  if (err != MG_OK) return err;
  partners = (int *)calloc(n ? n : 1, sizeof(*partners));
  if (!partners) {
    free(v);
    return MG_ERR_OOM;
  }
  for (i = 0; err == MG_OK && i < n; ++i) {
    for (j = i + 1; err == MG_OK && j < n && partners[i] < per_node; ++j) {
      double dot = 0.0;
      size_t d;
      if (partners[j] >= per_node) continue;
      for (d = 0; d < MG_EMBEDDING_DIM; ++d) dot += (double)v[i].emb[d] * v[j].emb[d];
      if (dot < thr) continue;
      partners[i]++;
      partners[j]++;
      err = pair_candidate(l, s, MG_MAINT_NEAR_DUPLICATE, dot, v[i].id, v[j].id, ev, n_ev);
    }
  }
  free(partners);
  free(v);
  return err;
}

static mg_err_t scan_contradictions(cand_list_t *l, mg_storage_t *s, size_t cap,
                                    const src_ev_t *ev, size_t n_ev) {
  mg_maint_pair_t *p = NULL;
  size_t n = 0, i;
  mg_err_t err = mg_storage_maint_contradictions(s, cap, &p, &n);
  for (i = 0; err == MG_OK && i < n; ++i) {
    err = pair_candidate(l, s, MG_MAINT_CONTRADICTION, p[i].weight, p[i].a, p[i].b, ev, n_ev);
  }
  free(p);
  return err;
}

static int cmp_ev(const void *a, const void *b) {
  const src_ev_t *x = (const src_ev_t *)a, *y = (const src_ev_t *)b;
  int c = strcmp(x->hex, y->hex);
  return c ? c : strcmp(x->locator, y->locator);
}

/* One candidate per ACTIVE node whose file sources changed or vanished. */
static mg_err_t scan_sources(cand_list_t *l, mg_storage_t *s, const char *project,
                             src_ev_t *ev, size_t n_ev) {
  static const char *const changed_actions[] = { "refresh", "stale", "supersede", "retire", NULL };
  static const char *const removed_actions[] = { "retire", "stale", "supersede", "keep", NULL };
  size_t i = 0, j, k;
  mg_err_t err = MG_OK;
  qsort(ev, n_ev, sizeof(*ev), cmp_ev);
  while (err == MG_OK && i < n_ev) {
    mg_node_t node;
    int removed = 0, kind;
    char idin[256], evin[2048], *buf;
    size_t len, off = 0;
    mpack_writer_t w;
    for (j = i; j < n_ev && !strcmp(ev[j].hex, ev[i].hex); ++j) {
      if (!strcmp(ev[j].state, "removed")) removed = 1;
    }
    if (mg_storage_get_node(s, ev[i].node, &node) != MG_OK) {
      i = j;
      continue;
    }
    if (node.state != MG_NODE_ACTIVE) {
      mg_node_free(&node);
      i = j;
      continue;
    }
    kind = removed ? MG_MAINT_SOURCE_REMOVED : MG_MAINT_SOURCE_CHANGED;
    id_input(kind, ev[i].hex, NULL, idin, sizeof(idin));
    evin[0] = '\0';
    for (k = i; k < j && off < sizeof(evin); ++k) {
      off += (size_t)snprintf(evin + off, sizeof(evin) - off, "%s=%s:%s;", ev[k].locator,
                              ev[k].state, ev[k].current ? ev[k].current : "-");
    }
    mg_node_free(&node);

    cand_begin(&w, &buf, &len, s, idin, kind, (double)(j - i),
               (const mg_node_id_t *)&ev[i].node, 1);
    mpack_write_cstr(&w, "signals");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "project"); mpack_write_cstr(&w, project ? project : "");
    mpack_write_cstr(&w, "sources");
    mpack_build_array(&w);
    for (k = i; k < j; ++k) {
      mpack_build_map(&w);
      mpack_write_cstr(&w, "locator"); mpack_write_cstr(&w, ev[k].locator);
      mpack_write_cstr(&w, "state");   mpack_write_cstr(&w, ev[k].state);
      mpack_write_cstr(&w, "recorded_fingerprint");
      if (ev[k].recorded) mpack_write_cstr(&w, ev[k].recorded); else mpack_write_nil(&w);
      mpack_write_cstr(&w, "current_fingerprint");
      if (ev[k].current) mpack_write_cstr(&w, ev[k].current); else mpack_write_nil(&w);
      mpack_complete_map(&w);
    }
    mpack_complete_array(&w);
    mpack_complete_map(&w);
    write_actions(&w, removed ? removed_actions : changed_actions);
    err = cand_finish(&w, &buf, &len);
    if (err == MG_OK) {
      err = cand_push(l, s, kind, (double)(j - i), idin, evin, ev[i].hex, buf, len);
    }
    i = j;
  }
  return err;
}

static mg_err_t scan_isolated(cand_list_t *l, mg_storage_t *s, const mg_config_t *cfg,
                              size_t cap, int64_t now) {
  static const char *const actions[] = { "keep", "retire", "stale", NULL };
  int64_t min_age = (int64_t)(cfg->maint_isolated_min_age_days > 0 ? cfg->maint_isolated_min_age_days : 30)
                  * MAINT_DAY_MS;
  mg_node_id_t *ids = NULL;
  size_t n = 0, i;
  mg_err_t err = mg_storage_maint_isolated(s, now - min_age, cap, &ids, &n);
  for (i = 0; err == MG_OK && i < n; ++i) {
    char csv[HEX_ID_LEN + 1], idin[256], evin[512], *buf;
    size_t len;
    int64_t age_days = (now - node_created(s, ids[i])) / MAINT_DAY_MS;
    mpack_writer_t w;
    hex_of(ids[i], csv);
    id_input(MG_MAINT_ISOLATED_LOW_VALUE, csv, NULL, idin, sizeof(idin));
    evidence_input(s, (const mg_node_id_t *)&ids[i], 1, NULL, evin, sizeof(evin));
    cand_begin(&w, &buf, &len, s, idin, MG_MAINT_ISOLATED_LOW_VALUE, (double)age_days,
               (const mg_node_id_t *)&ids[i], 1);
    mpack_write_cstr(&w, "signals");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "age_days");     mpack_write_int(&w, age_days);
    mpack_write_cstr(&w, "access_count"); mpack_write_int(&w, 0);
    mpack_write_cstr(&w, "edges");        mpack_write_int(&w, 0);
    mpack_complete_map(&w);
    write_actions(&w, actions);
    err = cand_finish(&w, &buf, &len);
    if (err == MG_OK) {
      err = cand_push(l, s, MG_MAINT_ISOLATED_LOW_VALUE, (double)age_days, idin, evin, csv, buf, len);
    }
  }
  free(ids);
  return err;
}

/* Keyword spelling key: ASCII lowercased, separators dropped, a plural 's'
 * stripped ("JWT-Tokens" and "jwt_token" share "jwttoken"). */
static void keyword_key(const char *text, char *out, size_t cap) {
  size_t n = 0;
  for (; *text && n + 1 < cap; ++text) {
    unsigned char c = (unsigned char)*text;
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
    if (c < 0x80 && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) continue;
    out[n++] = (char)c;
  }
  if (n > 3 && out[n - 1] == 's') n--;
  out[n] = '\0';
}

typedef struct {
  char key[128];
  size_t idx;
} kw_key_t;

static int cmp_kw_key(const void *a, const void *b) {
  const kw_key_t *x = (const kw_key_t *)a, *y = (const kw_key_t *)b;
  int c = strcmp(x->key, y->key);
  return c ? c : (x->idx < y->idx ? -1 : x->idx > y->idx);
}

/* A keyword carried by a single ACTIVE node while a spelling variant of it
 * is in use: the node probably wants the established spelling. */
static mg_err_t scan_keywords(cand_list_t *l, mg_storage_t *s, size_t cap) {
  static const char *const actions[] = { "keep", "supersede", NULL };
  mg_maint_keyword_t *kw = NULL;
  kw_key_t *keys;
  size_t n = 0, i = 0, j, k, emitted = 0;
  mg_err_t err = mg_storage_maint_keyword_usage(s, &kw, &n);
  if (err != MG_OK) return err;
  keys = (kw_key_t *)calloc(n ? n : 1, sizeof(*keys));
  if (!keys) {
    mg_maint_keywords_free(kw, n);
    return MG_ERR_OOM;
  }
  for (i = 0; i < n; ++i) {
    keyword_key(kw[i].text, keys[i].key, sizeof(keys[i].key));
    keys[i].idx = i;
  }
  qsort(keys, n, sizeof(*keys), cmp_kw_key);
  i = 0;
  while (err == MG_OK && i < n && emitted < cap) {
    size_t best;
    for (j = i; j < n && !strcmp(keys[j].key, keys[i].key); ++j) {}
    best = keys[i].idx;
    for (k = i; k < j; ++k) {
      const mg_maint_keyword_t *c = &kw[keys[k].idx];
      if (c->uses > kw[best].uses || (c->uses == kw[best].uses && c->id < kw[best].id)) {
        best = keys[k].idx;
      }
    }
    for (k = i; err == MG_OK && keys[i].key[0] && k < j && emitted < cap; ++k) {
      const mg_maint_keyword_t *frag = &kw[keys[k].idx];
      mg_node_id_t node;
      size_t got = 0;
      char csv[HEX_ID_LEN + 1], idin[512], evin[512], extra[256], *buf;
      size_t len;
      mpack_writer_t w;
      if (keys[k].idx == best || frag->uses != 1) continue;
      if (mg_storage_maint_keyword_nodes(s, frag->id, &node, 1, &got) != MG_OK || got != 1) continue;
      hex_of(node, csv);
      snprintf(extra, sizeof(extra), "%s>%s", frag->text, kw[best].text);
      id_input(MG_MAINT_KEYWORD_FRAGMENTATION, csv, frag->text, idin, sizeof(idin));
      evidence_input(s, (const mg_node_id_t *)&node, 1, extra, evin, sizeof(evin));
      cand_begin(&w, &buf, &len, s, idin, MG_MAINT_KEYWORD_FRAGMENTATION,
                 (double)kw[best].uses, (const mg_node_id_t *)&node, 1);
      mpack_write_cstr(&w, "signals");
      mpack_build_map(&w);
      mpack_write_cstr(&w, "keyword");              mpack_write_cstr(&w, frag->text);
      mpack_write_cstr(&w, "similar_keyword");      mpack_write_cstr(&w, kw[best].text);
      mpack_write_cstr(&w, "keyword_uses");         mpack_write_int(&w, frag->uses);
      mpack_write_cstr(&w, "similar_keyword_uses"); mpack_write_int(&w, kw[best].uses);
      mpack_complete_map(&w);
      write_actions(&w, actions);
      err = cand_finish(&w, &buf, &len);
      if (err == MG_OK) {
        err = cand_push(l, s, MG_MAINT_KEYWORD_FRAGMENTATION, (double)kw[best].uses, idin, evin,
                        csv, buf, len);
        emitted++;
      }
    }
    i = j;
  }
  free(keys);
  mg_maint_keywords_free(kw, n);
  return err;
}

static int cmp_cand(const void *a, const void *b) {
  const cand_t *x = (const cand_t *)a, *y = (const cand_t *)b;
  if (x->kind != y->kind) return x->kind - y->kind;
  if (x->score != y->score) return x->score > y->score ? -1 : 1;
  return strcmp(x->id, y->id);
}

static void src_ev_free(src_ev_t *ev, size_t n) {
  size_t i;
  if (!ev) return;
  for (i = 0; i < n; ++i) {
    free(ev[i].locator);
    free(ev[i].state);
    free(ev[i].recorded);
    free(ev[i].current);
  }
  free(ev);
}

/* Parses args.sources; entries naming no valid node or an unknown state are
 * skipped (the CLI only sends changed / removed links). */
static mg_err_t parse_source_evidence(mpack_node_t args, src_ev_t **out, size_t *out_n) {
  mpack_node_t arr;
  size_t n, i, count = 0;
  src_ev_t *ev;
  *out = NULL;
  *out_n = 0;
  if (mpack_node_is_missing(args) || mpack_node_is_nil(args)) return MG_OK;
  arr = mpack_node_map_cstr_optional(args, "sources");
  if (mpack_node_is_missing(arr) || mpack_node_is_nil(arr)) return MG_OK;
  if (mpack_node_type(arr) != mpack_type_array) return MG_ERR_INVALID_ARG;
  n = mpack_node_array_length(arr);
  if (n > MAINT_MAX_SOURCES) return MG_ERR_INVALID_ARG;
  ev = (src_ev_t *)calloc(n ? n : 1, sizeof(*ev));
  if (!ev) return MG_ERR_OOM;
  for (i = 0; i < n; ++i) {
    mpack_node_t item = mpack_node_array_at(arr, i);
    src_ev_t *e = &ev[count];
    int bad = 0;
    char *hex;
    if (mpack_node_type(item) != mpack_type_map) continue;
    hex = opt_str(item, "id_hex", &bad);
    e->locator = opt_str(item, "locator", &bad);
    e->state = opt_str(item, "state", &bad);
    e->recorded = opt_str(item, "recorded_fingerprint", &bad);
    e->current = opt_str(item, "fingerprint", &bad);
    if (bad || !hex || !e->locator || !e->state || !decode_id(hex, e->node) ||
        (strcmp(e->state, "changed") != 0 && strcmp(e->state, "removed") != 0)) {
      free(e->locator);
      free(e->state);
      free(e->recorded);
      free(e->current);
      memset(e, 0, sizeof(*e));
      free(hex);
      continue;
    }
    hex_of(e->node, e->hex);
    free(hex);
    count++;
  }
  *out = ev;
  *out_n = count;
  return MG_OK;
}

mg_err_t mg_op_maintain_scan(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  mg_config_t fallback;
  const mg_config_t *cfg;
  cand_list_t l;
  src_ev_t *ev = NULL;
  size_t n_ev = 0, scanned = 0, stored, i, cap;
  int64_t limit = MAINT_DEFAULT_LIMIT, now = now_ms();
  int64_t by_kind[MG_MAINT_N_KINDS];
  int bad = 0, sources_checked;
  char *project;
  mg_maint_candidate_t *rows;
  mg_err_t err;

  if (!ctx || !ctx->storage || !result) return MG_ERR_INVALID_ARG;
  if (opt_int(args, "limit", &limit) < 0 || limit < 0) return MG_ERR_INVALID_ARG;
  project = opt_str(args, "project", &bad);
  if (bad) {
    free(project);
    return MG_ERR_INVALID_ARG;
  }
  err = parse_source_evidence(args, &ev, &n_ev);
  if (err != MG_OK) {
    free(project);
    return err;
  }
  sources_checked = !mpack_node_is_missing(args) && !mpack_node_is_nil(args) &&
                    !mpack_node_is_missing(mpack_node_map_cstr_optional(args, "sources"));
  cfg = cfg_or_defaults(ctx, &fallback);
  cap = cfg->maint_scan_cap > 0 ? (size_t)cfg->maint_scan_cap : 200;

  memset(&l, 0, sizeof(l));
  err = scan_contradictions(&l, ctx->storage, cap, ev, n_ev);
  if (err == MG_OK) err = scan_sources(&l, ctx->storage, project, ev, n_ev);
  if (err == MG_OK) err = scan_near_duplicates(&l, ctx->storage, cfg, ev, n_ev, &scanned);
  if (err == MG_OK) err = scan_keywords(&l, ctx->storage, cap);
  if (err == MG_OK) err = scan_isolated(&l, ctx->storage, cfg, cap, now);
  if (!ctx->config) mg_config_free(&fallback);
  src_ev_free(ev, n_ev);
  free(project);
  if (err != MG_OK) {
    cand_list_free(&l);
    return err;
  }

  qsort(l.v, l.n, sizeof(*l.v), cmp_cand);
  stored = l.n < cap ? l.n : cap;
  rows = (mg_maint_candidate_t *)calloc(stored ? stored : 1, sizeof(*rows));
  if (!rows) {
    cand_list_free(&l);
    return MG_ERR_OOM;
  }
  memset(by_kind, 0, sizeof(by_kind));
  for (i = 0; i < stored; ++i) {
    memcpy(rows[i].id, l.v[i].id, sizeof(rows[i].id));
    snprintf(rows[i].kind, sizeof(rows[i].kind), "%s", mg_maint_kind_name((mg_maint_kind_t)l.v[i].kind));
    rows[i].priority = l.v[i].kind + 1;
    rows[i].score = l.v[i].score;
    rows[i].nodes = l.v[i].nodes;
    memcpy(rows[i].evidence, l.v[i].evidence, sizeof(rows[i].evidence));
    rows[i].payload = l.v[i].payload;
    rows[i].payload_len = l.v[i].payload_len;
    by_kind[l.v[i].kind]++;
  }
  err = mg_storage_maint_store_candidates(ctx->storage, rows, stored, now);
  free(rows);
  if (err != MG_OK) {
    cand_list_free(&l);
    return err;
  }

  mpack_build_map(result);
  mpack_write_cstr(result, "summary");
  mpack_build_map(result);
  mpack_write_cstr(result, "pending"); mpack_write_int(result, (int64_t)stored);
  mpack_write_cstr(result, "by_kind");
  mpack_build_map(result);
  for (i = 0; i < MG_MAINT_N_KINDS; ++i) {
    mpack_write_cstr(result, mg_maint_kind_name((mg_maint_kind_t)i));
    mpack_write_int(result, by_kind[i]);
  }
  mpack_complete_map(result);
  mpack_write_cstr(result, "dismissed");       mpack_write_int(result, l.dismissed);
  mpack_write_cstr(result, "truncated");       mpack_write_bool(result, l.n > cap);
  mpack_write_cstr(result, "scanned_nodes");   mpack_write_int(result, (int64_t)scanned);
  mpack_write_cstr(result, "sources_checked"); mpack_write_bool(result, sources_checked != 0);
  mpack_complete_map(result);
  mpack_write_cstr(result, "candidates");
  mpack_build_array(result);
  for (i = 0; i < stored && (int64_t)i < limit; ++i) {
    mpack_write_object_bytes(result, l.v[i].payload, l.v[i].payload_len);
  }
  mpack_complete_array(result);
  mpack_complete_map(result);
  cand_list_free(&l);
  return MG_OK;
}

/* --------------------------------------------------------------- resolve */

/* Splits a candidate's node csv. Returns the count (<= 2 kept). */
static size_t candidate_nodes(const char *csv, mg_node_id_t out[2]) {
  size_t n = 0;
  const char *p = csv;
  while (p && *p && n < 2) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    if (len == HEX_ID_LEN && mg_retrieve_hex_decode(p, len, out[n], MG_NODE_ID_BYTES) == 0) n++;
    p = comma ? comma + 1 : NULL;
  }
  return n;
}

static int index_of(const mg_node_id_t *ids, size_t n, const mg_node_id_t id) {
  size_t i;
  for (i = 0; i < n; ++i) {
    if (memcmp(ids[i], id, MG_NODE_ID_BYTES) == 0) return (int)i;
  }
  return -1;
}

/* `refresh` on a source_changed candidate: record the fingerprints the scan
 * observed on the node's links. Returns the number of links updated. */
static mg_err_t refresh_from_payload(mg_storage_t *s, const mg_maint_candidate_t *c,
                                     const mg_node_id_t node, int64_t *updated) {
  mpack_tree_t tree;
  mpack_node_t sig, srcs, proj;
  char *project = NULL;
  size_t i, n;
  mg_err_t err = MG_OK;
  int bad = 0;
  *updated = 0;
  mpack_tree_init_data(&tree, (const char *)c->payload, c->payload_len);
  mpack_tree_parse(&tree);
  if (mpack_tree_error(&tree) != mpack_ok) {
    mpack_tree_destroy(&tree);
    return MG_ERR_STORAGE;
  }
  sig = mpack_node_map_cstr_optional(mpack_tree_root(&tree), "signals");
  proj = mpack_node_map_cstr_optional(sig, "project");
  if (mpack_node_type(proj) == mpack_type_str) project = mg_retrieve_node_str_dup(proj);
  srcs = mpack_node_map_cstr_optional(sig, "sources");
  n = mpack_node_type(srcs) == mpack_type_array ? mpack_node_array_length(srcs) : 0;
  for (i = 0; err == MG_OK && i < n; ++i) {
    mpack_node_t item = mpack_node_array_at(srcs, i);
    char *loc = opt_str(item, "locator", &bad);
    char *fp = opt_str(item, "current_fingerprint", &bad);
    int64_t one = 0;
    if (loc && fp) {
      err = mg_storage_refresh_source(s, node, project ? project : "", "file", loc, fp,
                                      now_ms(), &one);
      *updated += one;
    }
    free(loc);
    free(fp);
  }
  free(project);
  mpack_tree_destroy(&tree);
  return err;
}

mg_err_t mg_op_maintain_resolve(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  mg_maint_candidate_t cand;
  mg_node_id_t cnodes[2], node, by;
  mg_maint_change_t changes[2];
  size_t n_cnodes = 0, n_changes = 0, i;
  int bad = 0, has_cand = 0, has_node, has_by, dismiss = 0;
  char *action, *cid, *node_hex, *by_hex, *note, *actor;
  char nodes_log[3 * (HEX_ID_LEN + 1)], detail[160];
  int64_t refreshed = -1;
  mg_maint_log_t log;
  mg_err_t err = MG_OK;

  if (!ctx || !ctx->storage || !result) return MG_ERR_INVALID_ARG;
  memset(&cand, 0, sizeof(cand));
  memset(changes, 0, sizeof(changes));
  action = opt_str(args, "action", &bad);
  cid = opt_str(args, "candidate_id", &bad);
  node_hex = opt_str(args, "node", &bad);
  by_hex = opt_str(args, "by", &bad);
  note = opt_str(args, "note", &bad);
  actor = opt_str(args, "actor", &bad);
  has_node = node_hex != NULL;
  has_by = by_hex != NULL;
  if (bad || !action || (has_node && !decode_id(node_hex, node)) || (has_by && !decode_id(by_hex, by)) ||
      (!cid && !has_node)) {
    err = MG_ERR_INVALID_ARG;
    goto done;
  }
  if (cid) {
    err = mg_storage_maint_get_candidate(ctx->storage, cid, &cand);
    if (err != MG_OK) goto done;
    has_cand = 1;
    n_cnodes = candidate_nodes(cand.nodes, cnodes);
    /* a node named next to a candidate must be one of its nodes */
    if (has_node && index_of((const mg_node_id_t *)cnodes, n_cnodes, node) < 0) {
      err = MG_ERR_INVALID_ARG;
      goto done;
    }
  }
  /* the single target of stale / retire / restore / supersede */
  if (!has_node && has_cand) {
    if (n_cnodes == 1) {
      memcpy(node, cnodes[0], MG_NODE_ID_BYTES);
      has_node = 1;
    } else if (n_cnodes == 2 && has_by && !strcmp(action, "supersede")) {
      int at = index_of((const mg_node_id_t *)cnodes, 2, by);
      if (at >= 0) {
        memcpy(node, cnodes[1 - at], MG_NODE_ID_BYTES);
        has_node = 1;
      }
    }
  }

  detail[0] = '\0';
  if (!strcmp(action, "keep") || !strcmp(action, "keep_both")) {
    if (!has_cand) err = MG_ERR_INVALID_ARG;
    dismiss = 1;
    snprintf(detail, sizeof(detail), "dismissed until the evidence changes");
  } else if (!strcmp(action, "stale") || !strcmp(action, "retire") || !strcmp(action, "restore")) {
    if (!has_node) {
      err = MG_ERR_INVALID_ARG;
    } else {
      changes[0].to = !strcmp(action, "stale") ? MG_MAINT_TO_STALE
                    : !strcmp(action, "retire") ? MG_MAINT_TO_RETIRED : MG_MAINT_TO_ACTIVE;
      memcpy(changes[0].node, node, MG_NODE_ID_BYTES);
      n_changes = 1;
    }
  } else if (!strcmp(action, "supersede")) {
    if (!has_node || !has_by) {
      err = MG_ERR_INVALID_ARG;
    } else {
      changes[0].to = MG_MAINT_TO_SUPERSEDED;
      memcpy(changes[0].node, node, MG_NODE_ID_BYTES);
      memcpy(changes[0].by, by, MG_NODE_ID_BYTES);
      n_changes = 1;
    }
  } else if (!strcmp(action, "supersede_a") || !strcmp(action, "supersede_b")) {
    int a = !strcmp(action, "supersede_a") ? 0 : 1;
    if (!has_cand || n_cnodes != 2 || has_by) {
      err = MG_ERR_INVALID_ARG;
    } else {
      changes[0].to = MG_MAINT_TO_SUPERSEDED;
      memcpy(changes[0].node, cnodes[a], MG_NODE_ID_BYTES);
      memcpy(changes[0].by, cnodes[1 - a], MG_NODE_ID_BYTES);
      memcpy(by, cnodes[1 - a], MG_NODE_ID_BYTES);
      has_by = 1;
      n_changes = 1;
    }
  } else if (!strcmp(action, "merge")) {
    /* the agent inserted the merged note first; every candidate node that
     * is not that note is superseded by it */
    if (!has_cand || !has_by) {
      err = MG_ERR_INVALID_ARG;
    } else {
      for (i = 0; i < n_cnodes; ++i) {
        if (memcmp(cnodes[i], by, MG_NODE_ID_BYTES) == 0) continue;
        changes[n_changes].to = MG_MAINT_TO_SUPERSEDED;
        memcpy(changes[n_changes].node, cnodes[i], MG_NODE_ID_BYTES);
        memcpy(changes[n_changes].by, by, MG_NODE_ID_BYTES);
        n_changes++;
      }
    }
  } else if (!strcmp(action, "refresh")) {
    if (!has_cand || strcmp(cand.kind, "source_changed") != 0 || n_cnodes != 1) {
      err = MG_ERR_INVALID_ARG;
    } else {
      err = refresh_from_payload(ctx->storage, &cand, cnodes[0], &refreshed);
      snprintf(detail, sizeof(detail), "refreshed=%lld", (long long)refreshed);
    }
  } else {
    err = MG_ERR_INVALID_ARG;
  }
  if (err != MG_OK) goto done;

  /* audit row: the nodes touched (candidate's when none) plus `by` */
  if (n_changes > 0) {
    mg_node_id_t touched[3];
    size_t nt = 0;
    for (i = 0; i < n_changes; ++i) memcpy(touched[nt++], changes[i].node, MG_NODE_ID_BYTES);
    if (has_by && index_of((const mg_node_id_t *)touched, nt, by) < 0) memcpy(touched[nt++], by, MG_NODE_ID_BYTES);
    nodes_csv((const mg_node_id_t *)touched, nt, nodes_log, sizeof(nodes_log));
  } else {
    snprintf(nodes_log, sizeof(nodes_log), "%s", has_cand ? cand.nodes : "");
  }
  if (!detail[0] && n_changes > 0) {
    if (changes[0].to == MG_MAINT_TO_SUPERSEDED) {
      char h[HEX_ID_LEN + 1];
      hex_of(by, h);
      snprintf(detail, sizeof(detail), "superseded_by=%s", h);
    } else {
      snprintf(detail, sizeof(detail), "to=%s",
               changes[0].to == MG_MAINT_TO_STALE ? "stale"
               : changes[0].to == MG_MAINT_TO_RETIRED ? "retired" : "active");
    }
  }
  memset(&log, 0, sizeof(log));
  log.ts = now_ms();
  log.actor = actor && *actor ? actor : "agent";
  log.action = action;
  log.candidate_id = has_cand ? cand.id : NULL;
  log.kind = has_cand ? cand.kind : NULL;
  log.nodes = nodes_log;
  log.detail = detail;
  log.note = note;
  err = mg_storage_maint_apply(ctx->storage, changes, n_changes,
                               dismiss ? cand.id : NULL, dismiss ? cand.kind : NULL,
                               dismiss ? cand.evidence : NULL, has_cand ? cand.id : NULL, &log);
  if (err != MG_OK) goto done;

  mpack_build_map(result);
  mpack_write_cstr(result, "action"); mpack_write_cstr(result, action);
  mpack_write_cstr(result, "candidate_id");
  if (has_cand) mpack_write_cstr(result, cand.id); else mpack_write_nil(result);
  mpack_write_cstr(result, "dismissed"); mpack_write_bool(result, dismiss != 0);
  if (refreshed >= 0) {
    mpack_write_cstr(result, "refreshed");
    mpack_write_int(result, refreshed);
  }
  mpack_write_cstr(result, "nodes");
  mpack_build_array(result);
  if (n_changes > 0) {
    for (i = 0; i < n_changes; ++i) write_node(result, ctx->storage, changes[i].node);
  } else {
    for (i = 0; i < n_cnodes; ++i) write_node(result, ctx->storage, cnodes[i]);
  }
  mpack_complete_array(result);
  mpack_complete_map(result);

done:
  mg_maint_candidate_clear(&cand);
  free(action);
  free(cid);
  free(node_hex);
  free(by_hex);
  free(note);
  free(actor);
  return err;
}

/* ------------------------------------------------------------ apply-safe */

mg_err_t mg_op_maintain_apply_safe(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  mg_config_t fallback;
  const mg_config_t *cfg;
  mg_storage_consolidate_report_t rep;
  mg_maint_status_t before;
  int64_t now = now_ms(), purged = 0, collapsed = 0, dropped = 0, retention_days;
  int bad = 0;
  char *actor;
  char detail[192];
  mg_maint_log_t log;
  mg_err_t err;

  if (!ctx || !ctx->storage || !result) return MG_ERR_INVALID_ARG;
  actor = opt_str(args, "actor", &bad);
  if (bad) {
    free(actor);
    return MG_ERR_INVALID_ARG;
  }
  cfg = cfg_or_defaults(ctx, &fallback);
  retention_days = cfg->maint_retention_days >= 0 ? cfg->maint_retention_days : 30;
  if (!ctx->config) mg_config_free(&fallback);

  err = mg_storage_maint_status(ctx->storage, now - retention_days * MAINT_DAY_MS, &before);
  if (err == MG_OK) {
    err = mg_storage_maint_purge_retired(ctx->storage, now - retention_days * MAINT_DAY_MS, now,
                                         actor && *actor ? actor : "apply-safe", &purged);
  }
  if (err == MG_OK) {
    err = mg_storage_maint_collapse_exact(ctx->storage, now, actor && *actor ? actor : "apply-safe",
                                          &collapsed);
  }
  /* the structural pass is consolidate's, unchanged */
  if (err == MG_OK) err = mg_storage_consolidate(ctx->storage, &rep);
  if (err == MG_OK) err = mg_storage_maint_prune_candidates(ctx->storage, &dropped);
  if (err == MG_OK) err = mg_storage_maint_meta_set(ctx->storage, MG_MAINT_META_LAST_APPLY_SAFE, now);
  if (err == MG_OK) {
    snprintf(detail, sizeof(detail),
             "purged=%lld collapsed=%lld expired=%lld orphan_sources=%lld candidates_dropped=%lld",
             (long long)purged, (long long)collapsed, (long long)rep.expired_deleted,
             (long long)rep.orphan_sources_deleted, (long long)dropped);
    memset(&log, 0, sizeof(log));
    log.ts = now;
    log.actor = actor && *actor ? actor : "apply-safe";
    log.action = "apply_safe";
    log.detail = detail;
    err = mg_storage_maint_log_append(ctx->storage, &log);
  }
  free(actor);
  if (err != MG_OK) return err;

  mpack_build_map(result);
  mpack_write_cstr(result, "previous_apply_safe_at"); mpack_write_int(result, before.last_apply_safe_at);
  mpack_write_cstr(result, "last_apply_safe_at");     mpack_write_int(result, now);
  mpack_write_cstr(result, "inserts_since_last");     mpack_write_int(result, before.inserts_since_apply_safe);
  mpack_write_cstr(result, "retention_days");         mpack_write_int(result, retention_days);
  mpack_write_cstr(result, "purged_retired");         mpack_write_int(result, purged);
  mpack_write_cstr(result, "collapsed_duplicates");   mpack_write_int(result, collapsed);
  mpack_write_cstr(result, "candidates_dropped");     mpack_write_int(result, dropped);
  mpack_write_cstr(result, "consolidate");
  mpack_build_map(result);
  mpack_write_cstr(result, "expired_deleted");              mpack_write_int(result, rep.expired_deleted);
  mpack_write_cstr(result, "duplicate_edges_deleted");      mpack_write_int(result, rep.duplicate_edges_deleted);
  mpack_write_cstr(result, "orphan_edges_deleted");         mpack_write_int(result, rep.orphan_edges_deleted);
  mpack_write_cstr(result, "orphan_node_keywords_deleted"); mpack_write_int(result, rep.orphan_node_keywords_deleted);
  mpack_write_cstr(result, "invalid_edges_deleted");        mpack_write_int(result, rep.invalid_edges_deleted);
  mpack_write_cstr(result, "orphan_sources_deleted");       mpack_write_int(result, rep.orphan_sources_deleted);
  mpack_write_cstr(result, "sqlite_analyzed");              mpack_write_bool(result, true);
  mpack_complete_map(result);
  mpack_write_cstr(result, "graph");
  mpack_build_map(result);
  mpack_write_cstr(result, "n_nodes");    mpack_write_int(result, rep.n_nodes);
  mpack_write_cstr(result, "n_edges");    mpack_write_int(result, rep.n_edges);
  mpack_write_cstr(result, "n_keywords"); mpack_write_int(result, rep.n_keywords);
  mpack_complete_map(result);
  mpack_complete_map(result);
  return MG_OK;
}

/* ------------------------------------------------------------ log/status */

mg_err_t mg_op_maintain_log(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  mg_maint_log_row_t *rows = NULL;
  size_t n = 0, i;
  int64_t limit = 50;
  int bad = 0;
  char *node;
  mg_node_id_t id;
  mg_err_t err;

  if (!ctx || !ctx->storage || !result) return MG_ERR_INVALID_ARG;
  if (opt_int(args, "limit", &limit) < 0 || limit <= 0) return MG_ERR_INVALID_ARG;
  node = opt_str(args, "node", &bad);
  if (bad || (node && !decode_id(node, id))) {
    free(node);
    return MG_ERR_INVALID_ARG;
  }
  if (node) {
    char lower[HEX_ID_LEN + 1];
    hex_of(id, lower);
    memcpy(node, lower, sizeof(lower));
  }
  err = mg_storage_maint_log_list(ctx->storage, (size_t)limit, node, &rows, &n);
  free(node);
  if (err != MG_OK) return err;

  mpack_build_map(result);
  mpack_write_cstr(result, "entries");
  mpack_build_array(result);
  for (i = 0; i < n; ++i) {
    const char *p = rows[i].nodes;
    mpack_build_map(result);
    mpack_write_cstr(result, "id");     mpack_write_int(result, rows[i].id);
    mpack_write_cstr(result, "ts");     mpack_write_int(result, rows[i].ts);
    mpack_write_cstr(result, "actor");  mpack_write_cstr(result, rows[i].actor ? rows[i].actor : "");
    mpack_write_cstr(result, "action"); mpack_write_cstr(result, rows[i].action ? rows[i].action : "");
    mpack_write_cstr(result, "candidate_id");
    if (rows[i].candidate_id) mpack_write_cstr(result, rows[i].candidate_id); else mpack_write_nil(result);
    mpack_write_cstr(result, "kind");
    if (rows[i].kind) mpack_write_cstr(result, rows[i].kind); else mpack_write_nil(result);
    mpack_write_cstr(result, "nodes");
    mpack_build_array(result);
    while (p && *p) {
      const char *comma = strchr(p, ',');
      size_t len = comma ? (size_t)(comma - p) : strlen(p);
      mpack_write_str(result, p, (uint32_t)len);
      p = comma ? comma + 1 : NULL;
    }
    mpack_complete_array(result);
    mpack_write_cstr(result, "detail");
    if (rows[i].detail) mpack_write_cstr(result, rows[i].detail); else mpack_write_nil(result);
    mpack_write_cstr(result, "note");
    if (rows[i].note) mpack_write_cstr(result, rows[i].note); else mpack_write_nil(result);
    mpack_complete_map(result);
  }
  mpack_complete_array(result);
  mpack_complete_map(result);
  mg_maint_log_rows_free(rows, n);
  return MG_OK;
}

mg_err_t mg_op_maintain_status(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  mg_config_t fallback;
  const mg_config_t *cfg;
  mg_maint_status_t st;
  int64_t now = now_ms(), retention_days, trigger;
  size_t i;
  mg_err_t err;
  (void)args;

  if (!ctx || !ctx->storage || !result) return MG_ERR_INVALID_ARG;
  cfg = cfg_or_defaults(ctx, &fallback);
  retention_days = cfg->maint_retention_days >= 0 ? cfg->maint_retention_days : 30;
  trigger = cfg->maint_trigger_inserts > 0 ? cfg->maint_trigger_inserts : 50;
  if (!ctx->config) mg_config_free(&fallback);
  err = mg_storage_maint_status(ctx->storage, now - retention_days * MAINT_DAY_MS, &st);
  if (err != MG_OK) return err;

  mpack_build_map(result);
  mpack_write_cstr(result, "last_apply_safe_at");       mpack_write_int(result, st.last_apply_safe_at);
  mpack_write_cstr(result, "last_scan_at");             mpack_write_int(result, st.last_scan_at);
  mpack_write_cstr(result, "inserts_since_apply_safe"); mpack_write_int(result, st.inserts_since_apply_safe);
  mpack_write_cstr(result, "inserts_since_scan");       mpack_write_int(result, st.inserts_since_scan);
  mpack_write_cstr(result, "pending");
  mpack_build_map(result);
  mpack_write_cstr(result, "total"); mpack_write_int(result, st.pending_total);
  mpack_write_cstr(result, "by_kind");
  mpack_build_map(result);
  for (i = 0; i < MG_MAINT_N_KINDS; ++i) {
    mpack_write_cstr(result, mg_maint_kind_name((mg_maint_kind_t)i));
    mpack_write_int(result, st.pending[i]);
  }
  mpack_complete_map(result);
  mpack_complete_map(result);
  mpack_write_cstr(result, "nodes");
  mpack_build_map(result);
  mpack_write_cstr(result, "active");      mpack_write_int(result, st.n_active);
  mpack_write_cstr(result, "stale");       mpack_write_int(result, st.n_stale);
  mpack_write_cstr(result, "superseded");  mpack_write_int(result, st.n_superseded);
  mpack_write_cstr(result, "retired");     mpack_write_int(result, st.n_retired);
  mpack_write_cstr(result, "retired_due"); mpack_write_int(result, st.retired_due);
  mpack_complete_map(result);
  mpack_write_cstr(result, "retention_days");  mpack_write_int(result, retention_days);
  mpack_write_cstr(result, "trigger_inserts"); mpack_write_int(result, trigger);
  /* what an agent should run next, cheapest first; empty = nothing due */
  mpack_write_cstr(result, "recommended");
  mpack_build_array(result);
  if (st.inserts_since_apply_safe >= trigger || st.retired_due > 0 ||
      (st.last_apply_safe_at == 0 && st.n_active + st.n_stale > 0)) {
    mpack_write_cstr(result, "apply-safe");
  }
  if (st.inserts_since_scan >= trigger || (st.last_scan_at == 0 && st.n_active > 1)) {
    mpack_write_cstr(result, "scan");
  }
  if (st.pending_total > 0) mpack_write_cstr(result, "resolve");
  mpack_complete_array(result);
  mpack_complete_map(result);
  return MG_OK;
}
