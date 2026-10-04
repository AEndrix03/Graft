#ifndef GRAFT_CLI_STATUS_H
#define GRAFT_CLI_STATUS_H

#include "project.h"
#include "mpack.h"

#include <stddef.h>
#include <stdint.h>

/* `graft status [--root DIR]` (issue #6): one cheap call that tells an agent
 * whether any Graft housekeeping is due in this project. argv[1] is
 * "status". Prints JSON on stdout; returns the exit code (2 usage, 1 I/O
 * error). Never starts the daemon. */
int mg_status_cmd(int argc, char **argv);

/* ---- exposed for tests and for `graft hook` ---- */

/* The signals the plan is computed from. */
typedef struct {
    int         git;                /* the project root is a git checkout */
    int         bootstrapped;
    int64_t     pending_topics;
    const char *next_topic;         /* highest-priority pending topic, or NULL */
    int         have_maint;         /* the daemon answered maintain_status */
    int         rec_apply_safe;     /* maintain status recommends apply-safe */
    int         rec_scan;           /* ... and scan */
    int64_t     inserts_since_apply_safe;
    int64_t     inserts_since_scan;
    int64_t     pending_total;
    int64_t     contradictions;
    int64_t     src_changed;
    int64_t     src_removed;
} mg_status_signals_t;

#define MG_STATUS_MAX_STEPS 6

typedef struct {
    const char *action;             /* bootstrap apply-safe resolve refresh-sources scan */
    int         priority;           /* 1 high, 2 normal, 3 low */
    char        reason[192];
    const char *command;
} mg_status_step_t;

/* The housekeeping worth doing now, most urgent first (stable within a
 * priority). Returns the number of steps written (<= cap). */
size_t mg_status_plan(const mg_status_signals_t *s, mg_status_step_t *out, size_t cap);

typedef struct {
    mg_project_ctx_t   ctx;
    mg_project_state_t st;
    int                prov_rc;
    char               prov_error[256];
    int64_t            files;
    int64_t            nodes;
    /* the daemon's maintain_status reply; resp NULL when it did not answer */
    mpack_tree_t       maint_tree;
    void              *maint_resp;
    const char        *maint_error;
    int                daemon_checked;
    int                daemon_running;
    mg_status_signals_t sig;
    mg_status_step_t   steps[MG_STATUS_MAX_STEPS];
    size_t             n_steps;
} mg_status_t;

/* Gathers everything for dir: project state, provenance counts, and, when
 * use_daemon is set, the maintenance status of a daemon that is already
 * running (it is never started for this, and its failures stay silent).
 * Returns 0, -1 when dir is not a directory, -2 when the profile DB or
 * the state file cannot be resolved or read. */
int  mg_status_gather(const char *dir, int use_daemon, mg_status_t *s);
void mg_status_free(mg_status_t *s);

/* The printed envelope { status: 0, result: {...} } as mpack (free() it).
 * Returns 0, or 1 on allocation failure. */
int  mg_status_report(const mg_status_t *s, char **out, size_t *out_len);

/* Reads the signals of a maintain_status result map into s. */
void mg_status_read_maint(mpack_node_t result, mg_status_signals_t *s);

#endif
