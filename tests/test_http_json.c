/* mpack -> JSON conversion used at the HTTP boundary. Every REST response
 * goes through mg_http_mpack_to_json, so a wrong escape here breaks every
 * JSON consumer (viewer, curl | jq, microservices). */

#include "../src/http/internal.h"
#include "mpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_http_json: %s\n", msg);
    g_failures++;
  }
}

typedef void (*build_fn)(mpack_writer_t *w);

/* Encode with `build`, convert, and compare with the expected JSON text. */
static void expect_json(build_fn build, const char *want, const char *msg) {
  char *buf = NULL, *json = NULL;
  size_t len = 0, json_len = 0;
  mpack_writer_t w;
  mpack_writer_init_growable(&w, &buf, &len);
  build(&w);
  if (mpack_writer_destroy(&w) != mpack_ok) {
    expect(0, "mpack encoding failed");
    return;
  }
  if (mg_http_mpack_to_json(buf, len, &json, &json_len) != MG_OK) {
    expect(0, msg);
  } else if (strcmp(json, want) != 0 || json_len != strlen(want)) {
    fprintf(stderr, "test_http_json: %s\n  got:  %s\n  want: %s\n", msg, json, want);
    g_failures++;
  }
  free(json);
  free(buf);
}

/* --- builders --- */

static void build_scalars(mpack_writer_t *w) {
  mpack_start_array(w, 8);
  mpack_write_nil(w);
  mpack_write_true(w);
  mpack_write_false(w);
  mpack_write_i64(w, -42);
  mpack_write_u64(w, 18446744073709551615ULL);
  mpack_write_double(w, 0.25);
  mpack_write_float(w, 1.5f);
  mpack_write_cstr(w, "plain");
  mpack_finish_array(w);
}

static void build_response_shape(mpack_writer_t *w) {
  /* The {status, result} root that mg_dispatch produces. */
  mpack_start_map(w, 2);
  mpack_write_cstr(w, "status");
  mpack_write_cstr(w, "ok");
  mpack_write_cstr(w, "result");
  mpack_start_map(w, 2);
  mpack_write_cstr(w, "hits");
  mpack_start_array(w, 2);
  mpack_start_map(w, 1);
  mpack_write_cstr(w, "title");
  mpack_write_cstr(w, "a");
  mpack_finish_map(w);
  mpack_start_map(w, 0);
  mpack_finish_map(w);
  mpack_finish_array(w);
  mpack_write_cstr(w, "empty");
  mpack_start_array(w, 0);
  mpack_finish_array(w);
  mpack_finish_map(w);
  mpack_finish_map(w);
}

static void build_escapes(mpack_writer_t *w) {
  static const char s[] = "q\"b\\n\nr\rt\tb\bf\fc\x01" "C:\\Users";
  mpack_write_str(w, s, (uint32_t)(sizeof(s) - 1));
}

static void build_utf8(mpack_writer_t *w) {
  mpack_write_cstr(w, "caff\xC3\xA8 \xE2\x9C\x93");
}

static void build_embedded_nul(mpack_writer_t *w) {
  mpack_write_str(w, "a\0b", 3);
}

static void build_bin(mpack_writer_t *w) {
  mpack_start_map(w, 1);
  mpack_write_cstr(w, "embedding");
  mpack_write_bin(w, "\x00\x01\x02", 3);
  mpack_finish_map(w);
}

static void build_non_string_key(mpack_writer_t *w) {
  mpack_start_map(w, 1);
  mpack_write_u64(w, 7);
  mpack_write_true(w);
  mpack_finish_map(w);
}

/* --- tests --- */

static void test_conversions(void) {
  expect_json(build_scalars,
              "[null,true,false,-42,18446744073709551615,0.25,1.5,\"plain\"]",
              "scalars");
  expect_json(build_response_shape,
              "{\"status\":\"ok\",\"result\":{\"hits\":[{\"title\":\"a\"},{}],\"empty\":[]}}",
              "nested maps and arrays");
  expect_json(build_escapes,
              "\"q\\\"b\\\\n\\nr\\rt\\tb\\bf\\fc\\u0001C:\\\\Users\"",
              "JSON escapes, control chars and Windows paths");
  expect_json(build_utf8, "\"caff\xC3\xA8 \xE2\x9C\x93\"", "UTF-8 passes through unchanged");
  expect_json(build_embedded_nul, "\"a\\u0000b\"", "embedded NUL is escaped, not truncated");
  expect_json(build_bin, "{\"embedding\":null}", "binary values become null");
  expect_json(build_non_string_key, "{7:true}", "non-string keys are emitted as-is");
  printf("ok conversions\n");
}

static void test_errors(void) {
  static const char truncated[] = { (char)0x92, (char)0xc0 };  /* array(2) with one item */
  char *json = (char *)"untouched";
  size_t len = 99;
  expect(mg_http_mpack_to_json(truncated, sizeof(truncated), &json, &len) == MG_ERR_INTERNAL,
         "truncated mpack is rejected");
  expect(json == NULL && len == 0, "outputs are cleared on error");
  expect(mg_http_mpack_to_json(NULL, 1, &json, &len) == MG_ERR_INVALID_ARG, "NULL input");
  expect(mg_http_mpack_to_json(truncated, 1, NULL, &len) == MG_ERR_INVALID_ARG, "NULL out");
  expect(mg_http_mpack_to_json(truncated, 1, &json, NULL) == MG_ERR_INVALID_ARG, "NULL out_len");
  printf("ok errors\n");
}

int main(void) {
  test_conversions();
  test_errors();
  if (g_failures) {
    fprintf(stderr, "test_http_json: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
