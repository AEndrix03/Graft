#include "memgraph/config.h"

#include <stdio.h>
#include <string.h>

static int expect_int(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_config: %s\n", msg);
    return 1;
  }
  return 0;
}

int main(void) {
  const char *path = "test_config_tmp.yaml";
  FILE *fp = fopen(path, "wb");
  mg_config_t cfg;
  int failures = 0;

  if (!fp) {
    fprintf(stderr, "test_config: cannot create temp yaml\n");
    return 1;
  }

  fputs("daemon:\n", fp);
  fputs("  socket_path: \"./custom.sock\"\n", fp);
  fputs("embedding:\n", fp);
  fputs("  threads: 2\n", fp);
  fputs("verification:\n", fp);
  fputs("  cross_encoder_enabled: true\n", fp);
  fputs("cache:\n", fp);
  fputs("  weak_hit_min_vec: 0.9\n", fp);
  fputs("explore:\n", fp);
  fputs("  default_depth: 7\n", fp);
  fputs("http:\n", fp);
  fputs("  enabled: true\n", fp);
  fputs("  bind: \"127.0.0.1\"\n", fp);
  fputs("  port: 9977\n", fp);
  fclose(fp);

  failures += expect_int(mg_config_load(path, &cfg) == MG_OK, "load overrides");
  if (failures == 0) {
    failures += expect_int(strcmp(cfg.socket_path, "./custom.sock") == 0, "socket override");
    failures += expect_int(strcmp(cfg.db_path, "./memgraph.db") == 0, "db default");
    failures += expect_int(cfg.embed_threads == 2, "threads override");
    failures += expect_int(cfg.embed_ctx_size == 8192, "ctx default");
    failures += expect_int(cfg.cross_encoder_enabled, "cross encoder bool");
    failures += expect_int(cfg.weak_hit_min_vec > 0.89f && cfg.weak_hit_min_vec < 0.91f, "float override");
    failures += expect_int(cfg.explore_default_depth == 7, "explore override");
    failures += expect_int(cfg.http_enabled, "http enabled");
    failures += expect_int(strcmp(cfg.http_bind, "127.0.0.1") == 0, "http bind");
    mg_config_free(&cfg);
  }

  failures += expect_int(mg_config_load("missing_config_file.yaml", &cfg) != MG_OK, "missing file error");

  remove(path);
  return failures == 0 ? 0 : 1;
}
