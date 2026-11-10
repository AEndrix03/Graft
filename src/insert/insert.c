#include "graft/ops.h"
#include "graft/maintain.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

mg_err_t mg_insert_build_edges_from_embedding(
  mg_storage_t *s,
  const mg_config_t *cfg,
  const mg_node_id_t src,
  const mg_embedding_t q,
  const mg_keyword_id_t *kw_ids,
  size_t n_kw,
  mg_edge_t **out_edges,
  size_t *out_n_edges,
  size_t *out_n_kw_edges,
  size_t *out_n_sem_edges
);

static mg_err_t copy_msgpack_string(mpack_node_t map, const char *key, char **out) {
  if (!key || !out) {
    return MG_ERR_INVALID_ARG;
  }

  mpack_node_t node = mpack_node_map_cstr(map, key);
  if (mpack_node_type(node) != mpack_type_str) {
    return MG_ERR_INVALID_ARG;
  }

  size_t len = mpack_node_strlen(node);
  char *copy = (char *)malloc(len + 1u);
  if (!copy) {
    return MG_ERR_OOM;
  }
  memcpy(copy, mpack_node_str(node), len);
  copy[len] = '\0';

  if (len == 0u) {
    free(copy);
    return MG_ERR_INVALID_ARG;
  }

  *out = copy;
  return MG_OK;
}

static int cmp_cstr_ptr(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

/* A keyword may not be empty or contain ',' or NUL. The content hash joins
 * the sorted keywords with ',', so ["a,b", "c"] and ["a", "b,c"] would hash
 * alike and the second insert would come back as a false duplicate.
 * Rejecting the separator keeps the hash of every existing node unchanged. */
int mg_insert_keyword_valid(const char *kw, size_t len) {
  return len > 0u && !memchr(kw, ',', len) && !memchr(kw, '\0', len);
}

static mg_err_t parse_keywords(mpack_node_t args, char ***out_keywords, size_t *out_count) {
  if (!out_keywords || !out_count) {
    return MG_ERR_INVALID_ARG;
  }

  mpack_node_t arr = mpack_node_map_cstr(args, "keywords");
  if (mpack_node_type(arr) != mpack_type_array) {
    return MG_ERR_INVALID_ARG;
  }

  size_t count = mpack_node_array_length(arr);
  char **keywords = NULL;
  if (count > 0u) {
    keywords = (char **)calloc(count, sizeof(*keywords));
    if (!keywords) {
      return MG_ERR_OOM;
    }
  }

  for (size_t i = 0u; i < count; ++i) {
    mpack_node_t item = mpack_node_array_at(arr, i);
    if (mpack_node_type(item) != mpack_type_str) {
      for (size_t j = 0u; j < i; ++j) {
        free(keywords[j]);
      }
      free(keywords);
      return MG_ERR_INVALID_ARG;
    }

    size_t len = mpack_node_strlen(item);
    if (!mg_insert_keyword_valid(mpack_node_str(item), len)) {
      for (size_t j = 0u; j < i; ++j) {
        free(keywords[j]);
      }
      free(keywords);
      return MG_ERR_INVALID_ARG;
    }

    keywords[i] = (char *)malloc(len + 1u);
    if (!keywords[i]) {
      for (size_t j = 0u; j < i; ++j) {
        free(keywords[j]);
      }
      free(keywords);
      return MG_ERR_OOM;
    }
    memcpy(keywords[i], mpack_node_str(item), len);
    keywords[i][len] = '\0';
  }

  *out_keywords = keywords;
  *out_count = count;
  return MG_OK;
}

static void free_keywords(char **keywords, size_t count) {
  if (!keywords) {
    return;
  }
  for (size_t i = 0u; i < count; ++i) {
    free(keywords[i]);
  }
  free(keywords);
}

/* Content hash covers title + body + sorted keywords. Author and dates are
 * intentionally NOT hashed: the same node saved by different authors or at
 * different times should still dedupe on identical content. */
static mg_err_t build_content_hash(
  const char *title,
  const char *body,
  char **keywords,
  size_t n_keywords,
  mg_hash_t out
) {
  size_t total = strlen(title) + 1u + strlen(body) + 1u;
  for (size_t i = 0u; i < n_keywords; ++i) {
    total += strlen(keywords[i]);
    if (i + 1u < n_keywords) {
      total += 1u;
    }
  }

  char *buf = (char *)malloc(total);
  if (!buf) {
    return MG_ERR_OOM;
  }

  qsort(keywords, n_keywords, sizeof(*keywords), cmp_cstr_ptr);

  size_t pos = 0u;
  size_t len = strlen(title);
  memcpy(buf + pos, title, len);
  pos += len;
  buf[pos++] = '\0';

  len = strlen(body);
  memcpy(buf + pos, body, len);
  pos += len;
  buf[pos++] = '\0';

  for (size_t i = 0u; i < n_keywords; ++i) {
    len = strlen(keywords[i]);
    memcpy(buf + pos, keywords[i], len);
    pos += len;
    if (i + 1u < n_keywords) {
      buf[pos++] = ',';
    }
  }

  mg_blake3((const uint8_t *)buf, pos, out);
  free(buf);
  return MG_OK;
}

static int64_t now_unix_ms(void) {
  return (int64_t)time(NULL) * 1000;
}

static void write_id_hex(const mg_node_id_t id, char out[33]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0u; i < MG_NODE_ID_BYTES; ++i) {
    out[i * 2u] = hex[(id[i] >> 4) & 0x0f];
    out[i * 2u + 1u] = hex[id[i] & 0x0f];
  }
  out[32] = '\0';
}

#define MG_INSERT_SIMILAR_CAP 10

/* Existing notes close to the new one (issue #22), for the agent writing it
 * to check right away: a correction should supersede them, a conflict it
 * cannot settle yet is recorded with `maintain resolve --action contradicts`.
 * Only ACTIVE / STALE notes are candidates (vector_topk's filter); the node
 * an explicit `supersedes` already replaces is left out. */
static size_t find_similar(mg_storage_t *s, const mg_config_t *cfg, const mg_embedding_t q,
                           const mg_node_id_t *skip, mg_node_score_t *out) {
  mg_node_score_t top[MG_INSERT_SIMILAR_CAP + 1];
  int n_top = 0, max = cfg->edge_similar_report_max;
  size_t n = 0;
  if (max <= 0) return 0;
  if (max > MG_INSERT_SIMILAR_CAP) max = MG_INSERT_SIMILAR_CAP;
  if (mg_storage_vector_topk(s, q, max + 1, top, &n_top) != MG_OK) return 0;
  for (int i = 0; i < n_top && n < (size_t)max; ++i) {
    if (top[i].score < cfg->edge_similar_report_min) break;   /* sorted descending */
    if (skip && memcmp(top[i].id, *skip, MG_NODE_ID_BYTES) == 0) continue;
    out[n++] = top[i];
  }
  return n;
}

static void write_insert_result(
  mpack_writer_t *result,
  mg_storage_t *s,
  const mg_node_id_t id,
  size_t n_kw_edges,
  size_t n_sem_edges,
  bool duplicate,
  size_t n_sources,
  mg_node_state_t state,
  const mg_node_id_t superseded_by,
  const mg_node_score_t *similar,   /* NULL on a duplicate: no list at all */
  size_t n_similar
) {
  char id_hex[33];
  write_id_hex(id, id_hex);

  mpack_start_map(result, 6u + (n_sources > 0 ? 1u : 0u)
                          + (state == MG_NODE_SUPERSEDED ? 1u : 0u)
                          + (similar ? 1u : 0u));
  mpack_write_cstr(result, "id");
  mpack_write_bin(result, (const char *)id, MG_NODE_ID_BYTES);
  mpack_write_cstr(result, "id_hex");
  mpack_write_cstr(result, id_hex);
  mpack_write_cstr(result, "n_kw_edges");
  mpack_write_u64(result, (uint64_t)n_kw_edges);
  mpack_write_cstr(result, "n_sem_edges");
  mpack_write_u64(result, (uint64_t)n_sem_edges);
  mpack_write_cstr(result, "duplicate");
  mpack_write_bool(result, duplicate);
  /* A duplicate answers with the existing node, which may not be live:
   * the state says whether a search can still reach it. */
  mpack_write_cstr(result, "state");
  mpack_write_cstr(result, mg_node_state_name(state));
  if (state == MG_NODE_SUPERSEDED) {
    if (superseded_by) {
      char by_hex[33];
      write_id_hex(superseded_by, by_hex);
      mpack_write_cstr(result, "superseded_by");
      mpack_write_cstr(result, by_hex);
    } else {
      mpack_write_cstr(result, "superseded_by");
      mpack_write_nil(result);
    }
  }
  if (n_sources > 0) {
    /* also on a duplicate: the sources were attached to the existing node */
    mpack_write_cstr(result, "sources_attached");
    mpack_write_u64(result, (uint64_t)n_sources);
  }
  if (similar) {
    mpack_write_cstr(result, "similar");
    mpack_start_array(result, (uint32_t)n_similar);
    for (size_t i = 0u; i < n_similar; ++i) {
      mg_node_t node = {0};
      char hex[33];
      int found = mg_storage_get_node(s, similar[i].id, &node) == MG_OK;
      write_id_hex(similar[i].id, hex);
      mpack_start_map(result, 4);
      mpack_write_cstr(result, "id_hex");     mpack_write_cstr(result, hex);
      mpack_write_cstr(result, "title");      mpack_write_cstr(result, found && node.title ? node.title : "");
      mpack_write_cstr(result, "state");
      mpack_write_cstr(result, mg_node_state_name(found ? (mg_node_state_t)node.state : MG_NODE_ACTIVE));
      mpack_write_cstr(result, "similarity");
      mpack_write_double(result, (double)(long long)(similar[i].score * 10000.0f + 0.5f) / 10000.0);
      mpack_finish_map(result);
      if (found) mg_node_free(&node);
    }
    mpack_finish_array(result);
  }
  mpack_finish_map(result);
}

/* Saving the exact content of an existing node again. A retired node is
 * wanted after all: bring it back (audited like a `maintain resolve ...
 * restore`) instead of answering with an id no search can reach. A
 * superseded one stays superseded, since restoring it would break its
 * lineage: the caller gets its state and successor and decides. */
static mg_err_t settle_duplicate(mg_storage_t *s, const mg_node_id_t id, int64_t now,
                                 mg_node_state_t *state, mg_node_id_t superseded_by,
                                 bool *has_superseded_by) {
  mg_node_t node;
  mg_maint_change_t change;
  mg_maint_log_t log;
  char hex[33];
  mg_err_t err = mg_storage_get_node(s, id, &node);
  if (err != MG_OK) return err;
  *state = node.state;
  *has_superseded_by = false;
  mg_node_free(&node);
  if (*state == MG_NODE_SUPERSEDED) {
    err = mg_storage_superseded_by(s, id, superseded_by);
    if (err == MG_OK) *has_superseded_by = true;
    return err == MG_ERR_NOT_FOUND ? MG_OK : err;
  }
  if (*state != MG_NODE_RETIRED) return MG_OK;
  memset(&change, 0, sizeof(change));
  change.to = MG_MAINT_TO_ACTIVE;
  memcpy(change.node, id, MG_NODE_ID_BYTES);
  write_id_hex(id, hex);
  memset(&log, 0, sizeof(log));
  log.ts = now;
  log.actor = "insert";
  log.action = "restore";
  log.nodes = hex;
  log.detail = "same content inserted again";
  err = mg_storage_maint_apply(s, &change, 1, NULL, NULL, NULL, NULL, &log);
  if (err == MG_OK) *state = MG_NODE_ACTIVE;
  return err;
}

mg_err_t mg_op_insert(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  if (!ctx || !ctx->storage || !ctx->embed || !ctx->config || !result) {
    return MG_ERR_INVALID_ARG;
  }

  char *title = NULL;
  char *body = NULL;
  char *author = NULL;
  char **keywords = NULL;
  size_t n_keywords = 0u;
  int64_t expires_at = 0;
  mg_node_id_t supersedes_id;
  bool has_supersedes = false;
  mg_node_score_t similar[MG_INSERT_SIMILAR_CAP];
  size_t n_similar = 0u;

  mg_err_t err = copy_msgpack_string(args, "title", &title);
  if (err != MG_OK) {
    return err;
  }
  err = copy_msgpack_string(args, "body", &body);
  if (err != MG_OK) {
    free(title);
    return err;
  }
  /* Optional fields. Author is empty/missing → NULL on the node. */
  {
    mpack_node_t a = mpack_node_map_cstr_optional(args, "author");
    if (!mpack_node_is_missing(a) && !mpack_node_is_nil(a)) {
      const char *s = mpack_node_str(a);
      uint32_t n = (uint32_t)mpack_node_strlen(a);
      if (s && n > 0u) {
        author = (char *)malloc((size_t)n + 1u);
        if (author) { memcpy(author, s, n); author[n] = '\0'; }
      }
    }
    mpack_node_t e = mpack_node_map_cstr_optional(args, "expires_at");
    if (!mpack_node_is_missing(e) && !mpack_node_is_nil(e)) {
      expires_at = (int64_t)mpack_node_i64(e);
      if (expires_at < 0) expires_at = 0;
    }
    /* Optional supersedes: hex node id whose content this insert replaces. */
    mpack_node_t sup = mpack_node_map_cstr_optional(args, "supersedes");
    if (!mpack_node_is_missing(sup) && !mpack_node_is_nil(sup)
        && mpack_node_type(sup) == mpack_type_str) {
      const char *hex = mpack_node_str(sup);
      uint32_t hex_n  = (uint32_t)mpack_node_strlen(sup);
      if (hex_n == 2 * MG_NODE_ID_BYTES) {
        size_t j;
        bool ok = true;
        for (j = 0; j < MG_NODE_ID_BYTES; ++j) {
          int hi = -1, lo = -1;
          char c = hex[j * 2];
          if      (c >= '0' && c <= '9') hi = c - '0';
          else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
          else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
          c = hex[j * 2 + 1];
          if      (c >= '0' && c <= '9') lo = c - '0';
          else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
          else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
          if (hi < 0 || lo < 0) { ok = false; break; }
          supersedes_id[j] = (uint8_t)((hi << 4) | lo);
        }
        has_supersedes = ok;
      }
    }
  }
  err = parse_keywords(args, &keywords, &n_keywords);
  if (err != MG_OK) {
    free(title);
    free(body);
    free(author);
    return err;
  }

  /* Optional provenance. Parsed before anything is written so a bad source
   * rejects the whole insert. */
  int64_t now = now_unix_ms();
  mg_source_t *sources = NULL;
  size_t n_sources = 0u;
  err = mg_op_parse_sources(args, now, &sources, &n_sources);
  if (err != MG_OK) {
    free_keywords(keywords, n_keywords);
    free(title);
    free(body);
    free(author);
    return err;
  }

  mg_keyword_id_t *kw_ids = NULL;
  mg_edge_t *edges = NULL;
  size_t n_edges = 0u;
  size_t n_kw_edges = 0u;
  size_t n_sem_edges = 0u;
  mg_node_id_t existing_id;
  mg_node_id_t dup_by;
  mg_node_state_t dup_state = MG_NODE_ACTIVE;
  bool has_dup_by = false;
  mg_node_t node;
  memset(&node, 0, sizeof(node));

  mg_hash_t content_hash;
  err = build_content_hash(title, body, keywords, n_keywords, content_hash);
  if (err != MG_OK) {
    goto done;
  }

  err = mg_storage_node_id_by_hash(ctx->storage, content_hash, existing_id);
  if (err == MG_OK) {
    /* Same content again: attach the new sources to the existing node, so
     * repeated ingestion runs accumulate provenance instead of failing. */
    if (n_sources > 0u) {
      err = mg_storage_attach_sources(ctx->storage, existing_id, sources, n_sources);
    }
    if (err == MG_OK) {
      err = settle_duplicate(ctx->storage, existing_id, now, &dup_state, dup_by, &has_dup_by);
    }
    if (err == MG_OK) {
      write_insert_result(result, ctx->storage, existing_id, 0u, 0u, true, n_sources, dup_state,
                          has_dup_by ? dup_by : NULL, NULL, 0u);
    }
    goto done;
  }
  if (err != MG_ERR_NOT_FOUND) {
    goto done;
  }

  mg_uuidv7(node.id);
  memcpy(node.content_hash, content_hash, MG_HASH_BYTES);

  mg_embedding_t q;
  err = mg_embed_text(ctx->embed, title, q);
  if (err != MG_OK) {
    goto done;
  }

  if (n_keywords > 0u) {
    kw_ids = (mg_keyword_id_t *)calloc(n_keywords, sizeof(*kw_ids));
    if (!kw_ids) {
      err = MG_ERR_OOM;
      goto done;
    }
  }

  for (size_t i = 0u; i < n_keywords; ++i) {
    err = mg_storage_upsert_keyword(ctx->storage, keywords[i], NULL, &kw_ids[i]);
    if (err != MG_OK) {
      goto done;
    }
  }

  err = mg_insert_build_edges_from_embedding(
    ctx->storage,
    ctx->config,
    node.id,
    q,
    kw_ids,
    n_keywords,
    &edges,
    &n_edges,
    &n_kw_edges,
    &n_sem_edges
  );
  if (err != MG_OK) {
    goto done;
  }
  /* before the insert, so the new node cannot list itself */
  n_similar = find_similar(ctx->storage, ctx->config, q,
                           has_supersedes ? (const mg_node_id_t *)&supersedes_id : NULL, similar);

  node.title = title;
  node.body = body;
  node.author = author;
  node.created_at = now;
  node.expires_at = expires_at;
  node.last_access = now;
  node.access_count = 0;
  node.state = MG_NODE_ACTIVE;

  err = mg_storage_insert_node_with_sources(ctx->storage, &node, q, kw_ids, n_keywords, edges, n_edges,
                                            has_supersedes ? (const mg_node_id_t *)&supersedes_id : NULL,
                                            sources, n_sources);
  if (err == MG_OK) {
    write_insert_result(result, ctx->storage, node.id, n_kw_edges, n_sem_edges, false, n_sources,
                        MG_NODE_ACTIVE, NULL, similar, n_similar);
  } else if (err == MG_ERR_DUPLICATE &&
             mg_storage_node_id_by_hash(ctx->storage, content_hash, existing_id) == MG_OK) {
    /* A concurrent insert of the same content committed between our hash
     * lookup and our transaction: the UNIQUE content_hash rejected ours.
     * Resolve to the winner so the idempotency contract holds. */
    err = n_sources > 0u
        ? mg_storage_attach_sources(ctx->storage, existing_id, sources, n_sources)
        : MG_OK;
    if (err == MG_OK) {
      err = settle_duplicate(ctx->storage, existing_id, now, &dup_state, dup_by, &has_dup_by);
    }
    if (err == MG_OK) {
      write_insert_result(result, ctx->storage, existing_id, 0u, 0u, true, n_sources, dup_state,
                          has_dup_by ? dup_by : NULL, NULL, 0u);
    }
  }

done:
  free(edges);
  free(kw_ids);
  mg_op_sources_free(sources, n_sources);
  free_keywords(keywords, n_keywords);
  free(title);
  free(body);
  free(author);
  return err;
}

