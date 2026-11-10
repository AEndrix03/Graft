#ifndef MG_MAINTAIN_DIVERGENCE_H
#define MG_MAINTAIN_DIVERGENCE_H

/* Contradiction heuristic for near-duplicate notes (issue #22).
 *
 * Two notes about the same thing (the caller already knows they are near
 * duplicates) that diverge in polarity are worth an agent's look: a negation
 * on one side only, a pair of opposite terms split across the two, or
 * different numbers. No model is involved and nothing here decides anything:
 * the score only promotes a near_duplicate candidate to
 * possible_contradiction. English and Italian word lists; tuned to prefer
 * false negatives. */

typedef struct {
  double score;          /* 0..1: negation 0.5, each opposite pair 0.5, numbers 0.25 */
  int    negation;       /* a negation word on one side only */
  int    numbers;        /* both sides carry numbers and the sets differ */
  int    n_opposites;
  char   opposites[256]; /* "enable/disable,always/never" (side a first) */
} mg_divergence_t;

/* a and b are the two notes' texts (title + body); NULL counts as empty. */
void mg_divergence(const char *a, const char *b, mg_divergence_t *out);

#endif
