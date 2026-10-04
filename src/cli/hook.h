#ifndef GRAFT_CLI_HOOK_H
#define GRAFT_CLI_HOOK_H

#include "status.h"

#include <stddef.h>

/* `graft hook <session-start|prompt>` (issue #6): the commands behind the
 * Claude Code plugin hooks. Each reads the hook's JSON on stdin and, when
 * there is something worth telling the agent, prints one
 * { "hookSpecificOutput": { "hookEventName", "additionalContext" } } object.
 * Otherwise it prints nothing. Whatever goes wrong at run time (daemon
 * down, bad input, old daemon) it stays silent and exits 0: a hook must
 * never get in the way of the prompt. Neither starts the daemon.
 *
 * Opt-out: GRAFT_HOOKS=0 turns both off, GRAFT_HOOK_PROMPT=0 only the
 * per-prompt lookup. argv[1] is "hook"; returns 2 only on a usage error. */
int mg_hook_cmd(int argc, char **argv);

/* ---- exposed for tests ---- */

/* The string value of a top-level key of a JSON object, decoded to UTF-8
 * in *out (free() it). Returns 0, 1 when the key is absent or not a
 * string, -1 on malformed JSON or allocation failure. */
int    mg_hook_json_string(const char *json, size_t len, const char *key, char **out);

/* Appends s as a JSON string literal (quotes included) to buf; returns the
 * new length, or cap when it did not fit (buf is then truncated). */
size_t mg_hook_json_escape(char *buf, size_t len, size_t cap, const char *s);

/* Whether a prompt is worth a graph lookup: at least 3 words and 12
 * characters, and not a slash command. */
int    mg_hook_prompt_worth(const char *prompt);

/* The one-line session hint for a plan; 0 when there is nothing to say. */
size_t mg_hook_session_text(const mg_status_step_t *steps, size_t n, char *buf, size_t cap);

/* The context injected for a STRONG hit; returns its length. */
size_t mg_hook_prompt_text(const char *id, const char *title, const char *body,
                           char *buf, size_t cap);

#endif
