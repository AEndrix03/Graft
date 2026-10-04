/* graft CLI — Claude Code plugin hooks (issue #6).
 *
 *   graft hook session-start   (SessionStart: one housekeeping hint)
 *   graft hook prompt          (UserPromptSubmit: a STRONG note as context)
 *
 * The plugin's hooks/hooks.json runs these, so no bash or PowerShell script
 * has to work on every platform: the binary reads the hook JSON on stdin
 * and answers with additionalContext, or with nothing at all.
 *
 * Both are cheap and fail silent. Neither starts the daemon: a cold start
 * loads the embedding model (seconds), which must not happen inside the
 * prompt path. session-start reads the project state like `graft status`;
 * prompt sends one `query` to a daemon that is already up and injects the
 * note only on a STRONG hit, so a miss or a weak match costs the agent's
 * context nothing.
 */

#include "hook.h"
#include "client.h"
#include "status.h"
#include "mpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

#define HOOK_STDIN_MAX   (1u << 20)   /* a prompt is never this big; refuse more */
#define HOOK_QUERY_MAX   1000         /* bytes of the prompt sent as the query */
#define HOOK_BODY_MAX    800          /* bytes of the note body injected */
#define HOOK_TEXT_CAP    4096

/* ---------- JSON in ---------- */

typedef struct {
    const char *p;
    const char *end;
} jcur_t;

static void ws(jcur_t *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int hex4(jcur_t *c, unsigned *out) {
    unsigned v = 0;
    if (c->end - c->p < 4) return -1;
    for (int i = 0; i < 4; i++) {
        int h = hexval(c->p[i]);
        if (h < 0) return -1;
        v = (v << 4) | (unsigned)h;
    }
    c->p += 4;
    *out = v;
    return 0;
}

static size_t utf8_put(unsigned cp, char *o) {
    if (cp < 0x80)    { o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses the string at c->p (on its opening quote). out NULL: skip only.
 * A decoded string is never longer than its escaped form. */
static int jstring(jcur_t *c, char **out) {
    const char *start;
    char *d = NULL;
    size_t n = 0;
    if (c->p >= c->end || *c->p != '"') return -1;
    start = ++c->p;
    if (out) {
        const char *q = start;
        while (q < c->end && *q != '"') q += (*q == '\\') ? 2 : 1;
        if (q >= c->end) return -1;
        d = (char *)malloc((size_t)(q - start) + 1);
        if (!d) return -1;
    }
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;
        if ((unsigned char)ch < 0x20) goto bad;
        if (ch != '\\') {
            if (d) d[n++] = ch;
            continue;
        }
        if (c->p >= c->end) goto bad;
        ch = *c->p++;
        switch (ch) {
        case '"': case '\\': case '/': if (d) d[n++] = ch; break;
        case 'b': if (d) d[n++] = '\b'; break;
        case 'f': if (d) d[n++] = '\f'; break;
        case 'n': if (d) d[n++] = '\n'; break;
        case 'r': if (d) d[n++] = '\r'; break;
        case 't': if (d) d[n++] = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (hex4(c, &cp) != 0) goto bad;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                    c->p += 2;
                    if (hex4(c, &lo) != 0) goto bad;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else cp = 0xFFFD;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            /* "\uXXXX" is 6 bytes, its UTF-8 at most 3; a pair is 12 for 4 */
            if (d) n += utf8_put(cp, d + n);
            break;
        }
        default:
            goto bad;
        }
    }
    if (c->p >= c->end) goto bad;
    c->p++;
    if (d) {
        d[n] = '\0';
        *out = d;
    }
    return 0;
bad:
    free(d);
    return -1;
}

static int jskip(jcur_t *c, int depth) {
    ws(c);
    if (c->p >= c->end || depth > 64) return -1;
    if (*c->p == '"') return jstring(c, NULL);
    if (*c->p == '{' || *c->p == '[') {
        char close = *c->p == '{' ? '}' : ']';
        int obj = *c->p == '{';
        c->p++;
        ws(c);
        if (c->p < c->end && *c->p == close) { c->p++; return 0; }
        for (;;) {
            if (obj) {
                ws(c);
                if (jstring(c, NULL) != 0) return -1;
                ws(c);
                if (c->p >= c->end || *c->p != ':') return -1;
                c->p++;
            }
            if (jskip(c, depth + 1) != 0) return -1;
            ws(c);
            if (c->p >= c->end) return -1;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == close) { c->p++; return 0; }
            return -1;
        }
    }
    /* number / true / false / null */
    {
        const char *s = c->p;
        while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' &&
               *c->p != ' ' && *c->p != '\t' && *c->p != '\n' && *c->p != '\r')
            c->p++;
        return c->p > s ? 0 : -1;
    }
}

int mg_hook_json_string(const char *json, size_t len, const char *key, char **out) {
    jcur_t c = { json, json + len };
    *out = NULL;
    ws(&c);
    if (c.p >= c.end || *c.p != '{') return -1;
    c.p++;
    ws(&c);
    if (c.p < c.end && *c.p == '}') return 1;
    for (;;) {
        char *k = NULL;
        int match;
        ws(&c);
        if (jstring(&c, &k) != 0) return -1;
        match = !strcmp(k, key);
        free(k);
        ws(&c);
        if (c.p >= c.end || *c.p != ':') return -1;
        c.p++;
        ws(&c);
        if (match) {
            if (c.p < c.end && *c.p == '"') return jstring(&c, out) == 0 ? 0 : -1;
            return 1;
        }
        if (jskip(&c, 0) != 0) return -1;
        ws(&c);
        if (c.p >= c.end) return -1;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == '}') return 1;
        return -1;
    }
}

/* ---------- JSON out ---------- */

static size_t put(char *buf, size_t len, size_t cap, const char *s, size_t n) {
    if (len >= cap) return cap;
    if (n >= cap - len) {
        memcpy(buf + len, s, cap - len - 1);
        buf[cap - 1] = '\0';
        return cap;
    }
    memcpy(buf + len, s, n);
    buf[len + n] = '\0';
    return len + n;
}

static size_t put_s(char *buf, size_t len, size_t cap, const char *s) {
    return put(buf, len, cap, s, strlen(s));
}

size_t mg_hook_json_escape(char *buf, size_t len, size_t cap, const char *s) {
    len = put(buf, len, cap, "\"", 1);
    for (; *s && len < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        char esc[8];
        switch (ch) {
        case '"':  len = put(buf, len, cap, "\\\"", 2); break;
        case '\\': len = put(buf, len, cap, "\\\\", 2); break;
        case '\n': len = put(buf, len, cap, "\\n", 2); break;
        case '\r': len = put(buf, len, cap, "\\r", 2); break;
        case '\t': len = put(buf, len, cap, "\\t", 2); break;
        default:
            if (ch < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", ch);
                len = put(buf, len, cap, esc, 6);
            } else {
                len = put(buf, len, cap, (const char *)&ch, 1);
            }
        }
    }
    return put(buf, len, cap, "\"", 1);
}

static void emit(const char *event, const char *text) {
    /* the text is at most HOOK_TEXT_CAP; escaping at most doubles it */
    static char out[3 * HOOK_TEXT_CAP];
    size_t n = 0;
    n = put_s(out, n, sizeof(out), "{\"hookSpecificOutput\":{\"hookEventName\":");
    n = mg_hook_json_escape(out, n, sizeof(out), event);
    n = put_s(out, n, sizeof(out), ",\"additionalContext\":");
    n = mg_hook_json_escape(out, n, sizeof(out), text);
    n = put_s(out, n, sizeof(out), "}}\n");
    if (n >= sizeof(out)) return;   /* never print half an object */
    fwrite(out, 1, n, stdout);
    fflush(stdout);
}

/* ---------- texts ---------- */

int mg_hook_prompt_worth(const char *p) {
    size_t chars = 0;
    int words = 0, in_word = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '/' || *p == '!' || *p == '#') return 0;   /* command, bash, memory */
    for (; *p; p++) {
        int space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';
        if (!space) chars++;
        if (!space && !in_word) words++;
        in_word = !space;
    }
    return words >= 3 && chars >= 12;
}

size_t mg_hook_session_text(const mg_status_step_t *steps, size_t n, char *buf, size_t cap) {
    size_t len = 0;
    if (n == 0 || cap == 0) return 0;
    buf[0] = '\0';
    len = put_s(buf, len, cap, "graft: housekeeping due in this project - ");
    for (size_t i = 0; i < n; i++) {
        char item[320];
        int k = snprintf(item, sizeof(item), "%s%s (%s; run: %s)", i ? "; " : "",
                         steps[i].action, steps[i].reason, steps[i].command);
        if (k > 0) len = put(buf, len, cap, item, (size_t)k < sizeof(item) ? (size_t)k : sizeof(item) - 1);
    }
    len = put_s(buf, len, cap,
              ". Handle it as the graft skill's lifecycle says: bounded, alongside or after the "
              "user's task, never blocking it and without asking the user. `graft status` has "
              "the details.");
    return len < cap ? len : cap - 1;
}

/* Bytes of s to keep so that at most max bytes remain, on a UTF-8 boundary. */
static size_t utf8_cut(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n <= max) return n;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

size_t mg_hook_prompt_text(const char *id, const char *title, const char *body,
                           char *buf, size_t cap) {
    size_t len = 0, bn = utf8_cut(body ? body : "", HOOK_BODY_MAX);
    char head[512];
    int k;
    if (cap == 0) return 0;
    buf[0] = '\0';
    k = snprintf(head, sizeof(head),
                 "graft memory: a stored note is a STRONG match for this prompt. Close is not "
                 "proven - check it against the code before relying on it.\n[%s] %s\n",
                 id ? id : "?", title ? title : "");
    if (k > 0) len = put(buf, len, cap, head, (size_t)k < sizeof(head) ? (size_t)k : sizeof(head) - 1);
    len = put(buf, len, cap, body ? body : "", bn);
    if (body && bn < strlen(body)) len = put_s(buf, len, cap, " [...]");
    k = snprintf(head, sizeof(head), "\nFull note: graft get %s", id ? id : "?");
    if (k > 0) len = put(buf, len, cap, head, (size_t)k < sizeof(head) ? (size_t)k : sizeof(head) - 1);
    return len < cap ? len : cap - 1;
}

/* ---------- command ---------- */

static int env_off(const char *name) {
    const char *v = getenv(name);
    return v && (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no"));
}

/* All of stdin (<= HOOK_STDIN_MAX), NUL-terminated; NULL on failure. */
static char *read_stdin(size_t *len) {
    size_t cap = 4096, n = 0;
    char *buf = (char *)malloc(cap + 1);
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
    if (!buf) return NULL;
    for (;;) {
        size_t r;
        if (n == cap) {
            char *nb;
            if (cap >= HOOK_STDIN_MAX) { free(buf); return NULL; }
            cap *= 2;
            nb = (char *)realloc(buf, cap + 1);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        r = fread(buf + n, 1, cap - n, stdin);
        n += r;
        if (r == 0) break;
    }
    if (ferror(stdin)) { free(buf); return NULL; }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static void hook_session_start(const char *in, size_t len) {
    static mg_status_t s;
    static char text[HOOK_TEXT_CAP];
    char *cwd = NULL;
    int rc = -1;
    (void)mg_hook_json_string(in, len, "cwd", &cwd);
    if (cwd && *cwd) {
        rc = mg_status_gather(cwd, 1, &s);
        if (rc != 0) mg_status_free(&s);
    }
    /* Claude Code starts the hook in the project directory anyway */
    if (rc != 0) rc = mg_status_gather(".", 1, &s);
    if (rc == 0 && mg_hook_session_text(s.steps, s.n_steps, text, sizeof(text)) > 0) {
        emit("SessionStart", text);
    }
    mg_status_free(&s);
    free(cwd);
}

static void hook_prompt(const char *in, size_t len) {
    static char text[HOOK_TEXT_CAP];
    char *prompt = NULL, *args = NULL, *id = NULL, *title = NULL, *body = NULL;
    size_t args_len = 0;
    mpack_writer_t w;
    mpack_tree_t tree;
    void *resp = NULL;

    if (env_off("GRAFT_HOOK_PROMPT")) return;
    if (mg_hook_json_string(in, len, "prompt", &prompt) != 0) return;
    if (!mg_hook_prompt_worth(prompt)) {
        free(prompt);
        return;
    }
    mpack_writer_init_growable(&w, &args, &args_len);
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "text");
    mpack_write_str(&w, prompt, (uint32_t)utf8_cut(prompt, HOOK_QUERY_MAX));
    mpack_finish_map(&w);
    free(prompt);
    if (mpack_writer_destroy(&w) != mpack_ok) {
        free(args);
        return;
    }
    if (mg_cli_call_opts("query", args, args_len, &tree, &resp,
                         MG_CLI_NO_AUTOSTART | MG_CLI_QUIET) == 0) {
        mpack_node_t r = mpack_node_map_cstr_optional(mpack_tree_root(&tree), "result");
        mpack_node_t hit = mpack_node_type(r) == mpack_type_map
                               ? mpack_node_map_cstr_optional(r, "hit") : r;
        if (mpack_node_type(hit) == mpack_type_str && mpack_node_strlen(hit) == 6 &&
            !memcmp(mpack_node_str(hit), "STRONG", 6)) {
            mpack_node_t f;
#define DUP(var, key) \
            f = mpack_node_map_cstr_optional(r, key); \
            if (mpack_node_type(f) == mpack_type_str) var = mpack_node_cstr_alloc(f, 1u << 20);
            DUP(id, "id_hex")
            DUP(title, "title")
            DUP(body, "body")
#undef DUP
            if (id && mg_hook_prompt_text(id, title, body, text, sizeof(text)) > 0)
                emit("UserPromptSubmit", text);
            free(id);
            free(title);
            free(body);
        }
    }
    if (resp) {
        mpack_tree_destroy(&tree);
        free(resp);
    }
    free(args);
}

int mg_hook_cmd(int argc, char **argv) {
    char *in;
    size_t len = 0;
    int session;
    if (argc != 3 || (strcmp(argv[2], "session-start") != 0 && strcmp(argv[2], "prompt") != 0)) {
        fprintf(stderr, "usage: graft hook <session-start|prompt>   (reads the hook JSON on stdin)\n");
        return 2;
    }
    if (env_off("GRAFT_HOOKS")) return 0;
    session = !strcmp(argv[2], "session-start");
    in = read_stdin(&len);
    if (!in) return 0;
    if (session) hook_session_start(in, len);
    else         hook_prompt(in, len);
    free(in);
    return 0;
}
