/* Utility-level smoke tests: UUIDv7 and BLAKE3.
 * These cover the two primitives used everywhere before any storage or embed
 * code is reachable, so they're the first line of defence for environment
 * correctness. */

#include "graft/types.h"

#include <stdio.h>
#include <string.h>

static int test_uuidv7_unique(void) {
    mg_node_id_t a, b, c;
    mg_uuidv7(a);
    mg_uuidv7(b);
    mg_uuidv7(c);
    if (memcmp(a, b, MG_NODE_ID_BYTES) == 0 ||
        memcmp(a, c, MG_NODE_ID_BYTES) == 0 ||
        memcmp(b, c, MG_NODE_ID_BYTES) == 0) {
        fprintf(stderr, "uuidv7: duplicate IDs generated\n");
        return 1;
    }
    printf("ok uuidv7 unique\n");
    return 0;
}

static int test_uuidv7_version(void) {
    mg_node_id_t id;
    mg_uuidv7(id);
    /* RFC 9562 §5.7: version nibble = 7 at bits 76-79 (byte 6, high nibble) */
    if ((id[6] >> 4) != 7) {
        fprintf(stderr, "uuidv7: version nibble 0x%x, expected 7\n", id[6] >> 4);
        return 1;
    }
    /* RFC 9562 §4.1: variant = 10xx at bits 64-65 (byte 8, two high bits) */
    if ((id[8] >> 6) != 2) {
        fprintf(stderr, "uuidv7: variant bits 0x%x, expected 2 (10xx)\n", id[8] >> 6);
        return 1;
    }
    printf("ok uuidv7 version/variant bits\n");
    return 0;
}

static int test_blake3_deterministic(void) {
    const uint8_t data[] = "hello graft";
    mg_hash_t h1, h2;
    mg_blake3(data, sizeof(data) - 1, h1);
    mg_blake3(data, sizeof(data) - 1, h2);
    if (memcmp(h1, h2, MG_HASH_BYTES) != 0) {
        fprintf(stderr, "blake3: same input produced different hashes\n");
        return 1;
    }
    printf("ok blake3 deterministic\n");
    return 0;
}

static int test_blake3_distinct(void) {
    const uint8_t a[] = "hello";
    const uint8_t b[] = "world";
    mg_hash_t ha, hb;
    mg_blake3(a, sizeof(a) - 1, ha);
    mg_blake3(b, sizeof(b) - 1, hb);
    if (memcmp(ha, hb, MG_HASH_BYTES) == 0) {
        fprintf(stderr, "blake3: different inputs produced the same hash\n");
        return 1;
    }
    printf("ok blake3 collision-free\n");
    return 0;
}

static int test_blake3_non_zero(void) {
    const uint8_t data[] = "graft memory graph";
    mg_hash_t h;
    int all_zero = 1;
    int i;
    mg_blake3(data, sizeof(data) - 1, h);
    for (i = 0; i < MG_HASH_BYTES; i++) {
        if (h[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero) {
        fprintf(stderr, "blake3: output is all zeros\n");
        return 1;
    }
    printf("ok blake3 non-zero output\n");
    return 0;
}

static int test_blake3_empty_input(void) {
    /* Empty input must produce a valid (non-zero) hash without crashing. */
    mg_hash_t h, h2;
    int all_zero = 1;
    int i;
    mg_blake3((const uint8_t *)"", 0, h);
    mg_blake3((const uint8_t *)"", 0, h2);
    for (i = 0; i < MG_HASH_BYTES; i++) {
        if (h[i] != 0) { all_zero = 0; }
    }
    if (all_zero) {
        fprintf(stderr, "blake3: empty-input hash is all zeros\n");
        return 1;
    }
    if (memcmp(h, h2, MG_HASH_BYTES) != 0) {
        fprintf(stderr, "blake3: empty-input hash is not deterministic\n");
        return 1;
    }
    printf("ok blake3 empty input\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_uuidv7_unique();
    rc |= test_uuidv7_version();
    rc |= test_blake3_deterministic();
    rc |= test_blake3_distinct();
    rc |= test_blake3_non_zero();
    rc |= test_blake3_empty_input();
    if (rc == 0) printf("test_smoke: PASS\n");
    return rc;
}
