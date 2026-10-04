/* Provenance locator parsing, project identity and file fingerprints.
 * See include/graft/source.h for the rules. */

#include "graft/source.h"

#include "blake3.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define mg_getcwd(b, n) _getcwd((b), (int)(n))
#else
#  include <limits.h>
#  include <unistd.h>
#  define mg_getcwd(b, n) getcwd((b), (n))
#endif

#define MG_SRC_PATH_MAX 4096

static void set_err(char *err, size_t cap, const char *fmt, const char *arg) {
  if (err && cap) {
    snprintf(err, cap, fmt, arg ? arg : "");
  }
}

static char *dup_str(const char *s) {
  size_t n = strlen(s) + 1;
  char *out = (char *)malloc(n);
  if (out) {
    memcpy(out, s, n);
  }
  return out;
}

static void to_slashes(char *p) {
  for (; *p; ++p) {
    if (*p == '\\') {
      *p = '/';
    }
  }
}

static int is_absolute(const char *p) {
#ifdef _WIN32
  if (p[0] == '/' || p[0] == '\\') return 1;
  return isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\');
#else
  return p[0] == '/';
#endif
}

/* Absolute form of an existing-or-not path, '/' separated, no trailing '/'
 * except on a root ("/", "c:/"). POSIX resolves symlinks (so /tmp and
 * /private/tmp agree on macOS) and therefore needs the path to exist. */
static int path_abs(const char *in, char *out, size_t cap) {
  size_t n;
#ifdef _WIN32
  if (!_fullpath(out, in, cap)) return -1;
  to_slashes(out);
  if (isalpha((unsigned char)out[0]) && out[1] == ':') {
    out[0] = (char)tolower((unsigned char)out[0]);
  }
#else
  char buf[PATH_MAX];
  if (!realpath(in, buf)) return -1;
  if (strlen(buf) + 1 > cap) return -1;
  memcpy(out, buf, strlen(buf) + 1);
#endif
  n = strlen(out);
  while (n > 1 && out[n - 1] == '/' && !(n == 3 && out[1] == ':')) {
    out[--n] = '\0';
  }
  return 0;
}

/* 0 missing, 1 regular file, 2 directory, 3 other. */
static int path_kind(const char *p) {
  struct stat st;
  if (stat(p, &st) != 0) return 0;
  if ((st.st_mode & S_IFMT) == S_IFDIR) return 2;
  if ((st.st_mode & S_IFMT) == S_IFREG) return 1;
  return 3;
}

/* Strip the last component. -1 when path already is a root. */
static int parent_dir(char *path) {
  char *slash = strrchr(path, '/');
  if (!slash || slash[1] == '\0') return -1;
  if (slash == path || (slash == path + 2 && path[1] == ':')) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  return 0;
}

static int join_path(char *out, size_t cap, const char *dir, const char *rel) {
  size_t n = strlen(dir);
  int w = snprintf(out, cap, "%s%s%s", dir, (n && dir[n - 1] == '/') ? "" : "/", rel);
  return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

static int prefix_eq(const char *a, const char *b, size_t n) {
#ifdef _WIN32
  size_t i;
  for (i = 0; i < n; ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
  }
  return 1;
#else
  return strncmp(a, b, n) == 0;
#endif
}

/* If path lies under root, the offset of its root-relative part, else -1. */
static long rel_offset(const char *path, const char *root) {
  size_t n = strlen(root);
  if (!prefix_eq(path, root, n)) return -1;
  if (n && root[n - 1] == '/') return (long)n;
  if (path[n] == '/') return (long)n + 1;
  return -1;
}

/* Nearest ancestor of dir (itself included) holding a `.git` entry. */
static int find_git_root(const char *dir, char *out, size_t cap) {
  char cur[MG_SRC_PATH_MAX];
  char probe[MG_SRC_PATH_MAX];
  if (strlen(dir) + 1 > sizeof(cur)) return -1;
  memcpy(cur, dir, strlen(dir) + 1);
  for (;;) {
    if (join_path(probe, sizeof(probe), cur, ".git") == 0 && path_kind(probe) != 0) {
      if (strlen(cur) + 1 > cap) return -1;
      memcpy(out, cur, strlen(cur) + 1);
      return 0;
    }
    if (parent_dir(cur) != 0) return -1;
  }
}

static char *trim(char *s) {
  char *e;
  while (*s && isspace((unsigned char)*s)) s++;
  e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
  return s;
}

/* First line of a small file, trimmed. */
static int read_first_line(const char *path, char *out, size_t cap) {
  char buf[MG_SRC_PATH_MAX];
  char *t;
  FILE *fp = fopen(path, "rb");
  if (!fp) return -1;
  if (!fgets(buf, sizeof(buf), fp)) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  t = trim(buf);
  if (strlen(t) + 1 > cap) return -1;
  memcpy(out, t, strlen(t) + 1);
  return 0;
}

/* Directory holding the repository's shared `config`: `.git` itself, the
 * `gitdir:` a worktree's `.git` file points to, or that gitdir's commondir. */
static int git_config_path(const char *root, char *out, size_t cap) {
  char gitdir[MG_SRC_PATH_MAX];
  char line[MG_SRC_PATH_MAX];
  char tmp[MG_SRC_PATH_MAX];
  if (join_path(gitdir, sizeof(gitdir), root, ".git") != 0) return -1;
  if (path_kind(gitdir) == 1) {
    if (read_first_line(gitdir, line, sizeof(line)) != 0 || strncmp(line, "gitdir:", 7) != 0) {
      return -1;
    }
    char *target = trim(line + 7);
    to_slashes(target);
    if (is_absolute(target)) {
      snprintf(gitdir, sizeof(gitdir), "%s", target);
    } else if (join_path(gitdir, sizeof(gitdir), root, target) != 0) {
      return -1;
    }
  }
  if (join_path(tmp, sizeof(tmp), gitdir, "commondir") == 0 &&
      read_first_line(tmp, line, sizeof(line)) == 0 && line[0]) {
    to_slashes(line);
    if (is_absolute(line)) {
      snprintf(tmp, sizeof(tmp), "%s", line);
    } else if (join_path(tmp, sizeof(tmp), gitdir, line) != 0) {
      return -1;
    }
    memcpy(gitdir, tmp, strlen(tmp) + 1);
  }
  return join_path(out, cap, gitdir, "config");
}

/* url of [remote "origin"] in the repository's config. */
static int read_git_origin(const char *root, char *out, size_t cap) {
  char path[MG_SRC_PATH_MAX];
  char line[MG_SRC_PATH_MAX];
  int in_origin = 0;
  FILE *fp;
  if (git_config_path(root, path, sizeof(path)) != 0) return -1;
  fp = fopen(path, "rb");
  if (!fp) return -1;
  while (fgets(line, sizeof(line), fp)) {
    char *t = trim(line);
    if (t[0] == '[') {
      char sect[256];
      size_t j = 0;
      const char *p;
      /* collapse whitespace: [remote "origin"] */
      for (p = t; *p && j + 1 < sizeof(sect); ++p) {
        if (!isspace((unsigned char)*p)) sect[j++] = *p;
      }
      sect[j] = '\0';
      in_origin = strcmp(sect, "[remote\"origin\"]") == 0;
      continue;
    }
    if (in_origin && strncmp(t, "url", 3) == 0) {
      char *v = trim(t + 3);
      if (*v != '=') continue;
      v = trim(v + 1);
      if (!*v || strlen(v) + 1 > cap) break;
      memcpy(out, v, strlen(v) + 1);
      fclose(fp);
      return 0;
    }
  }
  fclose(fp);
  return -1;
}

int mg_source_normalize_remote(const char *url, char *out, size_t cap) {
  char buf[MG_SRC_PATH_MAX];
  char host[512] = "";
  char *path;
  char *scheme_end;
  size_t n;
  if (!url || !out || cap == 0 || strlen(url) + 1 > sizeof(buf)) return -1;
  memcpy(buf, url, strlen(url) + 1);
  path = trim(buf);
  to_slashes(path);

  scheme_end = strstr(path, "://");
  if (scheme_end) {
    int is_ssh = (scheme_end - path == 3 && strncmp(path, "ssh", 3) == 0) ||
                 (scheme_end - path == 7 && strncmp(path, "git+ssh", 7) == 0);
    char *authority = scheme_end + 3;
    char *slash = strchr(authority, '/');
    char *at;
    size_t alen = slash ? (size_t)(slash - authority) : strlen(authority);
    if (alen >= sizeof(host)) return -1;
    memcpy(host, authority, alen);
    host[alen] = '\0';
    path = slash ? slash : authority + alen;
    /* credentials: user[:password]@host */
    at = strrchr(host, '@');
    if (at) memmove(host, at + 1, strlen(at + 1) + 1);
    if (is_ssh) {
      char *colon = strchr(host, ':');
      if (colon) *colon = '\0';
    }
  } else {
    char *colon = strchr(path, ':');
    char *slash = strchr(path, '/');
    /* scp-like [user@]host:path; a drive letter (c:/x) is a local path */
    if (colon && (!slash || colon < slash) && colon - path > 1) {
      size_t hlen = (size_t)(colon - path);
      char *at;
      if (hlen >= sizeof(host)) return -1;
      memcpy(host, path, hlen);
      host[hlen] = '\0';
      at = strrchr(host, '@');
      if (at) memmove(host, at + 1, strlen(at + 1) + 1);
      path = colon + 1;
    }
  }
  for (char *h = host; *h; ++h) *h = (char)tolower((unsigned char)*h);

  n = strlen(path);
  while (n > 0 && path[n - 1] == '/') path[--n] = '\0';
  if (n >= 4 && strcmp(path + n - 4, ".git") == 0) {
    path[n - 4] = '\0';
    n -= 4;
  }
  while (n > 0 && path[n - 1] == '/') path[--n] = '\0';
  if (host[0]) {
    while (*path == '/') path++;
    if (snprintf(out, cap, "%s%s%s", host, *path ? "/" : "", path) >= (int)cap) return -1;
  } else {
    if (!*path || strlen(path) + 1 > cap) return -1;
    memcpy(out, path, strlen(path) + 1);
  }
  return out[0] ? 0 : -1;
}

static int project_id(const char *root, char *out, size_t cap) {
  char url[MG_SRC_PATH_MAX];
  if (read_git_origin(root, url, sizeof(url)) == 0 &&
      mg_source_normalize_remote(url, out, cap) == 0) {
    return 0;
  }
  if (strlen(root) + 1 > cap) return -1;
  memcpy(out, root, strlen(root) + 1);
  return 0;
}

int mg_source_resolve_project(const char *dir, char *root, size_t root_cap,
                              char *project, size_t project_cap) {
  char abs[MG_SRC_PATH_MAX];
  if (!dir || !root || !project) return -1;
  if (path_abs(dir, abs, sizeof(abs)) != 0 || path_kind(abs) != 2) return -1;
  if (find_git_root(abs, root, root_cap) != 0) {
    if (strlen(abs) + 1 > root_cap) return -1;
    memcpy(root, abs, strlen(abs) + 1);
  }
  return project_id(root, project, project_cap);
}

int mg_source_hash_file(const char *path, char out_hex[MG_SOURCE_FP_HEX + 1]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char buf[65536];
  unsigned char digest[32];
  blake3_hasher hasher;
  size_t got;
  FILE *fp;
  int i;
  if (path_kind(path) != 1) return 1;
  fp = fopen(path, "rb");
  if (!fp) return -1;
  blake3_hasher_init(&hasher);
  while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
    blake3_hasher_update(&hasher, buf, got);
  }
  if (ferror(fp)) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  blake3_hasher_finalize(&hasher, digest, sizeof(digest));
  for (i = 0; i < 32; ++i) {
    out_hex[i * 2] = hex[digest[i] >> 4];
    out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
  }
  out_hex[MG_SOURCE_FP_HEX] = '\0';
  return 0;
}

int mg_source_kind_valid(const char *kind) {
  return kind && (!strcmp(kind, "file") || !strcmp(kind, "url") ||
                  !strcmp(kind, "conversation") || !strcmp(kind, "manual"));
}

void mg_source_spec_free(mg_source_spec_t *spec) {
  if (!spec) return;
  free(spec->project);
  free(spec->locator);
  spec->project = NULL;
  spec->locator = NULL;
}

static int parse_file(const char *path, int allow_relative, mg_source_spec_t *out,
                      char *err, size_t err_cap) {
  char abs[MG_SRC_PATH_MAX];
  char dir[MG_SRC_PATH_MAX];
  char root[MG_SRC_PATH_MAX];
  char project[MG_SRC_PATH_MAX];
  long off;
  int kind;

  if (!*path) {
    set_err(err, err_cap, "source 'file:' needs a path%s", NULL);
    return -1;
  }
  if (!allow_relative && !is_absolute(path)) {
    set_err(err, err_cap, "source file path must be absolute here: %s", path);
    return -1;
  }
  if (path_abs(path, abs, sizeof(abs)) != 0 || (kind = path_kind(abs)) == 0) {
    set_err(err, err_cap, "source file not found: %s", path);
    return -1;
  }
  if (kind != 1) {
    set_err(err, err_cap, "source is not a regular file: %s", path);
    return -1;
  }
  memcpy(dir, abs, strlen(abs) + 1);
  if (parent_dir(dir) != 0) {
    set_err(err, err_cap, "source file not found: %s", path);
    return -1;
  }
  if (find_git_root(dir, root, sizeof(root)) != 0) {
    char cwd[MG_SRC_PATH_MAX];
    char cwd_abs[MG_SRC_PATH_MAX];
    memcpy(root, dir, strlen(dir) + 1);
    if (allow_relative && mg_getcwd(cwd, sizeof(cwd)) &&
        path_abs(cwd, cwd_abs, sizeof(cwd_abs)) == 0 && rel_offset(abs, cwd_abs) >= 0) {
      memcpy(root, cwd_abs, strlen(cwd_abs) + 1);
    }
  }
  off = rel_offset(abs, root);
  if (off < 0 || project_id(root, project, sizeof(project)) != 0) {
    set_err(err, err_cap, "cannot resolve the project of: %s", path);
    return -1;
  }
  if (mg_source_hash_file(abs, out->fingerprint) != 0) {
    set_err(err, err_cap, "cannot read source file: %s", path);
    return -1;
  }
  out->project = dup_str(project);
  out->locator = dup_str(abs + off);
  if (!out->project || !out->locator) {
    mg_source_spec_free(out);
    set_err(err, err_cap, "out of memory%s", NULL);
    return -1;
  }
  return 0;
}

int mg_source_parse(const char *spec, int allow_relative,
                    mg_source_spec_t *out, char *err, size_t err_cap) {
  static const char *const tagged[] = { "conversation", "manual" };
  size_t i;
  if (!spec || !out) {
    set_err(err, err_cap, "missing source%s", NULL);
    return -1;
  }
  memset(out, 0, sizeof(*out));

  if (strncmp(spec, "file:", 5) == 0) {
    snprintf(out->kind, sizeof(out->kind), "file");
    return parse_file(spec + 5, allow_relative, out, err, err_cap);
  }
  if (strncmp(spec, "url:", 4) == 0) {
    if (!spec[4]) {
      set_err(err, err_cap, "source 'url:' needs a url%s", NULL);
      return -1;
    }
    snprintf(out->kind, sizeof(out->kind), "url");
    out->project = dup_str("");
    out->locator = dup_str(spec + 4);
  } else {
    for (i = 0; i < sizeof(tagged) / sizeof(*tagged); ++i) {
      size_t n = strlen(tagged[i]);
      if (strncmp(spec, tagged[i], n) == 0 && (spec[n] == '\0' || spec[n] == ':')) {
        snprintf(out->kind, sizeof(out->kind), "%s", tagged[i]);
        out->project = dup_str("");
        out->locator = dup_str(spec[n] ? spec + n + 1 : "");
        break;
      }
    }
    if (!out->kind[0]) {
      set_err(err, err_cap,
              "unknown source '%s' (expected file:<path>, url:<url>, conversation or manual)",
              spec);
      return -1;
    }
  }
  if (!out->project || !out->locator) {
    mg_source_spec_free(out);
    set_err(err, err_cap, "out of memory%s", NULL);
    return -1;
  }
  return 0;
}
