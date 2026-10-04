#ifndef GRAFT_SOURCE_H
#define GRAFT_SOURCE_H

/* Provenance locators (issue #4).
 *
 * A node may record where it came from with one or more source locators:
 *
 *   file:<path>         a file inside a project; fingerprinted with BLAKE3
 *   url:<url>           a web page or document, stored verbatim
 *   conversation[:tag]  learned in a conversation
 *   manual[:tag]        written by hand
 *
 * File locators are stored relative to a project root with '/' separators,
 * and carry a project id so the same relative path in two repositories never
 * collides inside a profile shared across repositories:
 *
 *   root     the nearest ancestor of the file holding a `.git` entry; without
 *            one, the working directory when it contains the file, else the
 *            file's own directory.
 *   project  the normalized `origin` remote of that repository when it has
 *            one (scheme, credentials, port-less ssh form, trailing `.git`
 *            dropped, host lowercased: `github.com/Owner/Repo`), otherwise
 *            the absolute root path with '/' separators.
 *
 * Nothing here talks to the daemon or the database: the CLI resolves file
 * sources itself (cwd-relative paths), the daemon only for absolute ones. */

#include <stddef.h>

#define MG_SOURCE_FP_HEX 64   /* BLAKE3 digest as lowercase hex */

typedef struct {
  char  kind[16];                     /* file | url | conversation | manual */
  char *project;                      /* "" for kinds without a project */
  char *locator;                      /* relative path, url, or tag ("" allowed) */
  char  fingerprint[MG_SOURCE_FP_HEX + 1];  /* "" when not applicable */
} mg_source_spec_t;

/* Parse a `--source` value. allow_relative=1 resolves a relative file path
 * against the working directory (CLI); 0 rejects it (daemon / HTTP, whose
 * working directory means nothing to the caller). Returns 0 on success, -1
 * with a message in err otherwise (unknown form, missing file, ...). */
int  mg_source_parse(const char *spec, int allow_relative,
                     mg_source_spec_t *out, char *err, size_t err_cap);
void mg_source_spec_free(mg_source_spec_t *spec);

/* Kinds accepted on the wire. */
int  mg_source_kind_valid(const char *kind);

/* Resolve the project a directory belongs to: root is its nearest `.git`
 * ancestor, or the directory itself. Both outputs use '/' separators.
 * Returns 0 on success, -1 if dir does not exist. */
int  mg_source_resolve_project(const char *dir, char *root, size_t root_cap,
                               char *project, size_t project_cap);

/* Normalize a git remote URL into a project id (see above). */
int  mg_source_normalize_remote(const char *url, char *out, size_t cap);

/* BLAKE3 of a file's bytes as lowercase hex.
 * Returns 0 on success, 1 if the path is missing or not a regular file,
 * -1 if it exists but cannot be read. */
int  mg_source_hash_file(const char *path, char out_hex[MG_SOURCE_FP_HEX + 1]);

#endif
