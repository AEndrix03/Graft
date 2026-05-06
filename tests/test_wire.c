/* Wire-level smoke tests:
 *   1. op string <-> enum roundtrip for every defined op
 *   2. unknown op string is rejected
 *   3. NULL input is rejected
 *
 * Frame I/O is not covered here (would require a socketpair, which is
 * non-trivial cross-platform); manual end-to-end is via memgraphd.
 */

#include "memgraph/wire.h"
#include "memgraph/error.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int test_op_roundtrip(void) {
    static const mg_op_t ops[] = {
        MG_OP_CLASSIFY, MG_OP_INSERT, MG_OP_QUERY, MG_OP_RETRIEVE,
        MG_OP_EXPLORE,  MG_OP_GET,    MG_OP_STATS, MG_OP_CONSOLIDATE
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        const char *s = mg_wire_op_to_string(ops[i]);
        assert(s != NULL);
        mg_op_t back;
        assert(mg_wire_op_from_string(s, &back) == MG_OK);
        assert(back == ops[i]);
    }
    printf("ok op roundtrip\n");
    return 0;
}

static int test_op_invalid(void) {
    mg_op_t op;
    assert(mg_wire_op_from_string("bogus",   &op) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_from_string("",        &op) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_from_string("Insert",  &op) == MG_ERR_INVALID_ARG); /* case-sensitive */
    assert(mg_wire_op_from_string(NULL,      &op) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_from_string("insert",  NULL) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_to_string((mg_op_t)999) == NULL);
    printf("ok op invalid rejected\n");
    return 0;
}

static int test_frame_invalid_args(void) {
    /* len > 0 with NULL payload is rejected without touching the fd. */
    assert(mg_wire_write_frame(-1, NULL, 10) == MG_ERR_INVALID_ARG);
    /* read_frame requires non-NULL out pointers. */
    void *p; size_t n;
    assert(mg_wire_read_frame(-1, NULL, &n) == MG_ERR_INVALID_ARG);
    assert(mg_wire_read_frame(-1, &p,   NULL) == MG_ERR_INVALID_ARG);
    /* Frame length over the cap should reject before any I/O. */
    char dummy[1] = {0};
    assert(mg_wire_write_frame(-1, dummy, (size_t)17 * 1024 * 1024) == MG_ERR_INVALID_ARG);
    printf("ok wire arg validation\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_op_roundtrip();
    rc |= test_op_invalid();
    rc |= test_frame_invalid_args();
    if (rc == 0) printf("test_wire: PASS\n");
    return rc;
}
