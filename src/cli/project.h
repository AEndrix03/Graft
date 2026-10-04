#ifndef GRAFT_CLI_PROJECT_H
#define GRAFT_CLI_PROJECT_H

#include <stddef.h>
#include <stdint.h>

/* `graft project <status|mark|reset> ...` (issue #3): the bootstrap
 * coverage state of one project inside the active profile. argv[1] is
 * "project". CLI-only: never starts or calls the daemon. Prints JSON on
 * stdout; returns the exit code (2 usage, 1 I/O error). */
int mg_project_cmd(int argc, char **argv);

/* ---- exposed for tests ---- */

#define MG_PROJECT_TOPIC_COVERED "covered"
#define MG_PROJECT_TOPIC_PENDING "pending"
#define MG_PROJECT_TOPIC_SKIPPED "skipped"

typedef struct {
    char   *name;
    char    state[12];     /* covered | pending | skipped */
    int     priority;      /* 1 high, 2 normal, 3 low */
    int64_t updated_at;    /* unix ms */
    char   *note;          /* "" when none */
} mg_project_topic_t;

typedef struct {
    int64_t             runs;
    int64_t             last_run_at;   /* unix ms, 0 = never */
    mg_project_topic_t *topics;        /* in insertion order */
    size_t              n_topics;
    size_t              cap;
} mg_project_state_t;

/* <dir of db_path>/projects/<blake3(project) as 32 hex>.tsv. The state
 * follows the DB it describes: one per profile (or per GRAFT_DB_PATH). */
int  mg_project_state_path(const char *db_path, const char *project, char *out, size_t cap);

/* A missing file loads as the empty state (0). Unknown lines are ignored.
 * Returns -1 when the file exists but cannot be read. */
int  mg_project_state_load(const char *path, mg_project_state_t *st);

/* Writes through a temp file + rename. Creates the projects/ dir. */
int  mg_project_state_save(const char *path, const char *project, const mg_project_state_t *st);
void mg_project_state_free(mg_project_state_t *st);

/* Upserts a topic. state "drop" removes it. priority <= 0 keeps the
 * existing one (2 for a new topic); note NULL keeps the existing one.
 * Tabs and newlines in name/note become spaces. Returns 0, -1 on a bad
 * state or empty name, -2 on allocation failure. */
int  mg_project_state_mark(mg_project_state_t *st, const char *name, const char *state,
                           int priority, const char *note, int64_t now_ms);

/* Where a directory's project state lives, as every `project` subcommand
 * (and `graft status`) resolves it: the project of dir, the active profile,
 * its DB (GRAFT_DB_PATH wins) and the state file next to it. Returns 0, -1
 * when dir is not a directory, -2 when the profile DB cannot be resolved,
 * -3 when the state path does not fit. */
typedef struct {
    char root[4096];
    char project[4096];
    char profile[128];
    char db[4096];
    char state_file[4096];
} mg_project_ctx_t;

int  mg_project_resolve(const char *dir, mg_project_ctx_t *c);

/* Files of `project` (kind file) supporting at least one live node, and the
 * number of those nodes, read straight from the SQLite file (read-only).
 * Returns 0; 1 when the DB does not exist yet (counts 0); -1 when it cannot
 * be read (no provenance tables yet, locked, corrupt), with the reason in
 * err. */
int  mg_project_provenance(const char *db_path, const char *project,
                           int64_t *files, int64_t *nodes, char *err, size_t err_cap);

/* The `project status` envelope { status: 0, result: {...} } as mpack in
 * *out (free() it). prov_rc / prov_error come from mg_project_provenance.
 * Returns 0, or 1 on allocation failure. */
int  mg_project_status_report(const char *project, const char *root, const char *profile,
                              const char *state_file, const mg_project_state_t *st,
                              int prov_rc, const char *prov_error, int64_t files, int64_t nodes,
                              char **out, size_t *out_len);

#endif
