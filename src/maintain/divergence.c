#include "divergence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIV_W_NEGATION 0.5
#define DIV_W_OPPOSITE 0.5
#define DIV_W_NUMBERS  0.25
#define DIV_MAX_TOKEN  48

/* Whole-word negations. Tokens ending in "n't" count too. */
static const char *const negations[] = {
  /* en */ "not", "no", "never", "none", "nothing", "nobody", "neither", "nor", "cannot", "without",
  /* it */ "non", "mai", "nessun", "nessuno", "nessuna", "niente", "nulla", "senza", "n\xc3\xa9",
  NULL
};

/* Opposite terms, each inflection listed: a stemmer would buy little here
 * and cost predictability. */
static const char *const opposites[][2] = {
  /* en */
  { "enable", "disable" }, { "enabled", "disabled" }, { "enables", "disables" },
  { "enabling", "disabling" }, { "allow", "deny" }, { "allowed", "denied" },
  { "allowed", "forbidden" }, { "always", "never" }, { "true", "false" },
  { "sync", "async" }, { "synchronous", "asynchronous" }, { "synchronously", "asynchronously" },
  { "required", "optional" }, { "mandatory", "optional" }, { "deprecated", "recommended" },
  { "supported", "unsupported" }, { "safe", "unsafe" }, { "before", "after" },
  { "increase", "decrease" }, { "increases", "decreases" }, { "faster", "slower" },
  { "include", "exclude" }, { "included", "excluded" }, { "public", "private" },
  { "blocking", "non-blocking" }, { "mutable", "immutable" }, { "lazy", "eager" },
  { "inclusive", "exclusive" }, { "valid", "invalid" }, { "works", "broken" },
  /* it */
  { "abilitato", "disabilitato" }, { "abilitata", "disabilitata" }, { "abilitare", "disabilitare" },
  { "abilita", "disabilita" }, { "sempre", "mai" }, { "vero", "falso" },
  { "obbligatorio", "opzionale" }, { "obbligatoria", "opzionale" }, { "sincrono", "asincrono" },
  { "sincrona", "asincrona" }, { "consentito", "vietato" }, { "consentita", "vietata" },
  { "prima", "dopo" }, { "attivo", "disattivo" }, { "attiva", "disattiva" },
  { "aumenta", "diminuisce" }, { "supportato", "deprecato" }, { "valido", "invalido" },
  { NULL, NULL }
};

typedef struct {
  char  **v;
  size_t  n, cap;
} tokset_t;

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static int has(const tokset_t *t, const char *w) {
  return bsearch(&w, t->v, t->n, sizeof(char *), cmp_str) != NULL;
}

static void push(tokset_t *t, const char *w, size_t len) {
  char *copy;
  if (t->n == t->cap) {
    size_t nc = t->cap ? t->cap * 2 : 64;
    char **nv = (char **)realloc(t->v, nc * sizeof(*nv));
    if (!nv) return;
    t->v = nv;
    t->cap = nc;
  }
  copy = (char *)malloc(len + 1);
  if (!copy) return;
  memcpy(copy, w, len);
  copy[len] = '\0';
  t->v[t->n++] = copy;
}

static void finish(tokset_t *t) {
  size_t i, o = 0;
  if (t->n) qsort(t->v, t->n, sizeof(char *), cmp_str);
  for (i = 0; i < t->n; ++i) {
    if (o && !strcmp(t->v[o - 1], t->v[i])) {
      free(t->v[i]);
      continue;
    }
    t->v[o++] = t->v[i];
  }
  t->n = o;
}

static void tokset_free(tokset_t *t) {
  size_t i;
  for (i = 0; i < t->n; ++i) free(t->v[i]);
  free(t->v);
}

static int is_alnum(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}

/* Lower-cased words. Letters, digits and UTF-8 bytes form a word; an
 * apostrophe (also U+2019), '-' or '.' joins two word characters, so
 * "don't", "no-op", "non-blocking" and "1.2.3" stay one token while
 * "no" in "no-op" is not read as a negation. */
static void tokenize(const char *text, tokset_t *words, tokset_t *numbers) {
  const unsigned char *p = (const unsigned char *)(text ? text : "");
  char buf[DIV_MAX_TOKEN];
  size_t len = 0;
  for (;; ++p) {
    unsigned char c = *p;
    int word = c && is_alnum(c);
    if (c == 0xE2 && p[1] == 0x80 && p[2] == 0x99) {   /* U+2019, read as ' */
      p += 2;
      c = '\'';
      word = 0;
    }
    if (!word && len > 0 && (c == '\'' || c == '-' || c == '.') && p[1] && p[1] < 0x80 &&
        is_alnum(p[1])) {
      word = 1;
    }
    if (word) {
      if (len + 1 < sizeof(buf)) buf[len++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
      continue;
    }
    if (len > 0) {
      push(buf[0] >= '0' && buf[0] <= '9' ? numbers : words, buf, len);
      len = 0;
    }
    if (!c) break;
  }
  finish(words);
  finish(numbers);
}

static int has_negation(const tokset_t *t) {
  size_t i;
  for (i = 0; negations[i]; ++i) {
    if (has(t, negations[i])) return 1;
  }
  for (i = 0; i < t->n; ++i) {
    size_t n = strlen(t->v[i]);
    if (n > 3 && !strcmp(t->v[i] + n - 3, "n't")) return 1;
  }
  return 0;
}

static int same_set(const tokset_t *a, const tokset_t *b) {
  size_t i;
  if (a->n != b->n) return 0;
  for (i = 0; i < a->n; ++i) {
    if (strcmp(a->v[i], b->v[i]) != 0) return 0;
  }
  return 1;
}

static void add_opposite(mg_divergence_t *out, const char *x, const char *y) {
  size_t off = strlen(out->opposites);
  int n = snprintf(out->opposites + off, sizeof(out->opposites) - off, "%s%s/%s",
                   off ? "," : "", x, y);
  if (n < 0 || (size_t)n >= sizeof(out->opposites) - off) out->opposites[off] = '\0';
  out->n_opposites++;
}

void mg_divergence(const char *a, const char *b, mg_divergence_t *out) {
  tokset_t wa = {0}, wb = {0}, na = {0}, nb = {0};
  size_t i;
  memset(out, 0, sizeof(*out));
  tokenize(a, &wa, &na);
  tokenize(b, &wb, &nb);

  out->negation = has_negation(&wa) != has_negation(&wb);
  out->numbers = na.n > 0 && nb.n > 0 && !same_set(&na, &nb);
  /* a term on one side, its opposite on the other, and neither side
   * carrying both (a note that names both terms is not taking a side) */
  for (i = 0; opposites[i][0]; ++i) {
    const char *x = opposites[i][0], *y = opposites[i][1];
    int ax = has(&wa, x), ay = has(&wa, y), bx = has(&wb, x), by = has(&wb, y);
    if (ax && ay) continue;
    if (bx && by) continue;
    if (ax && by) add_opposite(out, x, y);
    else if (ay && bx) add_opposite(out, y, x);
  }

  out->score = (out->negation ? DIV_W_NEGATION : 0.0) + DIV_W_OPPOSITE * out->n_opposites +
               (out->numbers ? DIV_W_NUMBERS : 0.0);
  if (out->score > 1.0) out->score = 1.0;
  tokset_free(&wa);
  tokset_free(&wb);
  tokset_free(&na);
  tokset_free(&nb);
}
