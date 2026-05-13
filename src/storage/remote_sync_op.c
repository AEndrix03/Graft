/* mg_op_remote_sync: pull-then-push a profile against a remote SQLite file.
 *
 * Args map:
 *   { "url": "<path-to-remote-sqlite-file>" }
 *
 * Result map:
 *   { "pulled": int, "deleted": int, "pushed": int }
 *
 * Running this op from inside the daemon avoids the "stop the daemon first"
 * dance that the standalone CLI sync used to require: the local handle is
 * already open and SQLite's WAL serializes the pull's writes against other
 * client threads. The remote file is opened transiently as a second SQLite
 * connection for the push half (mg_storage_merge_from) and closed again.
 */

#include "graft/ops.h"
#include "graft/storage.h"
#include "graft/config.h"
#include "graft/error.h"
#include "mpack.h"

#include <stdlib.h>

mg_err_t mg_op_remote_sync(mg_ctx_t *ctx, mpack_node_t args, mpack_writer_t *result) {
    if (!ctx || !ctx->storage || !ctx->config || !result) return MG_ERR_INVALID_ARG;

    mpack_node_t url_node = mpack_node_map_cstr(args, "url");
    if (mpack_node_type(url_node) != mpack_type_str) return MG_ERR_INVALID_ARG;
    char *url = mpack_node_cstr_alloc(url_node, 4096);
    if (!url || !*url) { free(url); return MG_ERR_INVALID_ARG; }

    int64_t pulled = 0, deleted = 0, pushed = 0;

    mg_err_t err = mg_storage_pull_remote_file(ctx->storage, url, &pulled, &deleted);
    if (err != MG_OK) { free(url); return err; }

    mg_storage_t *remote = NULL;
    err = mg_storage_open(url, &remote);
    if (err == MG_OK) err = mg_storage_apply_schema(remote);
    if (err == MG_OK) err = mg_storage_merge_from(remote, ctx->config->db_path, 0);
    if (remote) mg_storage_close(remote);
    if (err != MG_OK) { free(url); return err; }

    err = mg_storage_mark_local_pushed(ctx->storage, &pushed);
    if (err != MG_OK) { free(url); return err; }

    free(url);

    mpack_build_map(result);
    mpack_write_cstr(result, "pulled");  mpack_write_int(result, pulled);
    mpack_write_cstr(result, "deleted"); mpack_write_int(result, deleted);
    mpack_write_cstr(result, "pushed");  mpack_write_int(result, pushed);
    mpack_complete_map(result);

    return MG_OK;
}
