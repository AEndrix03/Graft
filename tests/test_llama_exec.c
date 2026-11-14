/* Every llama encode/decode must run on one fixed thread per context (#28).
 * OpenMP keeps a worker team per calling thread, and graftd serves each
 * connection on a fresh thread, so calling llama from the connection thread
 * leaked n_threads - 1 workers per request. This checks the executor contract
 * without a model: jobs submitted from many short-lived threads all run on the
 * same executor thread, their results come back to the caller, and stop()
 * joins cleanly. */

#include "graft/llama_exec.h"

#include <pthread.h>
#include <stdio.h>

#define N_CALLERS 16
#define N_JOBS    25

static int g_failures = 0;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_llama_exec: %s\n", msg);
    g_failures++;
  }
}

static mg_llama_exec_t *g_ex;
static pthread_t        g_first;
static int              g_have_first = 0;
static int              g_other_thread = 0;
static int              g_runs = 0;  /* only touched on the executor thread */

static int job(void *arg) {
  pthread_t self = pthread_self();
  if (!g_have_first) {
    g_first = self;
    g_have_first = 1;
  } else if (!pthread_equal(g_first, self)) {
    g_other_thread = 1;
  }
  g_runs++;
  return *(int *)arg * 2;
}

static void *caller(void *vp) {
  int base = *(int *)vp;
  for (int i = 0; i < N_JOBS; i++) {
    int in = base * 1000 + i;
    int out = mg_llama_exec_run(g_ex, job, &in);
    if (out != in * 2) {
      expect(0, "job result not returned to the caller");
    }
    if (pthread_equal(pthread_self(), g_first) && g_have_first) {
      expect(0, "job ran on the calling thread");
    }
  }
  return NULL;
}

int main(void) {
  pthread_t th[N_CALLERS];
  int ids[N_CALLERS];

  expect(mg_llama_exec_run(NULL, job, NULL) == -1, "NULL executor is rejected");
  expect(mg_llama_exec_start(&g_ex) == 0 && g_ex, "executor starts");
  if (!g_ex) return 1;

  for (int i = 0; i < N_CALLERS; i++) {
    ids[i] = i + 1;
    pthread_create(&th[i], NULL, caller, &ids[i]);
  }
  for (int i = 0; i < N_CALLERS; i++) {
    pthread_join(th[i], NULL);
  }

  expect(g_runs == N_CALLERS * N_JOBS, "every job ran exactly once");
  expect(!g_other_thread, "all jobs ran on the same executor thread");

  mg_llama_exec_stop(g_ex);
  mg_llama_exec_stop(NULL);

  if (g_failures) {
    fprintf(stderr, "test_llama_exec: %d failure(s)\n", g_failures);
    return 1;
  }
  puts("test_llama_exec: ok");
  return 0;
}
