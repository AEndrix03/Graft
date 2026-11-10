/* The contradiction heuristic of maintain scan (issue #22): positive
 * contradictions, paraphrases that must stay quiet, negation edge cases. */

#include "../src/maintain/divergence.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void expect(int cond, const char *what) {
  printf("%s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) g_failures++;
}

static mg_divergence_t div_of(const char *a, const char *b) {
  mg_divergence_t d;
  mg_divergence(a, b, &d);
  return d;
}

/* Default threshold of maintenance.contradiction_min. */
static int flagged(const char *a, const char *b) {
  return div_of(a, b).score >= 0.5;
}

static void test_contradictions(void) {
  mg_divergence_t d;
  expect(flagged("Retries are enabled by default for HTTP calls",
                 "Retries are disabled by default for HTTP calls"),
         "opposite terms: enabled / disabled");
  d = div_of("Retries are enabled by default", "Retries are disabled by default");
  expect(d.n_opposites == 1 && !strcmp(d.opposites, "enabled/disabled"),
         "opposites are reported side a first");
  expect(flagged("The cache is invalidated on every deploy",
                 "The cache is not invalidated on deploy"),
         "negation on one side only");
  expect(flagged("The worker restarts after a crash",
                 "The worker doesn't restart after a crash"),
         "contracted negation (n't)");
  expect(flagged("The worker restarts after a crash",
                 "The worker doesn\xe2\x80\x99t restart after a crash"),
         "contracted negation with a typographic apostrophe");
  expect(flagged("Il token di refresh viene ruotato a ogni login",
                 "Il token di refresh non viene ruotato al login"),
         "Italian negation (non)");
  expect(flagged("La validazione lato server e' obbligatoria",
                 "La validazione lato server e' opzionale"),
         "Italian opposite terms");
  expect(flagged("Writes to the index are synchronous",
                 "Writes to the index are asynchronous"),
         "synchronous / asynchronous");
}

static void test_quiet(void) {
  expect(!flagged("SQLite WAL mode needs a shared-memory file next to the DB",
                  "With WAL journaling SQLite keeps a -shm file beside the database"),
         "paraphrase: no signal");
  expect(!flagged("The cache is not shared between workers",
                  "Workers do not share the cache"),
         "negation on both sides: no signal");
  expect(!div_of("Use a no-op logger in tests", "Use a silent logger in tests").negation,
         "no-op on one side only: not read as a negation");
  expect(!flagged("Handlers must be non-blocking", "Every handler has to be non-blocking"),
         "non-blocking is one token, not the negation non");
  expect(!flagged("Enable the flag before the migration, disable it after",
                  "The flag is enabled for the migration only"),
         "a side naming both terms takes no side");
  expect(!flagged("Timeout is 30 seconds", "Timeout is 60 seconds"),
         "different numbers alone stay below the threshold");
  expect(div_of("Timeout is 30 seconds", "Timeout is 60 seconds").numbers == 1,
         "different numbers are still reported");
  expect(div_of("Requires Node 18.2.0", "Requires Node 18.2.0").score == 0.0,
         "identical text: no signal");
  expect(div_of(NULL, "anything").score == 0.0, "NULL text counts as empty");
}

int main(void) {
  test_contradictions();
  test_quiet();
  if (g_failures) {
    printf("%d failure(s)\n", g_failures);
    return 1;
  }
  printf("all ok\n");
  return 0;
}
