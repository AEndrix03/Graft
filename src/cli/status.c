/* graft CLI — one-call housekeeping state for agents (issue #6).
 *
 *   graft status [--root DIR]
 *
 * Everything an agent needs to decide, at the start of a turn, whether any
 * Graft housekeeping is due, in one JSON:
 *
 *   - project:     the bootstrap state of DIR's project (`project status`,
 *                  read from the state file and the DB, no daemon);
 *   - maintenance: the daemon's `maintain status`, only when a daemon is
 *                  already running. Starting one costs a model load (seconds)
 *                  and nothing here needs the model, so a stopped daemon is
 *                  reported as maintenance: null with the reason;
 *   - sources:     the changed / removed file sources as of the last
 *                  `maintain scan` (its candidate counts). Re-hashing every
 *                  recorded file on each call is not cheap, so this is the
 *                  last known count, never a fresh diff;
 *   - next:        the housekeeping worth doing now, most urgent first.
 *
 * Budget: a state file read, one read-only SQLite count and at most one
 * cheap daemon call (no embedding) - well under a second.
 */

#include "status.h"
#include "client.h"
#include "project.h"
#include "mpack.h"

#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MG_CLI_PATH 4096

/* ---------- plan ---------- */

static void push_step(mg_status_step_t *out, size_t *n, const char *action, int priority,
                      const char *command, const char *reason) {
    mg_status_step_t *st;
    if (*n >= MG_STATUS_MAX_STEPS) return;
    st = &out[(*n)++];
    st->action = action;
    st->priority = priority;
    st->command = command;
    snprintf(st->reason, sizeof(st->reason), "%s", reason);
}

#define RESOLVE_CMD "graft maintain scan --limit 5, then graft maintain resolve <id> --action ..."

size_t mg_status_plan(const mg_status_signals_t *s, mg_status_step_t *out, size_t cap) {
    mg_status_step_t tmp[MG_STATUS_MAX_STEPS];
    char why[192];
    size_t n = 0, k = 0;
    int64_t src = s->src_changed + s->src_removed;
    int64_t other = s->pending_total - src;

    if (s->have_maint && s->rec_apply_safe) {
        snprintf(why, sizeof(why), "mechanical cleanup due (%lld inserts since the last pass)",
                 (long long)s->inserts_since_apply_safe);
        push_step(tmp, &n, "apply-safe", 1, "graft maintain apply-safe", why);
    }
    if (s->have_maint && other > 0) {
        snprintf(why, sizeof(why), "%lld pending maintenance candidate(s)%s", (long long)other,
                 s->contradictions > 0 ? ", contradictions among them" : "");
        push_step(tmp, &n, "resolve", s->contradictions > 0 ? 1 : 2, RESOLVE_CMD, why);
    }
    if (s->have_maint && src > 0) {
        snprintf(why, sizeof(why),
                 "%lld note(s) backed by files that changed or disappeared (as of the last scan)",
                 (long long)src);
        push_step(tmp, &n, "refresh-sources", 2, "graft sources diff --changed-only", why);
    }
    if (s->git && !s->bootstrapped) {
        push_step(tmp, &n, "bootstrap", 2,
                  "/learn bootstrap (one bounded pass, the task area first)",
                  "project not bootstrapped yet");
    } else if (s->bootstrapped && s->pending_topics > 0) {
        snprintf(why, sizeof(why), "%lld pending bootstrap topic(s), next: %s",
                 (long long)s->pending_topics, s->next_topic ? s->next_topic : "?");
        push_step(tmp, &n, "bootstrap", 3, "/learn bootstrap (continue, one bounded pass)", why);
    }
    if (s->have_maint && s->rec_scan) {
        snprintf(why, sizeof(why), "%lld inserts since the last scan",
                 (long long)s->inserts_since_scan);
        push_step(tmp, &n, "scan", 3, "graft maintain scan --limit 20", why);
    }
    for (int p = 1; p <= 3; p++) {
        for (size_t i = 0; i < n && k < cap; i++) {
            if (tmp[i].priority == p) out[k++] = tmp[i];
        }
    }
    return k;
}

/* ---------- maintenance reply ---------- */

static int64_t int_at(mpack_node_t map, const char *key) {
    mpack_node_t n;
    if (mpack_node_type(map) != mpack_type_map) return 0;
    n = mpack_node_map_cstr_optional(map, key);
    if (mpack_node_type(n) == mpack_type_int)  return mpack_node_i64(n);
    if (mpack_node_type(n) == mpack_type_uint) return (int64_t)mpack_node_u64(n);
    return 0;
}

static int str_eq(mpack_node_t n, const char *s) {
    return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
           memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

void mg_status_read_maint(mpack_node_t result, mg_status_signals_t *s) {
    mpack_node_t pending, by_kind, rec;
    if (mpack_node_type(result) != mpack_type_map) return;
    s->have_maint = 1;
    s->inserts_since_apply_safe = int_at(result, "inserts_since_apply_safe");
    s->inserts_since_scan = int_at(result, "inserts_since_scan");
    pending = mpack_node_map_cstr_optional(result, "pending");
    s->pending_total = int_at(pending, "total");
    by_kind = mpack_node_type(pending) == mpack_type_map
                  ? mpack_node_map_cstr_optional(pending, "by_kind") : pending;
    /* a conflict between two notes is the most urgent thing to settle,
     * whether an agent or the scan heuristic flagged it */
    s->contradictions = int_at(by_kind, "contradiction") + int_at(by_kind, "possible_contradiction");
    s->src_changed = int_at(by_kind, "source_changed");
    s->src_removed = int_at(by_kind, "source_removed");
    rec = mpack_node_map_cstr_optional(result, "recommended");
    if (mpack_node_type(rec) == mpack_type_array) {
        for (size_t i = 0; i < mpack_node_array_length(rec); i++) {
            mpack_node_t r = mpack_node_array_at(rec, i);
            if (str_eq(r, "apply-safe")) s->rec_apply_safe = 1;
            if (str_eq(r, "scan"))       s->rec_scan = 1;
        }
    }
}

/* ---------- gather ---------- */

static int is_git_root(const char *root) {
    char p[MG_CLI_PATH];
    struct stat sb;
    if (snprintf(p, sizeof(p), "%s/.git", root) >= (int)sizeof(p)) return 0;
    return stat(p, &sb) == 0;
}

int mg_status_gather(const char *dir, int use_daemon, mg_status_t *s) {
    int rc;
    memset(s, 0, sizeof(*s));
    rc = mg_project_resolve(dir, &s->ctx);
    if (rc == -1) return -1;
    if (rc != 0) return -2;
    if (mg_project_state_load(s->ctx.state_file, &s->st) != 0) return -2;
    s->prov_rc = mg_project_provenance(s->ctx.db, s->ctx.project, &s->files, &s->nodes,
                                       s->prov_error, sizeof(s->prov_error));

    s->sig.git = is_git_root(s->ctx.root);
    s->sig.bootstrapped = s->st.runs > 0;
    for (int p = 1; p <= 3; p++) {
        for (size_t i = 0; i < s->st.n_topics; i++) {
            const mg_project_topic_t *t = &s->st.topics[i];
            if (strcmp(t->state, MG_PROJECT_TOPIC_PENDING) != 0) continue;
            if (t->priority == p) {
                s->sig.pending_topics++;
                if (!s->sig.next_topic) s->sig.next_topic = t->name;
            }
        }
    }

    if (!use_daemon) {
        s->maint_error = "not requested";
    } else {
        s->daemon_checked = 1;
        char *args = NULL;
        size_t len = 0;
        mpack_writer_t w;
        mpack_writer_init_growable(&w, &args, &len);
        mpack_start_map(&w, 0);
        mpack_finish_map(&w);
        if (mpack_writer_destroy(&w) != mpack_ok) {
            free(args);
            s->maint_error = "request encode failed";
        } else {
            rc = mg_cli_call_opts("maintain_status", args, len, &s->maint_tree, &s->maint_resp,
                                  MG_CLI_NO_AUTOSTART | MG_CLI_QUIET);
            free(args);
            s->daemon_running = rc != 1;
            if (rc == 0) {
                mg_status_read_maint(
                    mpack_node_map_cstr_optional(mpack_tree_root(&s->maint_tree), "result"),
                    &s->sig);
            } else {
                if (s->maint_resp) {
                    mpack_tree_destroy(&s->maint_tree);
                    free(s->maint_resp);
                    s->maint_resp = NULL;
                }
                s->maint_error = rc == 3
                    ? "the daemon refused maintain_status (older graftd?)"
                    : "daemon not running (graft status does not start it)";
            }
        }
    }
    s->n_steps = mg_status_plan(&s->sig, s->steps, MG_STATUS_MAX_STEPS);
    return 0;
}

void mg_status_free(mg_status_t *s) {
    if (s->maint_resp) {
        mpack_tree_destroy(&s->maint_tree);
        free(s->maint_resp);
        s->maint_resp = NULL;
    }
    mg_project_state_free(&s->st);
}

/* ---------- report ---------- */

static void copy_node(mpack_writer_t *w, mpack_node_t n) {
    switch (mpack_node_type(n)) {
    case mpack_type_bool:   mpack_write_bool(w, mpack_node_bool(n)); return;
    case mpack_type_int:    mpack_write_i64(w, mpack_node_i64(n)); return;
    case mpack_type_uint:   mpack_write_u64(w, mpack_node_u64(n)); return;
    case mpack_type_float:  mpack_write_float(w, mpack_node_float(n)); return;
    case mpack_type_double: mpack_write_double(w, mpack_node_double(n)); return;
    case mpack_type_str:
        mpack_write_str(w, mpack_node_str(n), (uint32_t)mpack_node_strlen(n));
        return;
    case mpack_type_array:
        mpack_build_array(w);
        for (size_t i = 0; i < mpack_node_array_length(n); i++)
            copy_node(w, mpack_node_array_at(n, i));
        mpack_complete_array(w);
        return;
    case mpack_type_map:
        mpack_build_map(w);
        for (size_t i = 0; i < mpack_node_map_count(n); i++) {
            copy_node(w, mpack_node_map_key_at(n, i));
            copy_node(w, mpack_node_map_value_at(n, i));
        }
        mpack_complete_map(w);
        return;
    default:
        mpack_write_nil(w);
        return;
    }
}

static const char *priority_name(int p) {
    return p == 1 ? "high" : p == 3 ? "low" : "normal";
}

int mg_status_report(const mg_status_t *s, char **out, size_t *out_len) {
    mpack_writer_t w;
    int64_t c_cov = 0, c_skip = 0;
    mpack_node_t maint;
    memset(&maint, 0, sizeof(maint));
    if (s->maint_resp) {
        maint = mpack_node_map_cstr_optional(mpack_tree_root((mpack_tree_t *)&s->maint_tree),
                                             "result");
    }
    for (size_t i = 0; i < s->st.n_topics; i++) {
        if (!strcmp(s->st.topics[i].state, MG_PROJECT_TOPIC_COVERED)) c_cov++;
        else if (!strcmp(s->st.topics[i].state, MG_PROJECT_TOPIC_SKIPPED)) c_skip++;
    }
    *out = NULL;
    *out_len = 0;
    mpack_writer_init_growable(&w, out, out_len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "status"); mpack_write_int(&w, 0);
    mpack_write_cstr(&w, "result");
    mpack_build_map(&w);

    mpack_write_cstr(&w, "project");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "name");         mpack_write_cstr(&w, s->ctx.project);
    mpack_write_cstr(&w, "root");         mpack_write_cstr(&w, s->ctx.root);
    mpack_write_cstr(&w, "profile");      mpack_write_cstr(&w, s->ctx.profile);
    mpack_write_cstr(&w, "git");          mpack_write_bool(&w, s->sig.git != 0);
    mpack_write_cstr(&w, "bootstrapped"); mpack_write_bool(&w, s->sig.bootstrapped != 0);
    mpack_write_cstr(&w, "runs");         mpack_write_int(&w, s->st.runs);
    mpack_write_cstr(&w, "last_run_at");
    if (s->st.last_run_at > 0) mpack_write_int(&w, s->st.last_run_at);
    else                       mpack_write_nil(&w);
    mpack_write_cstr(&w, "topics");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "covered"); mpack_write_int(&w, c_cov);
    mpack_write_cstr(&w, "pending"); mpack_write_int(&w, s->sig.pending_topics);
    mpack_write_cstr(&w, "skipped"); mpack_write_int(&w, c_skip);
    mpack_complete_map(&w);
    mpack_write_cstr(&w, "next_topic");
    if (s->sig.next_topic) mpack_write_cstr(&w, s->sig.next_topic);
    else                   mpack_write_nil(&w);
    mpack_write_cstr(&w, "provenance");
    if (s->prov_rc < 0) {
        mpack_write_nil(&w);
    } else {
        mpack_build_map(&w);
        mpack_write_cstr(&w, "files"); mpack_write_int(&w, s->files);
        mpack_write_cstr(&w, "nodes"); mpack_write_int(&w, s->nodes);
        mpack_complete_map(&w);
    }
    mpack_complete_map(&w);

    mpack_write_cstr(&w, "daemon");
    mpack_write_cstr(&w, s->daemon_running ? "running" : s->daemon_checked ? "not running"
                                                                           : "not checked");
    mpack_write_cstr(&w, "maintenance");
    if (s->sig.have_maint && s->maint_resp) {
        copy_node(&w, maint);
    } else {
        mpack_write_nil(&w);
        mpack_write_cstr(&w, "maintenance_error");
        mpack_write_cstr(&w, s->maint_error ? s->maint_error : "unavailable");
    }
    mpack_write_cstr(&w, "sources");
    if (s->sig.have_maint && s->maint_resp) {
        int64_t last_scan = int_at(maint, "last_scan_at");
        mpack_build_map(&w);
        mpack_write_cstr(&w, "changed"); mpack_write_int(&w, s->sig.src_changed);
        mpack_write_cstr(&w, "removed"); mpack_write_int(&w, s->sig.src_removed);
        mpack_write_cstr(&w, "as_of");
        if (last_scan > 0) mpack_write_int(&w, last_scan);
        else               mpack_write_nil(&w);
        mpack_complete_map(&w);
    } else {
        mpack_write_nil(&w);
    }

    mpack_write_cstr(&w, "next");
    mpack_build_array(&w);
    for (size_t i = 0; i < s->n_steps; i++) {
        const mg_status_step_t *st = &s->steps[i];
        mpack_build_map(&w);
        mpack_write_cstr(&w, "action");   mpack_write_cstr(&w, st->action);
        mpack_write_cstr(&w, "priority"); mpack_write_cstr(&w, priority_name(st->priority));
        mpack_write_cstr(&w, "reason");   mpack_write_cstr(&w, st->reason);
        mpack_write_cstr(&w, "command");  mpack_write_cstr(&w, st->command);
        mpack_complete_map(&w);
    }
    mpack_complete_array(&w);

    mpack_complete_map(&w);
    mpack_complete_map(&w);
    if (mpack_writer_destroy(&w) != mpack_ok) {
        free(*out);
        *out = NULL;
        *out_len = 0;
        return 1;
    }
    return 0;
}

/* ---------- command ---------- */

int mg_status_cmd(int argc, char **argv) {
    static mg_status_t s;
    const char *dir = ".";
    char *out = NULL;
    size_t out_len = 0;
    int rc;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) dir = argv[++i];
        else {
            fprintf(stderr, "graft status: unknown option or missing value: '%s'\n", argv[i]);
            return 2;
        }
    }
    rc = mg_status_gather(dir, 1, &s);
    if (rc == -1) {
        fprintf(stderr, "graft status: not a directory: %s\n", dir);
        return 2;
    }
    if (rc != 0) {
        fprintf(stderr, "graft status: cannot read the project state (%s)\n",
                s.ctx.state_file[0] ? s.ctx.state_file : "profile DB unresolved");
        mg_status_free(&s);
        return 1;
    }
    rc = mg_status_report(&s, &out, &out_len);
    if (rc == 0) rc = mg_cli_print_built(out, out_len);
    free(out);
    mg_status_free(&s);
    return rc;
}
