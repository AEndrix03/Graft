/* Wire-level smoke tests:
 *   1. op string <-> enum roundtrip for EVERY defined op (including DELETE/VIEW/REMOTE_SYNC)
 *   2. canonical wire strings checked explicitly
 *   3. unknown op string is rejected
 *   4. NULL input is rejected
 *
 * Frame I/O is not covered here (would require a socketpair, which is
 * non-trivial cross-platform); manual end-to-end is via graftd.
 */

#include "graft/wire.h"
#include "graft/error.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int test_op_roundtrip(void) {
    /* All ops defined in mg_op_t must have a wire string and round-trip. */
    static const mg_op_t ops[] = {
        MG_OP_CLASSIFY,  MG_OP_INSERT,      MG_OP_QUERY,  MG_OP_RETRIEVE,
        MG_OP_EXPLORE,   MG_OP_GET,         MG_OP_STATS,  MG_OP_CONSOLIDATE,
        MG_OP_DELETE,    MG_OP_VIEW,        MG_OP_REMOTE_SYNC
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

static int test_op_known_strings(void) {
    /* Canonical wire strings — renaming an op must break this test so the
     * protocol change is visible at review time. */
    assert(strcmp(mg_wire_op_to_string(MG_OP_CLASSIFY),    "classify")    == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_INSERT),      "insert")      == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_QUERY),       "query")       == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_RETRIEVE),    "retrieve")    == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_EXPLORE),     "explore")     == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_GET),         "get")         == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_STATS),       "stats")       == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_CONSOLIDATE), "consolidate") == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_DELETE),      "delete")      == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_VIEW),        "view")        == 0);
    assert(strcmp(mg_wire_op_to_string(MG_OP_REMOTE_SYNC), "remote_sync") == 0);
    printf("ok op known strings\n");
    return 0;
}

static int test_op_invalid(void) {
    mg_op_t op;
    assert(mg_wire_op_from_string("bogus",       &op) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_from_string("",            &op) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_from_string("Insert",      &op) == MG_ERR_INVALID_ARG); /* case-sensitive */
    assert(mg_wire_op_from_string("DELETE",      &op) == MG_ERR_INVALID_ARG); /* case-sensitive */
    assert(mg_wire_op_from_string("Remote_Sync", &op) == MG_ERR_INVALID_ARG); /* case-sensitive */
    assert(mg_wire_op_from_string(NULL,          &op) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_from_string("insert",      NULL) == MG_ERR_INVALID_ARG);
    assert(mg_wire_op_to_string((mg_op_t)999) == NULL);
    assert(mg_wire_op_to_string((mg_op_t)0)   == NULL); /* 0 is not a valid op value */
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
    rc |= test_op_known_strings();
    rc |= test_op_invalid();
    rc |= test_frame_invalid_args();
    if (rc == 0) printf("test_wire: PASS\n");
    return rc;
}
