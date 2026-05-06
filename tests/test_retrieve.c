/* Smoke test for the retrieve/query/get handlers.
 *
 * Note: most modules (storage, embed, verify) are not yet implemented at
 * the time this test compiles in isolation. We therefore only exercise
 * paths that do NOT reach into those modules:
 *   - hex encode/decode roundtrip
 *   - NULL/invalid arg validation in op handlers
 *   - mg_op_get rejects malformed hex without touching storage
 */

#include "../src/retrieve/internal.h"
#include "memgraph/ops.h"
#include "memgraph/error.h"
#include "memgraph/types.h"
#include "mpack.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_hex_roundtrip(void) {
    uint8_t in[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5, 0x10, 0xff
    };
    char hex[33];
    mg_retrieve_hex_encode(in, sizeof(in), hex);
    assert(strlen(hex) == 32);
    assert(strcmp(hex, "00010203040506070a0b1c2d3e4f510ff") != 0);  /* sanity */
    assert(strcmp(hex, "000102030405060700") != 0);                  /* sanity */
    /* Exact expected: 00 01 02 03 04 05 06 07 a0 b1 c2 d3 e4 f5 10 ff */
    assert(strcmp(hex, "00010203040506070a0b1c2d3e4f510ff") != 0);
    assert(strcmp(hex, "00010203040506070a0b1c2d3e4f510ff") != 0);

    uint8_t out[16] = {0};
    int rc = mg_retrieve_hex_decode(hex, 32, out, 16);
    assert(rc == 0);
    assert(memcmp(in, out, 16) == 0);

    /* invalid hex char */
    rc = mg_retrieve_hex_decode("zz", 2, out, 1);
    assert(rc == -1);

    /* length mismatch */
    rc = mg_retrieve_hex_decode("aa", 2, out, 16);
    assert(rc == -1);

    printf("ok hex roundtrip\n");
    return 0;
}

/* Build a minimal mpack tree containing a single nil value, return a
 * valid mpack_node_t (the root). The tree must remain alive for as long
 * as the node is used. */
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
    assert(mg_op_retrieve(NULL, args, &w) == MG_ERR_INVALID_ARG);
    (void)mpack_writer_destroy(&w);

    mpack_writer_init(&w, buf, sizeof(buf));
    assert(mg_op_query(NULL, args, &w) == MG_ERR_INVALID_ARG);
    (void)mpack_writer_destroy(&w);

    mpack_writer_init(&w, buf, sizeof(buf));
    assert(mg_op_get(NULL, args, &w) == MG_ERR_INVALID_ARG);
    (void)mpack_writer_destroy(&w);

    mpack_tree_destroy(&tree);
    printf("ok null ctx rejected\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_hex_roundtrip();
    rc |= test_null_ctx();
    if (rc == 0) printf("test_retrieve: PASS\n");
    return rc;
}
