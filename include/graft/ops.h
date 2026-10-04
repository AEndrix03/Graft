#ifndef GRAFT_OPS_H
#define GRAFT_OPS_H

#include "graft/storage.h"
#include "graft/embed.h"
#include "graft/config.h"
#include "graft/wire.h"
#include "mpack.h"

typedef struct {
  mg_storage_t   *storage;
  mg_embed_ctx_t *embed;
  void           *verify;    /* mg_verify_ctx_t* opaque */
  void           *rerank;    /* mg_rerank_ctx_t* opaque, NULL when disabled */
  mg_config_t    *config;
} mg_ctx_t;

/* Ogni op handler legge args dalla mappa MessagePack del request,
 * scrive 'result' nella mappa MessagePack della response.
 * Ritorna mg_err_t. Se non MG_OK, il dispatch wrapper imposta status+error. */

mg_err_t mg_op_classify(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_insert(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_query(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_retrieve(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_explore(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_get(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_stats(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_consolidate(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_delete(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_view(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_remote_sync(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_sources_list(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);
mg_err_t mg_op_sources_refresh(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result);

/* Parses the optional "sources" array of an insert request into storage
 * records stamped with observed_at. Each element is either a map
 * { kind, locator, project?, fingerprint?, role? } (what the CLI sends after
 * resolving file paths itself) or a locator string such as "url:..." or
 * "file:/abs/path" resolved here (HTTP). Free with mg_op_sources_free. */
mg_err_t mg_op_parse_sources(mpack_node_t args, int64_t observed_at,
                             mg_source_t **out, size_t *out_count);
void     mg_op_sources_free(mg_source_t *sources, size_t count);

/* Writes a node's sources as one mpack array value:
 * [ { kind, locator, project, fingerprint|nil, role, observed_at }, ... ] */
mg_err_t mg_op_write_node_sources(mg_storage_t *s, const mg_node_id_t id,
                                  mpack_writer_t *w);

/* Dispatch: legge "op" dal frame, instrada all'handler giusto. */
mg_err_t mg_dispatch(mg_ctx_t *ctx, const void *req_payload, size_t req_len,
                     void **resp_payload, size_t *resp_len);

#endif
