#ifndef GRAFT_MAINTAIN_H
#define GRAFT_MAINTAIN_H

/* Autonomous maintenance (issue #5).
 *
 * Graft applies the mechanical part itself (apply-safe) and reports the
 * semantic part as candidates (scan) that the integrated agent adjudicates
 * against the current code / docs and resolves with one narrow action.
 * Everything below is storage: the scan heuristics and the wire ops live in
 * src/maintain/maintain.c. Every public function takes the storage mutex
 * like the rest of mg_storage_*. */

#include "graft/storage.h"

#define MG_MAINT_ID_HEX 16   /* candidate ids and evidence digests */

/* maintenance_meta keys */
#define MG_MAINT_META_LAST_APPLY_SAFE "last_apply_safe_at"
#define MG_MAINT_META_LAST_SCAN       "last_scan_at"

/* Candidate kinds, in priority order (most urgent first). */
typedef enum {
  MG_MAINT_CONTRADICTION = 0,
  MG_MAINT_POSSIBLE_CONTRADICTION,
  MG_MAINT_SOURCE_REMOVED,
  MG_MAINT_SOURCE_CHANGED,
  MG_MAINT_POSSIBLE_SUPERSESSION,
  MG_MAINT_NEAR_DUPLICATE,
  MG_MAINT_KEYWORD_FRAGMENTATION,
  MG_MAINT_ISOLATED_LOW_VALUE,
  MG_MAINT_N_KINDS
} mg_maint_kind_t;

const char *mg_maint_kind_name(mg_maint_kind_t k);
/* -1 when name is not a kind */
int         mg_maint_kind_from_name(const char *name);

/* One audit-log row to write (all strings optional but actor / action). */
typedef struct {
  int64_t     ts;
  const char *actor;
  const char *action;
  const char *candidate_id;
  const char *kind;
  const char *nodes;     /* comma-separated hex ids */
  const char *detail;
  const char *note;
} mg_maint_log_t;

/* One audit-log row read back. Free arrays with mg_maint_log_rows_free. */
typedef struct {
  int64_t id;
  int64_t ts;
  char   *actor;
  char   *action;
  char   *candidate_id;
  char   *kind;
  char   *nodes;
  char   *detail;
  char   *note;
} mg_maint_log_row_t;

/* A cached candidate: the map the scan emitted (MessagePack in payload). */
typedef struct {
  char    id[MG_MAINT_ID_HEX + 1];
  char    kind[32];
  int     priority;
  double  score;
  char   *nodes;                            /* comma-separated hex ids */
  char    evidence[MG_MAINT_ID_HEX + 1];
  void   *payload;
  size_t  payload_len;
} mg_maint_candidate_t;

typedef struct {
  mg_node_id_t id;
  int64_t      created_at;
  float        emb[MG_EMBEDDING_DIM];       /* L2-normalized */
} mg_maint_vec_t;

typedef struct {
  mg_node_id_t a;
  mg_node_id_t b;
  float        weight;
} mg_maint_pair_t;

typedef struct {
  mg_keyword_id_t id;
  char           *text;
  int64_t         uses;                     /* ACTIVE nodes carrying it */
} mg_maint_keyword_t;

/* State transitions a resolution may apply. */
typedef enum {
  MG_MAINT_TO_STALE = 1,   /* ACTIVE -> STALE */
  MG_MAINT_TO_RETIRED,     /* any but RETIRED -> RETIRED (soft delete) */
  MG_MAINT_TO_ACTIVE,      /* STALE / SUPERSEDED / RETIRED -> ACTIVE */
  MG_MAINT_TO_SUPERSEDED,  /* ACTIVE / STALE -> SUPERSEDED by `by` */
  MG_MAINT_LINK_CONTRADICTS /* no state change: a CONTRADICTS edge node -> by,
                               both ACTIVE / STALE */
} mg_maint_transition_t;

typedef struct {
  mg_maint_transition_t to;
  mg_node_id_t          node;
  mg_node_id_t          by;                 /* TO_SUPERSEDED / LINK_CONTRADICTS */
} mg_maint_change_t;

typedef struct {
  int64_t last_apply_safe_at;               /* 0 = never */
  int64_t last_scan_at;                     /* 0 = never */
  int64_t inserts_since_apply_safe;
  int64_t inserts_since_scan;
  int64_t n_active;
  int64_t n_stale;
  int64_t n_superseded;
  int64_t n_retired;
  int64_t retired_due;                      /* retired before the cutoff */
  int64_t pending_total;
  int64_t pending[MG_MAINT_N_KINDS];
} mg_maint_status_t;

/* === meta === */
mg_err_t mg_storage_maint_meta_get(mg_storage_t *s, const char *key, int64_t *out);
mg_err_t mg_storage_maint_meta_set(mg_storage_t *s, const char *key, int64_t value);

/* === scan inputs (read-only) === */
/* The newest max_nodes ACTIVE, unexpired nodes with their embeddings. */
mg_err_t mg_storage_maint_active_vectors(mg_storage_t *s, size_t max_nodes,
                                         mg_maint_vec_t **out, size_t *n);
/* CONTRADICTS edges whose two ends are ACTIVE, one row per unordered pair. */
mg_err_t mg_storage_maint_contradictions(mg_storage_t *s, size_t limit,
                                         mg_maint_pair_t **out, size_t *n);
/* ACTIVE nodes with no edge, never accessed, created before `before`. */
mg_err_t mg_storage_maint_isolated(mg_storage_t *s, int64_t before, size_t limit,
                                   mg_node_id_t **out, size_t *n);
/* Every keyword carried by at least one ACTIVE node. */
mg_err_t mg_storage_maint_keyword_usage(mg_storage_t *s, mg_maint_keyword_t **out, size_t *n);
void     mg_maint_keywords_free(mg_maint_keyword_t *kw, size_t n);
/* ACTIVE nodes carrying keyword kw (at most max). */
mg_err_t mg_storage_maint_keyword_nodes(mg_storage_t *s, mg_keyword_id_t kw,
                                        mg_node_id_t *out, size_t max, size_t *n);
/* *out = 1 when a CONTRADICTS edge joins a and b, in either direction and
 * whatever their state. */
mg_err_t mg_storage_maint_has_contradicts(mg_storage_t *s, const mg_node_id_t a,
                                          const mg_node_id_t b, int *out);
mg_err_t mg_storage_maint_is_dismissed(mg_storage_t *s, const char *candidate_id,
                                       const char *evidence, int *out);

/* === candidate cache === */
/* Replaces the cache with rows and stamps MG_MAINT_META_LAST_SCAN = now. */
mg_err_t mg_storage_maint_store_candidates(mg_storage_t *s, const mg_maint_candidate_t *rows,
                                           size_t n, int64_t now);
/* Up to limit cached candidates, priority order. */
mg_err_t mg_storage_maint_list_candidates(mg_storage_t *s, size_t limit,
                                          mg_maint_candidate_t **out, size_t *n);
mg_err_t mg_storage_maint_get_candidate(mg_storage_t *s, const char *id,
                                        mg_maint_candidate_t *out);
void     mg_maint_candidate_clear(mg_maint_candidate_t *c);
void     mg_maint_candidates_free(mg_maint_candidate_t *c, size_t n);

/* === resolution === */
/* One transaction: applies the transitions, records the dismissal
 * (dismiss_id / dismiss_kind / dismiss_evidence, all or none), drops the
 * cached candidate drop_id and every cached candidate naming a node whose
 * state changed (a LINK_CONTRADICTS change touches no state),
 * and appends the audit row. MG_ERR_NOT_FOUND for a missing node,
 * MG_ERR_INVALID_ARG for a transition the node's state does not allow. */
mg_err_t mg_storage_maint_apply(mg_storage_t *s,
                                const mg_maint_change_t *changes, size_t n_changes,
                                const char *dismiss_id, const char *dismiss_kind,
                                const char *dismiss_evidence, const char *drop_id,
                                const mg_maint_log_t *log);

/* === apply-safe steps === */
/* Physically deletes RETIRED nodes retired at or before cutoff (a retired
 * node with no record is stamped now, starting its window). One audit row
 * per purged node. */
mg_err_t mg_storage_maint_purge_retired(mg_storage_t *s, int64_t cutoff, int64_t now,
                                        const char *actor, int64_t *purged);
/* Exact duplicates: ACTIVE / STALE nodes with byte-identical title and body
 * (content_hash already folds identical keyword sets, so these differ only
 * in keywords). The oldest is kept; every other one is SUPERSEDED by it and
 * its provenance links are copied to the keeper. Reversible with restore. */
mg_err_t mg_storage_maint_collapse_exact(mg_storage_t *s, int64_t now, const char *actor,
                                         int64_t *collapsed);
/* Drops cached candidates naming a node that is gone or no longer ACTIVE. */
mg_err_t mg_storage_maint_prune_candidates(mg_storage_t *s, int64_t *dropped);
mg_err_t mg_storage_maint_log_append(mg_storage_t *s, const mg_maint_log_t *log);
/* Newest first; node_hex filters on rows naming that node (NULL = all). */
mg_err_t mg_storage_maint_log_list(mg_storage_t *s, size_t limit, const char *node_hex,
                                   mg_maint_log_row_t **out, size_t *n);
void     mg_maint_log_rows_free(mg_maint_log_row_t *rows, size_t n);

/* Cheap counters: indexed counts and the candidate cache, no embedding. */
mg_err_t mg_storage_maint_status(mg_storage_t *s, int64_t retired_cutoff,
                                 mg_maint_status_t *out);

#endif
