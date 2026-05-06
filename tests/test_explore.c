/* Smoke test for explore/stats/consolidate handlers.
 *
 * Storage/embed are unimplemented at this point, so we exercise only:
 *   - NULL ctx rejection on all three handlers
 *   - the consolidate handler end-to-end (it doesn't touch any subsystem),
 *     checking the produced map matches the spec.
 */

#include "memgraph/ops.h"
#include "memgraph/error.h"
#include "memgraph/types.h"
#include "mpack.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mpack_node_t make_nil_node(mpack_tree_t *tree) {
    static const char nil_bytes[] = { (char)0xc0 };
    mpack_tree_init_data(tree, nil_bytes, sizeof(nil_bytes));
    mpack_tree_parse(tree);
    return mpack_tree_root(tree);
}

static int test_null_ctx(void) {
    char buf[256];
    mpack_writer_t w;
    mpack_tree_t   tree;
    mpack_node_t   args = make_nil_node(&tree);

    mpack_writer_init(&w, buf, sizeof(buf));
    assert(mg_op_explore(NULL, args, &w) == MG_ERR_INVALID_ARG);
    (void)mpack_writer_destroy(&w);

    mpack_writer_init(&w, buf, sizeof(buf));
    assert(mg_op_stats(NULL, args, &w) == MG_ERR_INVALID_ARG);
    (void)mpack_writer_destroy(&w);

    mpack_writer_init(&w, buf, sizeof(buf));
    assert(mg_op_consolidate(NULL, args, &w) == MG_ERR_INVALID_ARG);
    (void)mpack_writer_destroy(&w);

    mpack_tree_destroy(&tree);
    printf("ok null ctx rejected\n");
    return 0;
}

static int test_consolidate_output(void) {
    /* consolidate touches nothing; pass an otherwise-empty ctx. */
    mg_ctx_t ctx = (mg_ctx_t){0};

    char buf[1024];
    mpack_writer_t w;
    mpack_writer_init(&w, buf, sizeof(buf));

    mpack_tree_t tree;
    mpack_node_t args = make_nil_node(&tree);

    mg_err_t err = mg_op_consolidate(&ctx, args, &w);
    assert(err == MG_OK);

    size_t used = mpack_writer_buffer_used(&w);
    mpack_error_t we = mpack_writer_destroy(&w);
    assert(we == mpack_ok);
    mpack_tree_destroy(&tree);

    /* Parse the produced map and assert keys/values. */
    mpack_tree_t out;
    mpack_tree_init_data(&out, buf, used);
    mpack_tree_parse(&out);
    assert(mpack_tree_error(&out) == mpack_ok);

    mpack_node_t root = mpack_tree_root(&out);
    assert(mpack_node_int(mpack_node_map_cstr(root, "deduped")) == 0);
    assert(mpack_node_int(mpack_node_map_cstr(root, "contradictions_found")) == 0);
    assert(mpack_node_int(mpack_node_map_cstr(root, "stale_marked")) == 0);

    mpack_node_t note = mpack_node_map_cstr(root, "note");
    const char *note_str = mpack_node_str(note);
    size_t      note_len = mpack_node_strlen(note);
    assert(note_str != NULL);
    assert(note_len == strlen("stub MVP, not implemented"));
    assert(memcmp(note_str, "stub MVP, not implemented", note_len) == 0);

    mpack_tree_destroy(&out);
    printf("ok consolidate output\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_null_ctx();
    rc |= test_consolidate_output();
    if (rc == 0) printf("test_explore: PASS\n");
    return rc;
}
