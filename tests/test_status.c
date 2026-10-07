/* Zero-touch housekeeping (issue #6): the `graft status` plan, the
 * maintain_status reader, the report built without a daemon, and the
 * pieces behind `graft hook` (stdin JSON field extraction, JSON escaping,
 * prompt filter, injected texts). No daemon and no model. */

#include "../src/cli/status.h"
#include "../src/cli/hook.h"
#include "mpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define set_env(k, v) _putenv_s((k), (v))
#else
#  define set_env(k, v) setenv((k), (v), 1)
#endif

static int g_fail = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "test_status:%d: %s\n", __LINE__, msg); \
    g_fail++; \
  } \
} while (0)

/* ---------- plan ---------- */

static void test_plan_quiet(void) {
  mg_status_signals_t s;
  mg_status_step_t st[MG_STATUS_MAX_STEPS];
  memset(&s, 0, sizeof(s));
  CHECK(mg_status_plan(&s, st, MG_STATUS_MAX_STEPS) == 0, "nothing known: nothing to do");
  s.have_maint = 1;
  s.bootstrapped = 1;
  s.git = 1;
  CHECK(mg_status_plan(&s, st, MG_STATUS_MAX_STEPS) == 0, "healthy project: nothing to do");
  memset(&s, 0, sizeof(s));
  s.git = 0;   /* a plain folder (home, Downloads...) is not offered a bootstrap */
  CHECK(mg_status_plan(&s, st, MG_STATUS_MAX_STEPS) == 0, "no bootstrap outside a git checkout");
}

static void test_plan_bootstrap(void) {
  mg_status_signals_t s;
  mg_status_step_t st[MG_STATUS_MAX_STEPS];
  size_t n;
  memset(&s, 0, sizeof(s));
  s.git = 1;
  n = mg_status_plan(&s, st, MG_STATUS_MAX_STEPS);
  CHECK(n == 1 && !strcmp(st[0].action, "bootstrap") && st[0].priority == 2,
        "first encounter: one normal bootstrap step");
  CHECK(strstr(st[0].command, "/learn bootstrap") != NULL, "bootstrap command");

  s.bootstrapped = 1;
  s.pending_topics = 3;
  s.next_topic = "storage layer";
  n = mg_status_plan(&s, st, MG_STATUS_MAX_STEPS);
  CHECK(n == 1 && !strcmp(st[0].action, "bootstrap") && st[0].priority == 3,
        "pending topics: a low-priority continuation");
  CHECK(strstr(st[0].reason, "3 pending") && strstr(st[0].reason, "storage layer"),
        "reason names the count and the next topic");
}

static void test_plan_maintenance(void) {
  mg_status_signals_t s;
  mg_status_step_t st[MG_STATUS_MAX_STEPS];
  size_t n;
  memset(&s, 0, sizeof(s));
  s.git = 1;
  s.have_maint = 1;
  s.rec_apply_safe = 1;
  s.rec_scan = 1;
  s.inserts_since_apply_safe = 60;
  s.inserts_since_scan = 70;
  s.pending_total = 5;
  s.contradictions = 1;
  s.src_changed = 2;
  s.src_removed = 1;
  n = mg_status_plan(&s, st, MG_STATUS_MAX_STEPS);
  CHECK(n == 5, "apply-safe, resolve, refresh-sources, bootstrap, scan");
  if (n == 5) {
    CHECK(!strcmp(st[0].action, "apply-safe") && st[0].priority == 1, "apply-safe first");
    CHECK(!strcmp(st[1].action, "resolve") && st[1].priority == 1, "contradictions are urgent");
    CHECK(strstr(st[1].reason, "2 pending") != NULL, "resolve counts the non-source candidates");
    CHECK(!strcmp(st[2].action, "refresh-sources") && st[2].priority == 2, "sources next");
    CHECK(strstr(st[2].reason, "3 note") != NULL, "changed + removed");
    CHECK(!strcmp(st[3].action, "bootstrap"), "bootstrap after maintenance of equal priority");
    CHECK(!strcmp(st[4].action, "scan") && st[4].priority == 3, "scan last");
  }
  CHECK(mg_status_plan(&s, st, 2) == 2, "cap respected");

  s.contradictions = 0;
  s.src_changed = 0;
  s.src_removed = 0;
  s.rec_apply_safe = 0;
  s.rec_scan = 0;
  s.bootstrapped = 1;
  n = mg_status_plan(&s, st, MG_STATUS_MAX_STEPS);
  CHECK(n == 1 && !strcmp(st[0].action, "resolve") && st[0].priority == 2,
        "plain candidates: normal resolve");

  s.have_maint = 0;   /* the daemon did not answer: maintenance unknown */
  CHECK(mg_status_plan(&s, st, MG_STATUS_MAX_STEPS) == 0, "no maintenance step without the daemon");
}

/* ---------- maintain_status reader ---------- */

static void test_read_maint(void) {
  char *buf = NULL;
  size_t len = 0;
  mpack_writer_t w;
  mpack_tree_t tree;
  mg_status_signals_t s;
  mpack_writer_init_growable(&w, &buf, &len);
  mpack_build_map(&w);
  mpack_write_cstr(&w, "inserts_since_apply_safe"); mpack_write_int(&w, 51);
  mpack_write_cstr(&w, "inserts_since_scan");       mpack_write_int(&w, 7);
  mpack_write_cstr(&w, "pending");
  mpack_build_map(&w);
  mpack_write_cstr(&w, "total"); mpack_write_int(&w, 6);
  mpack_write_cstr(&w, "by_kind");
  mpack_build_map(&w);
  mpack_write_cstr(&w, "contradiction");  mpack_write_int(&w, 1);
  mpack_write_cstr(&w, "possible_contradiction"); mpack_write_int(&w, 2);
  mpack_write_cstr(&w, "source_removed"); mpack_write_int(&w, 1);
  mpack_write_cstr(&w, "source_changed"); mpack_write_int(&w, 2);
  mpack_complete_map(&w);
  mpack_complete_map(&w);
  mpack_write_cstr(&w, "recommended");
  mpack_build_array(&w);
  mpack_write_cstr(&w, "apply-safe");
  mpack_write_cstr(&w, "resolve");
  mpack_complete_array(&w);
  mpack_complete_map(&w);
  CHECK(mpack_writer_destroy(&w) == mpack_ok, "encode");

  memset(&s, 0, sizeof(s));
  mpack_tree_init_data(&tree, buf, len);
  mpack_tree_parse(&tree);
  mg_status_read_maint(mpack_tree_root(&tree), &s);
  CHECK(s.have_maint == 1, "have_maint");
  CHECK(s.inserts_since_apply_safe == 51 && s.inserts_since_scan == 7, "insert counters");
  CHECK(s.pending_total == 6 && s.contradictions == 3, "pending, possible contradictions counted");
  CHECK(s.src_changed == 2 && s.src_removed == 1, "source kinds");
  CHECK(s.rec_apply_safe == 1 && s.rec_scan == 0, "recommended");
  mpack_tree_destroy(&tree);
  free(buf);
}

/* ---------- gather + report, no daemon ---------- */

static int has_key(mpack_node_t map, const char *key) {
  return !mpack_node_is_missing(mpack_node_map_cstr_optional(map, key));
}

static void test_report_offline(void) {
  static mg_status_t s;
  char *out = NULL;
  size_t len = 0;
  mpack_tree_t tree;
  set_env("GRAFT_DB_PATH", "status_test.db");   /* never created: counts 0 */
  CHECK(mg_status_gather(".", 0, &s) == 0, "gather the cwd without the daemon");
  CHECK(s.maint_resp == NULL && s.maint_error != NULL, "maintenance not asked");
  CHECK(mg_status_report(&s, &out, &len) == 0, "report");
  mpack_tree_init_data(&tree, out, len);
  mpack_tree_parse(&tree);
  CHECK(mpack_tree_error(&tree) == mpack_ok, "report parses");
  if (mpack_tree_error(&tree) == mpack_ok) {
    mpack_node_t r = mpack_node_map_cstr(mpack_tree_root(&tree), "result");
    mpack_node_t p = mpack_node_map_cstr_optional(r, "project");
    CHECK(has_key(r, "project") && has_key(r, "next") && has_key(r, "sources"), "top-level keys");
    CHECK(mpack_node_is_nil(mpack_node_map_cstr_optional(r, "maintenance")), "maintenance null");
    CHECK(has_key(r, "maintenance_error"), "with a reason");
    CHECK(has_key(p, "bootstrapped") && has_key(p, "topics") && has_key(p, "provenance"),
          "project keys");
    CHECK(mpack_node_type(mpack_node_map_cstr_optional(r, "next")) == mpack_type_array, "next[]");
    CHECK(!strncmp(mpack_node_str(mpack_node_map_cstr(r, "daemon")), "not checked", 11),
          "daemon not checked");
  }
  mpack_tree_destroy(&tree);
  free(out);
  mg_status_free(&s);
  CHECK(mg_status_gather("no/such/dir/here", 0, &s) == -1, "missing dir");
  mg_status_free(&s);
}

/* ---------- hook pieces ---------- */

static void test_json_string(void) {
  char *v = NULL;
  const char *j = "{\"session_id\":\"abc\",\"nested\":{\"prompt\":\"inner\",\"a\":[1,-2.5e3,true,"
                  "null,{\"x\":\"]}\"}]},\"prompt\":\"fix \\\"it\\\"\\n\\u00e8\\ud83d\\ude00\\/ok\","
                  "\"n\":12}";
  CHECK(mg_hook_json_string(j, strlen(j), "prompt", &v) == 0, "top-level prompt found");
  CHECK(v && !strcmp(v, "fix \"it\"\n\xc3\xa8\xf0\x9f\x98\x80/ok"),
        "escapes, \\u and surrogate pairs decoded; nested key ignored");
  free(v);
  CHECK(mg_hook_json_string(j, strlen(j), "session_id", &v) == 0 && v && !strcmp(v, "abc"), "first key");
  free(v);
  CHECK(mg_hook_json_string(j, strlen(j), "cwd", &v) == 1 && v == NULL, "absent key");
  CHECK(mg_hook_json_string(j, strlen(j), "n", &v) == 1, "non-string value");
  CHECK(mg_hook_json_string("{}", 2, "prompt", &v) == 1, "empty object");
  CHECK(mg_hook_json_string("", 0, "prompt", &v) == -1, "empty input");
  CHECK(mg_hook_json_string("[1]", 3, "prompt", &v) == -1, "not an object");
  CHECK(mg_hook_json_string("{\"a\":\"x", 7, "a", &v) == -1, "unterminated string");
  CHECK(mg_hook_json_string("{\"a\":1,\"prompt\":\"\\q\"}", 21, "prompt", &v) == -1, "bad escape");
  CHECK(mg_hook_json_string("{\"a\" 1}", 7, "prompt", &v) == -1, "missing colon");
  {
    const char *k = " { \"cwd\" : \"C:\\\\repo\\\\x\" } ";
    CHECK(mg_hook_json_string(k, strlen(k), "cwd", &v) == 0 && v && !strcmp(v, "C:\\repo\\x"),
          "whitespace and Windows paths");
    free(v);
  }
}

static void test_json_escape(void) {
  char buf[64];
  size_t n;
  buf[0] = '\0';
  n = mg_hook_json_escape(buf, 0, sizeof(buf), "a\"b\\c\nd\x01\xc3\xa8");
  CHECK(!strcmp(buf, "\"a\\\"b\\\\c\\nd\\u0001\xc3\xa8\"") && n == strlen(buf), "escaped");
  n = mg_hook_json_escape(buf, 0, 8, "0123456789");
  CHECK(n == 8, "overflow reports cap");
}

static void test_prompt_worth(void) {
  CHECK(mg_hook_prompt_worth("why does the daemon refuse the socket"), "a question");
  CHECK(!mg_hook_prompt_worth("ok"), "too short");
  CHECK(!mg_hook_prompt_worth("yes do it"), "three tiny words");
  CHECK(!mg_hook_prompt_worth("/recall sqlite busy timeout"), "slash command");
  CHECK(!mg_hook_prompt_worth("  !git status --short --branch"), "bash mode");
  CHECK(!mg_hook_prompt_worth(""), "empty");
}

static void test_texts(void) {
  mg_status_step_t st[2];
  char buf[2048];
  char body[2000];
  size_t n;
  memset(st, 0, sizeof(st));
  CHECK(mg_hook_session_text(st, 0, buf, sizeof(buf)) == 0, "no steps: silent");
  st[0].action = "apply-safe"; st[0].priority = 1; st[0].command = "graft maintain apply-safe";
  snprintf(st[0].reason, sizeof(st[0].reason), "due");
  st[1].action = "bootstrap"; st[1].priority = 2; st[1].command = "/learn bootstrap";
  snprintf(st[1].reason, sizeof(st[1].reason), "not bootstrapped");
  n = mg_hook_session_text(st, 2, buf, sizeof(buf));
  CHECK(n == strlen(buf) && strstr(buf, "apply-safe (due") && strstr(buf, "; bootstrap ("),
        "session hint lists the steps");
  CHECK(strchr(buf, '\n') == NULL, "one line");
  CHECK(mg_hook_session_text(st, 2, buf, 16) == 15 && strlen(buf) == 15, "truncated, terminated");

  n = mg_hook_prompt_text("abcd", "Title", "short body", "active", buf, sizeof(buf));
  CHECK(n == strlen(buf) && strstr(buf, "[abcd] Title\nshort body") &&
        strstr(buf, "graft get abcd") && !strstr(buf, "[...]"), "short note injected whole");
  CHECK(!strstr(buf, "STALE"), "an active note carries no stale warning");
  n = mg_hook_prompt_text("abcd", "Title", "short body", "stale", buf, sizeof(buf));
  CHECK(n == strlen(buf) && strstr(buf, "marked STALE") && strstr(buf, "[abcd] (stale) Title") &&
        strstr(buf, "short body"), "a stale note is injected with a warning");
  /* 799 ASCII bytes then a 2-byte char: the cut must not split it */
  memset(body, 'x', 799);
  body[799] = '\xc3';
  body[800] = '\xa8';
  memset(body + 801, 'y', 100);
  body[901] = '\0';
  n = mg_hook_prompt_text("id", "T", body, NULL, buf, sizeof(buf));
  CHECK(strstr(buf, "x [...]") != NULL && strstr(buf, "\xc3 [") == NULL,
        "long body cut on a UTF-8 boundary");
}

int main(void) {
  test_plan_quiet();
  test_plan_bootstrap();
  test_plan_maintenance();
  test_read_maint();
  test_report_offline();
  test_json_string();
  test_json_escape();
  test_prompt_worth();
  test_texts();
  if (g_fail) {
    fprintf(stderr, "test_status: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("test_status: ok\n");
  return 0;
}
