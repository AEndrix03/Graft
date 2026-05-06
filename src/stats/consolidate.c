/* mg_op_consolidate — STUB for MVP.
 *
 * Real consolidation (dedup similar nodes, mark contradictions, stale-mark
 * untouched nodes) is deferred to v2. The handler always succeeds and
 * returns a fixed map indicating no work was done.
 *
 * Result map (handler writes a single mpack VALUE):
 *   {
 *     "deduped":              0,
 *     "contradictions_found": 0,
 *     "stale_marked":         0,
 *     "note":                 "stub MVP, not implemented"
 *   }
 */

#include "memgraph/ops.h"
#include "memgraph/error.h"
#include "mpack.h"

mg_err_t mg_op_consolidate(mg_ctx_t *ctx, mpack_node_t args,
                           mpack_writer_t *result) {
    (void)args;
    if (!ctx || !result) return MG_ERR_INVALID_ARG;

    mpack_build_map(result);

    mpack_write_cstr(result, "deduped");
    mpack_write_int(result, 0);

    mpack_write_cstr(result, "contradictions_found");
    mpack_write_int(result, 0);

    mpack_write_cstr(result, "stale_marked");
    mpack_write_int(result, 0);

    mpack_write_cstr(result, "note");
    mpack_write_cstr(result, "stub MVP, not implemented");

    mpack_complete_map(result);
    return MG_OK;
}
