/* Provenance ops (issue #4).
 *
 * sources_list args (all optional):
 *   { "project": str, "kind": str, "id_hex": str }
 * result:
 *   { "sources": [ { "project", "kind", "locator",
 *                    "nodes": [ { "id_hex", "title", "fingerprint"|nil,
 *                                 "observed_at", "role" } ] } ] }
 * Superseded nodes are left out unless id_hex names one. The daemon never
 * reads source files: `graft sources diff` re-hashes them client-side, where
 * the project root is known.
 *
 * sources_refresh args:
 *   { "id_hex": str,
 *     "updates": [ { "project", "kind", "locator", "fingerprint" } ] }
 * result:
 *   { "id_hex": str, "updated": int }
 * Stores the fingerprints the caller just observed on the node's links
 * without touching node content.
 */

#include "graft/ops.h"
#include "graft/source.h"
#include "graft/error.h"
#include "../retrieve/internal.h"
#include "mpack.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MG_OP_MAX_SOURCES 64

static int64_t now_unix_ms(void) {
  return (int64_t)time(NULL) * 1000;
}

/* Optional string field: missing/nil -> *out NULL; wrong type -> -1. */
static int opt_str(mpack_node_t map, const char *key, char **out) {
  mpack_node_t n = mpack_node_map_cstr_optional(map, key);
  *out = NULL;
  if (mpack_node_is_missing(n) || mpack_node_is_nil(n)) return 0;
  if (mpack_node_type(n) != mpack_type_str) return -1;
  *out = mg_retrieve_node_str_dup(n);
  return *out ? 0 : -1;
}

static int fingerprint_valid(const char *fp) {
  size_t i;
  if (!fp || strlen(fp) != MG_SOURCE_FP_HEX) return 0;
  for (i = 0; i < MG_SOURCE_FP_HEX; ++i) {
    char c = fp[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return 1;
}

static void free_source(mg_source_t *src) {
  free(src->project);
  free(src->kind);
  free(src->locator);
  free(src->fingerprint);
  free(src->role);
  memset(src, 0, sizeof(*src));
}

void mg_op_sources_free(mg_source_t *sources, size_t count) {
  size_t i;
  if (!sources) return;
  for (i = 0; i < count; ++i) free_source(&sources[i]);
  free(sources);
}

static char *dup_cstr(const char *s) {
  size_t n = strlen(s) + 1;
  char *out = (char *)malloc(n);
  if (out) memcpy(out, s, n);
  return out;
}

static mg_err_t parse_source_map(mpack_node_t item, mg_source_t *src) {
  if (opt_str(item, "kind", &src->kind) != 0 || opt_str(item, "locator", &src->locator) != 0 ||
      opt_str(item, "project", &src->project) != 0 ||
      opt_str(item, "fingerprint", &src->fingerprint) != 0 ||
      opt_str(item, "role", &src->role) != 0) {
    return MG_ERR_INVALID_ARG;
  }
  if (!mg_source_kind_valid(src->kind) || !src->locator) return MG_ERR_INVALID_ARG;
  if (!src->project && !(src->project = dup_cstr(""))) return MG_ERR_OOM;
  if (src->fingerprint && !src->fingerprint[0]) {
    free(src->fingerprint);
    src->fingerprint = NULL;
  }
  if (src->fingerprint && !fingerprint_valid(src->fingerprint)) return MG_ERR_INVALID_ARG;
  if (src->role && strcmp(src->role, "primary") != 0 && strcmp(src->role, "supporting") != 0) {
    return MG_ERR_INVALID_ARG;
  }
  if (!strcmp(src->kind, "file") &&
      (!src->locator[0] || !src->project[0] || !src->fingerprint)) {
    return MG_ERR_INVALID_ARG;
  }
  if (!strcmp(src->kind, "url") && !src->locator[0]) return MG_ERR_INVALID_ARG;
  return MG_OK;
}

static mg_err_t parse_source_string(mpack_node_t item, mg_source_t *src) {
  mg_source_spec_t spec;
  char *text = mg_retrieve_node_str_dup(item);
  int rc;
  if (!text) return MG_ERR_INVALID_ARG;
  rc = mg_source_parse(text, 0, &spec, NULL, 0);
  free(text);
  if (rc != 0) return MG_ERR_INVALID_ARG;
  src->kind = dup_cstr(spec.kind);
  src->project = spec.project;
  src->locator = spec.locator;
  src->fingerprint = spec.fingerprint[0] ? dup_cstr(spec.fingerprint) : NULL;
  if (!src->kind || (spec.fingerprint[0] && !src->fingerprint)) return MG_ERR_OOM;
  return MG_OK;
}

mg_err_t mg_op_parse_sources(mpack_node_t args, int64_t observed_at,
                             mg_source_t **out, size_t *out_count) {
  mpack_node_t arr;
  mg_source_t *sources;
  size_t count, i;
  if (!out || !out_count) return MG_ERR_INVALID_ARG;
  *out = NULL;
  *out_count = 0;
  arr = mpack_node_map_cstr_optional(args, "sources");
  if (mpack_node_is_missing(arr) || mpack_node_is_nil(arr)) return MG_OK;
  if (mpack_node_type(arr) != mpack_type_array) return MG_ERR_INVALID_ARG;
  count = mpack_node_array_length(arr);
  if (count == 0) return MG_OK;
  if (count > MG_OP_MAX_SOURCES) return MG_ERR_INVALID_ARG;
  sources = (mg_source_t *)calloc(count, sizeof(*sources));
  if (!sources) return MG_ERR_OOM;
  for (i = 0; i < count; ++i) {
    mpack_node_t item = mpack_node_array_at(arr, i);
    mg_err_t err;
    if (mpack_node_type(item) == mpack_type_map) {
      err = parse_source_map(item, &sources[i]);
    } else if (mpack_node_type(item) == mpack_type_str) {
      err = parse_source_string(item, &sources[i]);
    } else {
      err = MG_ERR_INVALID_ARG;
    }
    if (err != MG_OK) {
      mg_op_sources_free(sources, count);
      return err;
    }
    sources[i].observed_at = observed_at;
  }
  *out = sources;
  *out_count = count;
  return MG_OK;
}

static void write_opt_cstr(mpack_writer_t *w, const char *s) {
  if (s) mpack_write_cstr(w, s);
  else   mpack_write_nil(w);
}

mg_err_t mg_op_write_node_sources(mg_storage_t *s, const mg_node_id_t id,
                                  mpack_writer_t *w) {
  mg_source_link_t *links = NULL;
  size_t n = 0, i;
  mg_err_t err = mg_storage_source_links(s, NULL, NULL, (const mg_node_id_t *)id, &links, &n);
  mpack_build_array(w);
  for (i = 0; err == MG_OK && i < n; ++i) {
    const mg_source_t *src = &links[i].source;
    mpack_build_map(w);
    mpack_write_cstr(w, "kind");        mpack_write_cstr(w, src->kind);
    mpack_write_cstr(w, "locator");     mpack_write_cstr(w, src->locator);
    mpack_write_cstr(w, "project");     mpack_write_cstr(w, src->project);
    mpack_write_cstr(w, "fingerprint"); write_opt_cstr(w, src->fingerprint);
    mpack_write_cstr(w, "role");        mpack_write_cstr(w, src->role);
    mpack_write_cstr(w, "observed_at"); mpack_write_int(w, src->observed_at);
    mpack_complete_map(w);
  }
  mpack_complete_array(w);
  mg_source_links_free(links, n);
  return err;
}

/* Decodes args.id_hex when present. 1 = decoded, 0 = absent, -1 = invalid. */
static int opt_node_id(mpack_node_t args, mg_node_id_t id) {
  mpack_node_t n = mpack_node_map_cstr_optional(args, "id_hex");
  if (mpack_node_is_missing(n) || mpack_node_is_nil(n)) return 0;
  if (mpack_node_type(n) != mpack_type_str ||
      mpack_node_strlen(n) != 2 * MG_NODE_ID_BYTES ||
      mg_retrieve_hex_decode(mpack_node_str(n), mpack_node_strlen(n), id, MG_NODE_ID_BYTES) != 0) {
    return -1;
  }
  return 1;
}

static mg_err_t node_exists(mg_storage_t *s, const mg_node_id_t id) {
  mg_node_t node;
  mg_err_t err = mg_storage_get_node(s, id, &node);
  if (err == MG_OK) mg_node_free(&node);
  return err;
}

static int same_source(const mg_source_t *a, const mg_source_t *b) {
  return !strcmp(a->project, b->project) && !strcmp(a->kind, b->kind) &&
         !strcmp(a->locator, b->locator);
}

mg_err_t mg_op_sources_list(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  char *project = NULL, *kind = NULL;
  mg_node_id_t id;
  int has_id;
  mg_source_link_t *links = NULL;
  size_t n = 0, i;
  mg_err_t err;

  if (!ctx || !ctx->storage || !result) return MG_ERR_INVALID_ARG;
  if (!mpack_node_is_missing(args) && !mpack_node_is_nil(args)) {
    if (opt_str(args, "project", &project) != 0 || opt_str(args, "kind", &kind) != 0) {
      free(project);
      free(kind);
      return MG_ERR_INVALID_ARG;
    }
    has_id = opt_node_id(args, id);
  } else {
    has_id = 0;
  }
  if (has_id < 0) {
    free(project);
    free(kind);
    return MG_ERR_INVALID_ARG;
  }
  err = has_id ? node_exists(ctx->storage, id) : MG_OK;
  if (err == MG_OK) {
    err = mg_storage_source_links(ctx->storage, project, kind,
                                  has_id ? (const mg_node_id_t *)&id : NULL, &links, &n);
  }
  free(project);
  free(kind);
  if (err != MG_OK) return err;

  mpack_build_map(result);
  mpack_write_cstr(result, "sources");
  mpack_build_array(result);
  for (i = 0; i < n; ++i) {
    const mg_source_t *src = &links[i].source;
    char id_hex[2 * MG_NODE_ID_BYTES + 1];
    if (i == 0 || !same_source(src, &links[i - 1].source)) {
      if (i > 0) {
        mpack_complete_array(result);  /* nodes */
        mpack_complete_map(result);    /* source */
      }
      mpack_build_map(result);
      mpack_write_cstr(result, "project"); mpack_write_cstr(result, src->project);
      mpack_write_cstr(result, "kind");    mpack_write_cstr(result, src->kind);
      mpack_write_cstr(result, "locator"); mpack_write_cstr(result, src->locator);
      mpack_write_cstr(result, "nodes");
      mpack_build_array(result);
    }
    mg_retrieve_hex_encode(links[i].node_id, MG_NODE_ID_BYTES, id_hex);
    mpack_build_map(result);
    mpack_write_cstr(result, "id_hex");      mpack_write_cstr(result, id_hex);
    mpack_write_cstr(result, "title");       mpack_write_cstr(result, links[i].title);
    mpack_write_cstr(result, "fingerprint"); write_opt_cstr(result, src->fingerprint);
    mpack_write_cstr(result, "observed_at"); mpack_write_int(result, src->observed_at);
    mpack_write_cstr(result, "role");        mpack_write_cstr(result, src->role);
    mpack_complete_map(result);
  }
  if (n > 0) {
    mpack_complete_array(result);
    mpack_complete_map(result);
  }
  mpack_complete_array(result);
  mpack_complete_map(result);
  mg_source_links_free(links, n);
  return MG_OK;
}

mg_err_t mg_op_sources_refresh(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
  mg_node_id_t id;
  mpack_node_t arr;
  size_t count, i;
  int64_t total = 0;
  int64_t now = now_unix_ms();
  char id_hex[2 * MG_NODE_ID_BYTES + 1];
  mg_err_t err;

  if (!ctx || !ctx->storage || !result) return MG_ERR_INVALID_ARG;
  if (opt_node_id(args, id) != 1) return MG_ERR_INVALID_ARG;
  arr = mpack_node_map_cstr_optional(args, "updates");
  if (mpack_node_type(arr) != mpack_type_array) return MG_ERR_INVALID_ARG;
  count = mpack_node_array_length(arr);
  if (count > MG_OP_MAX_SOURCES) return MG_ERR_INVALID_ARG;
  err = node_exists(ctx->storage, id);
  if (err != MG_OK) return err;

  for (i = 0; err == MG_OK && i < count; ++i) {
    mpack_node_t item = mpack_node_array_at(arr, i);
    mg_source_t src;
    int64_t updated = 0;
    memset(&src, 0, sizeof(src));
    if (mpack_node_type(item) != mpack_type_map) {
      err = MG_ERR_INVALID_ARG;
      break;
    }
    err = parse_source_map(item, &src);
    if (err == MG_OK && !src.fingerprint) err = MG_ERR_INVALID_ARG;
    if (err == MG_OK) {
      err = mg_storage_refresh_source(ctx->storage, id, src.project, src.kind, src.locator,
                                      src.fingerprint, now, &updated);
      total += updated;
    }
    free_source(&src);
  }
  if (err != MG_OK) return err;

  mg_retrieve_hex_encode(id, MG_NODE_ID_BYTES, id_hex);
  mpack_build_map(result);
  mpack_write_cstr(result, "id_hex");  mpack_write_cstr(result, id_hex);
  mpack_write_cstr(result, "updated"); mpack_write_int(result, total);
  mpack_complete_map(result);
  return MG_OK;
}
