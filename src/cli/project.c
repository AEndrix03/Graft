/* graft CLI — bootstrap coverage state (issue #3).
 *
 *   graft project status [--root DIR]
 *   graft project mark   [--root DIR] [--topic NAME]... [--state S]
 *                        [--priority high|normal|low] [--note TEXT] [--run]
 *   graft project reset  [--root DIR]
 *
 * The bootstrap itself is an agent workflow (`/learn bootstrap`): only the
 * agent can judge what is worth remembering. This command keeps the cheap
 * state it needs between sessions, so a re-run knows what is left without
 * rescanning the repository:
 *
 *   - a topic map: each topic covered / pending / skipped, with a priority
 *     and a short note (where to look, why it was skipped);
 *   - how many bootstrap runs completed and when the last one did;
 *   - how many of the project's files already back a node, read from the
 *     provenance tables (issue #4).
 *
 * The project is resolved like `insert --source file:` does (normalized
 * origin remote, else the root path). The topic map lives next to the DB
 * it describes, <db dir>/projects/<hash>.tsv, so each profile has its own.
 * Nothing here talks to the daemon or loads the model: the provenance
 * counts come from a read-only SQLite connection, safe beside a running
 * daemon (WAL). That keeps `status` cheap enough for every session start.
 */

#include "project.h"
#include "client.h"
#include "profile.h"
#include "graft/source.h"
#include "graft/types.h"
#include "mpack.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
#  define mk_dir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define mk_dir(p) mkdir((p), 0755)
#endif

#define MG_CLI_PATH       4096
#define MG_PROJECT_MAX_TOPICS_ARG 64
#define MG_PROJECT_NAME_MAX 200
#define MG_PROJECT_NOTE_MAX 500

static const char STATE_MAGIC[] = "graft-project-state\t1";

/* ---------- state file ---------- */

int mg_project_state_path(const char *db_path, const char *project, char *out, size_t cap) {
    mg_hash_t h;
    char hex[33];
    size_t dn;
    const char *slash = NULL;
    if (!db_path || !*db_path || !project) return -1;
    for (const char *p = db_path; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    dn = slash ? (size_t)(slash - db_path) : 0;
    mg_blake3((const uint8_t *)project, strlen(project), h);
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    if (slash) {
        if (snprintf(out, cap, "%.*s/projects/%s.tsv", (int)dn, db_path, hex) >= (int)cap) return -1;
    } else {
        if (snprintf(out, cap, "projects/%s.tsv", hex) >= (int)cap) return -1;
    }
    return 0;
}

void mg_project_state_free(mg_project_state_t *st) {
    for (size_t i = 0; i < st->n_topics; i++) {
        free(st->topics[i].name);
        free(st->topics[i].note);
    }
    free(st->topics);
    memset(st, 0, sizeof(*st));
}

static int state_valid(const char *s) {
    return !strcmp(s, MG_PROJECT_TOPIC_COVERED) || !strcmp(s, MG_PROJECT_TOPIC_PENDING) ||
           !strcmp(s, MG_PROJECT_TOPIC_SKIPPED);
}

/* Copy of s (at most max bytes) with tabs / newlines turned into spaces and
 * surrounding blanks trimmed, so a value can never break the line format. */
static char *clean_dup(const char *s, size_t max) {
    size_t n;
    char *d;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    n = strlen(s);
    if (n > max) n = max;
    d = (char *)malloc(n + 1);
    if (!d) return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        d[i] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
    }
    while (n > 0 && d[n - 1] == ' ') n--;
    d[n] = '\0';
    return d;
}

static int push_topic(mg_project_state_t *st, const char *name, const char *state,
                      int priority, int64_t updated_at, const char *note) {
    mg_project_topic_t *t;
    if (st->n_topics == st->cap) {
        size_t nc = st->cap ? st->cap * 2 : 16;
        mg_project_topic_t *nt = (mg_project_topic_t *)realloc(st->topics, nc * sizeof(*nt));
        if (!nt) return -2;
        st->topics = nt;
        st->cap = nc;
    }
    t = &st->topics[st->n_topics];
    memset(t, 0, sizeof(*t));
    t->name = clean_dup(name, MG_PROJECT_NAME_MAX);
    t->note = clean_dup(note ? note : "", MG_PROJECT_NOTE_MAX);
    if (!t->name || !t->note) {
        free(t->name);
        free(t->note);
        return -2;
    }
    snprintf(t->state, sizeof(t->state), "%s", state);
    t->priority = priority >= 1 && priority <= 3 ? priority : 2;
    t->updated_at = updated_at;
    st->n_topics++;
    return 0;
}

/* Splits line in place on tabs; returns the number of fields (<= max). */
static int split_tabs(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '\t') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

int mg_project_state_load(const char *path, mg_project_state_t *st) {
    char line[2048];
    FILE *fp;
    memset(st, 0, sizeof(*st));
    errno = 0;
    fp = fopen(path, "rb");
    if (!fp) return errno == ENOENT ? 0 : -1;   /* never marked: empty state */
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        char *f[6];
        int nf;
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            int c;   /* over-long line: drop the rest of it */
            while ((c = fgetc(fp)) != EOF && c != '\n') {}
            continue;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        nf = split_tabs(line, f, 6);
        if (nf == 2 && !strcmp(f[0], "runs")) {
            st->runs = strtoll(f[1], NULL, 10);
        } else if (nf == 2 && !strcmp(f[0], "last_run_at")) {
            st->last_run_at = strtoll(f[1], NULL, 10);
        } else if (nf >= 5 && !strcmp(f[0], "topic") && state_valid(f[1]) && *f[4]) {
            if (push_topic(st, f[4], f[1], atoi(f[2]), strtoll(f[3], NULL, 10),
                           nf == 6 ? f[5] : "") != 0) {
                fclose(fp);
                mg_project_state_free(st);
                return -1;
            }
        }
    }
    if (ferror(fp)) {
        fclose(fp);
        mg_project_state_free(st);
        return -1;
    }
    fclose(fp);
    return 0;
}

int mg_project_state_save(const char *path, const char *project, const mg_project_state_t *st) {
    char dir[MG_CLI_PATH], tmp[MG_CLI_PATH];
    const char *slash = NULL;
    FILE *fp;
    int rc = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (slash) {
        size_t dn = (size_t)(slash - path);
        if (dn >= sizeof(dir)) return -1;
        memcpy(dir, path, dn);
        dir[dn] = '\0';
        (void)mk_dir(dir);   /* the DB dir above it already exists */
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    fp = fopen(tmp, "wb");
    if (!fp) return -1;
    fprintf(fp, "%s\nproject\t", STATE_MAGIC);
    for (const char *p = project; *p; p++) fputc((*p == '\t' || *p == '\n') ? ' ' : *p, fp);
    fprintf(fp, "\nruns\t%lld\nlast_run_at\t%lld\n", (long long)st->runs, (long long)st->last_run_at);
    for (size_t i = 0; i < st->n_topics; i++) {
        const mg_project_topic_t *t = &st->topics[i];
        fprintf(fp, "topic\t%s\t%d\t%lld\t%s\t%s\n", t->state, t->priority,
                (long long)t->updated_at, t->name, t->note);
    }
    if (ferror(fp)) rc = -1;
    if (fclose(fp) != 0) rc = -1;
    if (rc != 0) {
        remove(tmp);
        return -1;
    }
#ifdef _WIN32
    remove(path);   /* rename() does not replace on Windows */
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int mg_project_state_mark(mg_project_state_t *st, const char *name, const char *state,
                          int priority, const char *note, int64_t now_ms) {
    char *key;
    size_t i;
    int drop;
    if (!name || !state) return -1;
    drop = !strcmp(state, "drop");
    if (!drop && !state_valid(state)) return -1;
    key = clean_dup(name, MG_PROJECT_NAME_MAX);
    if (!key) return -2;
    if (!*key) {
        free(key);
        return -1;
    }
    for (i = 0; i < st->n_topics; i++) {
        if (!strcmp(st->topics[i].name, key)) break;
    }
    if (i == st->n_topics) {
        int rc = drop ? 0 : push_topic(st, key, state, priority, now_ms, note);
        free(key);
        return rc;
    }
    free(key);
    if (drop) {
        free(st->topics[i].name);
        free(st->topics[i].note);
        memmove(&st->topics[i], &st->topics[i + 1], (st->n_topics - i - 1) * sizeof(*st->topics));
        st->n_topics--;
        return 0;
    }
    if (note) {
        char *nn = clean_dup(note, MG_PROJECT_NOTE_MAX);
        if (!nn) return -2;
        free(st->topics[i].note);
        st->topics[i].note = nn;
    }
    snprintf(st->topics[i].state, sizeof(st->topics[i].state), "%s", state);
    if (priority >= 1 && priority <= 3) st->topics[i].priority = priority;
    st->topics[i].updated_at = now_ms;
    return 0;
}

/* ---------- provenance counts ---------- */

int mg_project_provenance(const char *db_path, const char *project,
                          int64_t *files, int64_t *nodes, char *err, size_t err_cap) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    FILE *probe;
    int rc = -1;
    *files = 0;
    *nodes = 0;
    if (err_cap) err[0] = '\0';
    probe = fopen(db_path, "rb");
    if (!probe) return 1;
    fclose(probe);
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (err_cap) snprintf(err, err_cap, "cannot open the DB: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 2000);
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(DISTINCT s.id), COUNT(DISTINCT ns.node_id) "
            "FROM sources s "
            "JOIN node_sources ns ON ns.source_id = s.id "
            "JOIN nodes n ON n.id = ns.node_id "
            "WHERE s.project = ?1 AND s.kind = 'file' AND n.state IN (0, 1)",  /* active or stale: not superseded, not retired */
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *files = sqlite3_column_int64(stmt, 0);
            *nodes = sqlite3_column_int64(stmt, 1);
            rc = 0;
        }
    }
    if (rc != 0 && err_cap) {
        const char *m = sqlite3_errmsg(db);
        /* a DB the daemon has not opened since provenance shipped */
        if (strstr(m, "no such table")) snprintf(err, err_cap, "no provenance tables yet (%s)", m);
        else                            snprintf(err, err_cap, "%s", m);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

/* ---------- report ---------- */

static const char *priority_name(int p) {
    return p == 1 ? "high" : p == 3 ? "low" : "normal";
}

static int priority_parse(const char *s) {
    if (!strcmp(s, "high"))   return 1;
    if (!strcmp(s, "normal")) return 2;
    if (!strcmp(s, "low"))    return 3;
    return 0;
}

static void write_names(mpack_writer_t *w, const mg_project_state_t *st, const char *state) {
    mpack_build_array(w);
    for (size_t i = 0; i < st->n_topics; i++) {
        if (!strcmp(st->topics[i].state, state)) mpack_write_cstr(w, st->topics[i].name);
    }
    mpack_complete_array(w);
}

int mg_project_status_report(const char *project, const char *root, const char *profile,
                             const char *state_file, const mg_project_state_t *st,
                             int prov_rc, const char *prov_error, int64_t files, int64_t nodes,
                             char **out, size_t *out_len) {
    mpack_writer_t w;
    int64_t c_cov = 0, c_pend = 0, c_skip = 0;
    for (size_t i = 0; i < st->n_topics; i++) {
        const char *s = st->topics[i].state;
        if      (!strcmp(s, MG_PROJECT_TOPIC_COVERED)) c_cov++;
        else if (!strcmp(s, MG_PROJECT_TOPIC_PENDING)) c_pend++;
        else                                           c_skip++;
    }
    *out = NULL;
    *out_len = 0;
    mpack_writer_init_growable(&w, out, out_len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "status"); mpack_write_int(&w, 0);
    mpack_write_cstr(&w, "result");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "project");      mpack_write_cstr(&w, project);
    mpack_write_cstr(&w, "root");         mpack_write_cstr(&w, root);
    mpack_write_cstr(&w, "profile");      mpack_write_cstr(&w, profile);
    mpack_write_cstr(&w, "state_file");   mpack_write_cstr(&w, state_file);
    mpack_write_cstr(&w, "bootstrapped"); mpack_write_bool(&w, st->runs > 0);
    mpack_write_cstr(&w, "runs");         mpack_write_int(&w, st->runs);
    mpack_write_cstr(&w, "last_run_at");
    if (st->last_run_at > 0) mpack_write_int(&w, st->last_run_at);
    else                     mpack_write_nil(&w);
    mpack_write_cstr(&w, "provenance");
    if (prov_rc < 0) {
        mpack_write_nil(&w);
        mpack_write_cstr(&w, "provenance_error");
        mpack_write_cstr(&w, prov_error && *prov_error ? prov_error : "unreadable");
    } else {
        mpack_build_map(&w);
        mpack_write_cstr(&w, "files"); mpack_write_int(&w, files);
        mpack_write_cstr(&w, "nodes"); mpack_write_int(&w, nodes);
        mpack_complete_map(&w);
    }
    mpack_write_cstr(&w, "topics");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "covered"); mpack_write_int(&w, c_cov);
    mpack_write_cstr(&w, "pending"); mpack_write_int(&w, c_pend);
    mpack_write_cstr(&w, "skipped"); mpack_write_int(&w, c_skip);
    mpack_complete_map(&w);
    /* pending first by priority, insertion order within a priority */
    mpack_write_cstr(&w, "pending");
    mpack_build_array(&w);
    for (int p = 1; p <= 3; p++) {
        for (size_t i = 0; i < st->n_topics; i++) {
            const mg_project_topic_t *t = &st->topics[i];
            if (t->priority != p || strcmp(t->state, MG_PROJECT_TOPIC_PENDING) != 0) continue;
            mpack_build_map(&w);
            mpack_write_cstr(&w, "topic");      mpack_write_cstr(&w, t->name);
            mpack_write_cstr(&w, "priority");   mpack_write_cstr(&w, priority_name(t->priority));
            mpack_write_cstr(&w, "note");       mpack_write_cstr(&w, t->note);
            mpack_write_cstr(&w, "updated_at"); mpack_write_int(&w, t->updated_at);
            mpack_complete_map(&w);
        }
    }
    mpack_complete_array(&w);
    mpack_write_cstr(&w, "covered"); write_names(&w, st, MG_PROJECT_TOPIC_COVERED);
    mpack_write_cstr(&w, "skipped"); write_names(&w, st, MG_PROJECT_TOPIC_SKIPPED);
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

static void reject_arg(const char *sub, const char *arg) {
    fprintf(stderr, "graft project %s: unknown option or missing value: '%s'\n", sub, arg);
    exit(2);
}

static int project_usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  graft project status [--root DIR]\n"
        "  graft project mark [--root DIR] [--topic NAME]... [--state covered|pending|skipped|drop]\n"
        "                     [--priority high|normal|low] [--note TEXT] [--run]\n"
        "  graft project reset [--root DIR]\n");
    return 2;
}

static int64_t unix_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

typedef struct {
    char root[MG_CLI_PATH];
    char project[MG_CLI_PATH];
    char profile[128];
    char db[MG_CLI_PATH];
    char state_file[MG_CLI_PATH];
} ctx_t;

static int resolve(const char *sub, const char *dir, ctx_t *c) {
    const char *db = getenv("GRAFT_DB_PATH");
    if (mg_source_resolve_project(dir, c->root, sizeof(c->root),
                                  c->project, sizeof(c->project)) != 0) {
        fprintf(stderr, "graft project %s: not a directory: %s\n", sub, dir);
        return 2;
    }
    (void)mg_profile_active(c->profile, sizeof(c->profile));
    if (db && *db) {
        snprintf(c->db, sizeof(c->db), "%s", db);
    } else if (mg_profile_db_path(c->profile, c->db, sizeof(c->db), 1) != 0) {
        fprintf(stderr, "graft project %s: cannot resolve the profile DB\n", sub);
        return 1;
    }
    if (mg_project_state_path(c->db, c->project, c->state_file, sizeof(c->state_file)) != 0) {
        fprintf(stderr, "graft project %s: state path too long\n", sub);
        return 1;
    }
    return 0;
}

static int print_built(char *buf, size_t len) {
    mpack_tree_t tree;
    int rc = 0;
    mpack_tree_init_data(&tree, buf, len);
    mpack_tree_parse(&tree);
    if (mpack_tree_error(&tree) != mpack_ok) {
        fprintf(stderr, "output encode error\n");
        rc = 1;
    } else {
        mg_cli_print_value(mpack_tree_root(&tree), 0);
        printf("\n");
    }
    mpack_tree_destroy(&tree);
    return rc;
}

static int print_status(const ctx_t *c, const mg_project_state_t *st) {
    int64_t files = 0, nodes = 0;
    char err[256];
    int prov = mg_project_provenance(c->db, c->project, &files, &nodes, err, sizeof(err));
    char *out = NULL;
    size_t out_len = 0;
    int rc = mg_project_status_report(c->project, c->root, c->profile, c->state_file, st,
                                      prov, err, files, nodes, &out, &out_len);
    if (rc == 0) rc = print_built(out, out_len);
    free(out);
    return rc;
}

static int load_or_fail(const char *sub, const ctx_t *c, mg_project_state_t *st) {
    if (mg_project_state_load(c->state_file, st) != 0) {
        fprintf(stderr, "graft project %s: cannot read %s\n", sub, c->state_file);
        return 1;
    }
    return 0;
}

static int cmd_status(int argc, char **argv) {
    static ctx_t c;
    const char *dir = ".";
    mg_project_state_t st;
    int rc;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) dir = argv[++i];
        else reject_arg("status", argv[i]);
    }
    if ((rc = resolve("status", dir, &c)) != 0) return rc;
    if ((rc = load_or_fail("status", &c, &st)) != 0) return rc;
    rc = print_status(&c, &st);
    mg_project_state_free(&st);
    return rc;
}

static int cmd_mark(int argc, char **argv) {
    static ctx_t c;
    const char *dir = ".", *state = NULL, *note = NULL;
    const char *topics[MG_PROJECT_MAX_TOPICS_ARG];
    int n_topics = 0, priority = 0, run = 0, rc;
    mg_project_state_t st;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--topic") && i + 1 < argc) {
            if (n_topics == MG_PROJECT_MAX_TOPICS_ARG) {
                fprintf(stderr, "graft project mark: at most %d --topic\n", MG_PROJECT_MAX_TOPICS_ARG);
                return 2;
            }
            topics[n_topics++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--note") && i + 1 < argc) note = argv[++i];
        else if (!strcmp(argv[i], "--priority") && i + 1 < argc) {
            priority = priority_parse(argv[++i]);
            if (!priority) {
                fprintf(stderr, "graft project mark: --priority is high, normal or low\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--run")) run = 1;
        else reject_arg("mark", argv[i]);
    }
    if (n_topics == 0 && !run) {
        fprintf(stderr, "graft project mark: nothing to mark (--topic NAME --state S, or --run)\n");
        return 2;
    }
    if (n_topics == 0 && (state || note || priority)) {
        fprintf(stderr, "graft project mark: --state/--priority/--note need --topic\n");
        return 2;
    }
    if (n_topics > 0 && (!state || (strcmp(state, "drop") != 0 && !state_valid(state)))) {
        fprintf(stderr, "graft project mark: --state is covered, pending, skipped or drop\n");
        return 2;
    }
    if ((rc = resolve("mark", dir, &c)) != 0) return rc;
    if ((rc = load_or_fail("mark", &c, &st)) != 0) return rc;
    int64_t now = unix_ms();
    for (int i = 0; i < n_topics; i++) {
        int mrc = mg_project_state_mark(&st, topics[i], state, priority, note, now);
        if (mrc == -1) {
            fprintf(stderr, "graft project mark: empty topic name\n");
            mg_project_state_free(&st);
            return 2;
        }
        if (mrc != 0) {
            fprintf(stderr, "out of memory\n");
            mg_project_state_free(&st);
            return 1;
        }
    }
    if (run) {
        st.runs++;
        st.last_run_at = now;
    }
    if (mg_project_state_save(c.state_file, c.project, &st) != 0) {
        fprintf(stderr, "graft project mark: cannot write %s\n", c.state_file);
        mg_project_state_free(&st);
        return 1;
    }
    rc = print_status(&c, &st);
    mg_project_state_free(&st);
    return rc;
}

static int cmd_reset(int argc, char **argv) {
    static ctx_t c;
    const char *dir = ".";
    FILE *fp;
    int existed, rc;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) dir = argv[++i];
        else reject_arg("reset", argv[i]);
    }
    if ((rc = resolve("reset", dir, &c)) != 0) return rc;
    fp = fopen(c.state_file, "rb");
    existed = fp != NULL;
    if (fp) fclose(fp);
    if (existed && remove(c.state_file) != 0) {
        fprintf(stderr, "graft project reset: cannot remove %s\n", c.state_file);
        return 1;
    }
    char *out = NULL;
    size_t out_len = 0;
    mpack_writer_t w;
    mpack_writer_init_growable(&w, &out, &out_len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "status"); mpack_write_int(&w, 0);
    mpack_write_cstr(&w, "result");
    mpack_build_map(&w);
    mpack_write_cstr(&w, "project");    mpack_write_cstr(&w, c.project);
    mpack_write_cstr(&w, "state_file"); mpack_write_cstr(&w, c.state_file);
    mpack_write_cstr(&w, "removed");    mpack_write_bool(&w, existed);
    mpack_complete_map(&w);
    mpack_complete_map(&w);
    rc = mpack_writer_destroy(&w) == mpack_ok ? print_built(out, out_len) : 1;
    free(out);
    return rc;
}

int mg_project_cmd(int argc, char **argv) {
    if (argc < 3) return project_usage();
    if (!strcmp(argv[2], "status")) return cmd_status(argc, argv);
    if (!strcmp(argv[2], "mark"))   return cmd_mark(argc, argv);
    if (!strcmp(argv[2], "reset"))  return cmd_reset(argc, argv);
    return project_usage();
}
