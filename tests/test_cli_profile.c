/* `graft profile` subcommands and path resolution. Runs against a throwaway
 * GRAFT_HOME inside the build directory, never the user's ~/.graft. The
 * subcommands print JSON on stdout; only exit codes and on-disk effects are
 * asserted here. */

#include "../src/cli/profile.h"
#include "graft/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <direct.h>
#  define SEP "\\"
static void set_env(const char *k, const char *v) { _putenv_s(k, v ? v : ""); }
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define SEP "/"
static void set_env(const char *k, const char *v) {
  if (v) setenv(k, v, 1); else unsetenv(k);
}
#endif

static int g_failures = 0;
static char g_home[1024];

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_cli_profile: %s\n", msg);
    g_failures++;
  }
}

static int file_exists(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

static int run(int argc, const char **argv) {
  int rc = mg_profile_cmd(argc, (char **)argv);
  fflush(stdout);
  return rc;
}

#define RUN(...) run((int)(sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *)), \
                     (const char *[]){__VA_ARGS__})

/* --- pure helpers --- */

static void test_name_validation(void) {
  char n64[66], n65[67];
  memset(n64, 'a', 64); n64[64] = '\0';
  memset(n65, 'a', 65); n65[65] = '\0';
  expect(mg_profile_name_valid("work") == 0, "simple name");
  expect(mg_profile_name_valid("Client_A-2") == 0, "letters, digits, '_' and '-'");
  expect(mg_profile_name_valid(n64) == 0, "64 chars is the limit");
  expect(mg_profile_name_valid(n65) != 0, "65 chars is too long");
  expect(mg_profile_name_valid("") != 0, "empty name");
  expect(mg_profile_name_valid(NULL) != 0, "NULL name");
  expect(mg_profile_name_valid("..") != 0, "'..' cannot escape the profiles dir");
  expect(mg_profile_name_valid("a/b") != 0, "'/' is rejected");
  expect(mg_profile_name_valid("a\\b") != 0, "'\\' is rejected");
  expect(mg_profile_name_valid("a b") != 0, "space is rejected");
  expect(mg_profile_name_valid("caf\xC3\xA8") != 0, "non-ASCII is rejected");
  printf("ok name validation\n");
}

static void test_active_profile(void) {
  char out[128];
  set_env("GRAFT_PROFILE", NULL);
  mg_profile_active(out, sizeof(out));
  expect(strcmp(out, MG_PROFILE_DEFAULT) == 0, "unset GRAFT_PROFILE means default");
  set_env("GRAFT_PROFILE", "work");
  mg_profile_active(out, sizeof(out));
  expect(strcmp(out, "work") == 0, "GRAFT_PROFILE selects the profile");
  set_env("GRAFT_PROFILE", "../etc");
  mg_profile_active(out, sizeof(out));
  expect(strcmp(out, MG_PROFILE_DEFAULT) == 0, "invalid GRAFT_PROFILE falls back to default");
  set_env("GRAFT_PROFILE", NULL);
  printf("ok active profile\n");
}

static void test_paths(void) {
  char out[1024], want[1024];
  expect(mg_profile_home(out, sizeof(out)) == 0 && strcmp(out, g_home) == 0,
         "GRAFT_HOME overrides the home directory");
  expect(mg_profile_dir("work", out, sizeof(out), 0) == 0, "profile dir");
  snprintf(want, sizeof(want), "%s" SEP "profiles" SEP "work", g_home);
  expect(strcmp(out, want) == 0, "profile dir is <home>/profiles/<name>");
  expect(mg_profile_exists("work") == 0, "create=0 does not create the dir");
  expect(mg_profile_db_path("work", out, sizeof(out), 0) == 0, "db path");
  snprintf(want, sizeof(want), "%s" SEP "profiles" SEP "work" SEP "graft.db", g_home);
  expect(strcmp(out, want) == 0, "db path is <dir>/graft.db");
  expect(mg_profile_socket_path("work", out, sizeof(out), 0) == 0, "socket path");
#ifdef _WIN32
  snprintf(want, sizeof(want), "%s\\sockets\\work.sock", g_home);
#else
  snprintf(want, sizeof(want), "/tmp/graft-work.sock");
#endif
  expect(strcmp(out, want) == 0, "socket path is per profile");
  expect(mg_profile_dir("work", out, 8, 0) != 0, "truncated output is an error");
  printf("ok paths\n");
}

/* --- subcommands --- */

static void test_add_and_remove(void) {
  expect(RUN("graft", "profile", "add", "work") == 0, "add a new profile");
  expect(mg_profile_exists("work"), "added profile exists on disk");
  expect(RUN("graft", "profile", "add", "work") == 1, "adding twice fails");
  expect(RUN("graft", "profile", "add", "bad/name") == 2, "invalid name is a usage error");
  expect(RUN("graft", "profile", "list") == 0, "list");
  expect(RUN("graft", "profile", "current") == 0, "current");
  expect(RUN("graft", "profile", "set", "work", "--shell", "fish") == 0, "set existing");
  expect(RUN("graft", "profile", "set", "ghost") == 1, "set unknown profile fails");
  expect(RUN("graft", "profile", "set", MG_PROFILE_DEFAULT) == 0,
         "default can be selected before it exists");

  expect(RUN("graft", "profile", "remove", MG_PROFILE_DEFAULT, "--yes") == 1,
         "default cannot be removed");
  expect(RUN("graft", "profile", "remove", "ghost", "--yes") == 1,
         "removing an unknown profile fails");
  expect(RUN("graft", "profile", "rm", "work", "-y") == 0, "remove with -y");
  expect(!mg_profile_exists("work"), "removed profile is gone from disk");
  printf("ok add and remove\n");
}

static void test_import_export(void) {
  char src[1024], junk[1024], out[1024], db[1024];
  mg_storage_t *s = NULL;
  FILE *f;

  snprintf(src, sizeof(src), "%s" SEP "src.db", g_home);
  snprintf(junk, sizeof(junk), "%s" SEP "junk.db", g_home);
  snprintf(out, sizeof(out), "%s" SEP "exported.db", g_home);

  expect(mg_storage_open(src, &s) == MG_OK && mg_storage_apply_schema(s) == MG_OK,
         "create a source graft DB");
  if (s) mg_storage_close(s);
  f = fopen(junk, "wb");
  if (f) { fputs("definitely not sqlite", f); fclose(f); }

  expect(RUN("graft", "profile", "import", "--name", "imp", "--file", junk) == 1,
         "a non-SQLite file is refused");
  expect(!mg_profile_exists("imp"), "a refused import creates nothing");
  expect(RUN("graft", "profile", "import", "--name", "imp", "--file", "no-such.db") == 1,
         "a missing file is refused");

  expect(RUN("graft", "profile", "import", "--name", "imp", "--file", src) == 0, "import");
  mg_profile_db_path("imp", db, sizeof(db), 0);
  expect(file_exists(db), "import writes <profile>/graft.db");
  expect(RUN("graft", "profile", "import", "--name", "imp", "--file", src) == 1,
         "importing over an existing profile needs --force");
  expect(RUN("graft", "profile", "import", "--name", "imp", "--file", src, "--force") == 0,
         "--force overwrites");

  expect(RUN("graft", "profile", "export", "imp") == 2, "export without --path");
  expect(RUN("graft", "profile", "export", "ghost", "--path", out) == 1,
         "exporting an unknown profile fails");
  expect(RUN("graft", "profile", "add", "empty") == 0, "add a profile with no DB");
  expect(RUN("graft", "profile", "export", "empty", "--path", out) == 1,
         "exporting a profile with no DB fails");
  /* A daemon that crashed leaves committed writes in graft.db-wal, not yet
   * checkpointed into graft.db. Keeping a connection open while exporting
   * holds the keyword in the WAL the same way. */
  {
    mg_storage_t *held = NULL;
    mg_keyword_id_t kw = 0;
    expect(mg_storage_open(db, &held) == MG_OK && mg_storage_apply_schema(held) == MG_OK,
           "open the profile DB");
    expect(mg_storage_upsert_keyword(held, "only-in-wal", NULL, &kw) == MG_OK,
           "commit a keyword to the WAL");
    expect(RUN("graft", "profile", "export", "imp", "--path", out) == 0, "export");
    if (held) mg_storage_close(held);
  }
  expect(file_exists(out), "export writes the target file");
  {
    mg_storage_t *exp = NULL;
    int64_t n_kw = 0;
    expect(mg_storage_open(out, &exp) == MG_OK
           && mg_storage_count(exp, 2, &n_kw) == MG_OK && n_kw == 1,
           "export includes writes still in the WAL");
    if (exp) mg_storage_close(exp);
  }
  expect(RUN("graft", "profile", "import", "--name", "roundtrip", "--file", out) == 0,
         "an exported file can be imported back");

  RUN("graft", "profile", "remove", "imp", "--yes");
  RUN("graft", "profile", "remove", "empty", "--yes");
  RUN("graft", "profile", "remove", "roundtrip", "--yes");
  remove(src);
  remove(junk);
  remove(out);
  printf("ok import and export\n");
}

static void test_usage_errors(void) {
  expect(RUN("graft", "profile") != 0, "missing subcommand");
  expect(RUN("graft", "profile", "add") != 0, "add without a name");
  expect(RUN("graft", "profile", "import", "--name", "x") != 0, "import without --file");
  printf("ok usage errors\n");
}

int main(void) {
  char cwd_home[512];
#ifdef _WIN32
  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
  if (!_getcwd(cwd_home, sizeof(cwd_home))) return 1;
#else
  if (!getcwd(cwd_home, sizeof(cwd_home))) return 1;
#endif
  /* ctest runs from the build directory: keep the sandbox there. */
  snprintf(g_home, sizeof(g_home), "%s" SEP "test_cli_profile_home", cwd_home);
  set_env("GRAFT_HOME", g_home);
  set_env("GRAFT_PROFILE", NULL);
  /* Leftovers from an earlier failed run. */
  RUN("graft", "profile", "remove", "work", "--yes");
  RUN("graft", "profile", "remove", "imp", "--yes");
  RUN("graft", "profile", "remove", "empty", "--yes");
  RUN("graft", "profile", "remove", "roundtrip", "--yes");

  test_name_validation();
  test_active_profile();
  test_paths();
  test_add_and_remove();
  test_import_export();
  test_usage_errors();

  if (g_failures) {
    fprintf(stderr, "test_cli_profile: %d failure(s), sandbox kept at %s\n", g_failures, g_home);
    return 1;
  }
  return 0;
}
