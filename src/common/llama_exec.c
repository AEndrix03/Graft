/* Fixed-thread executor for llama work: see graft/llama_exec.h. */

#include "graft/llama_exec.h"

#include <pthread.h>
#include <stdlib.h>

struct mg_llama_exec {
  pthread_t       thread;
  pthread_mutex_t submit;   /* serialises callers: one job in flight */
  pthread_mutex_t mu;
  pthread_cond_t  cv_job;
  pthread_cond_t  cv_done;
  mg_llama_job_fn fn;
  void           *arg;
  int             result;
  int             has_job;
  int             done;
  int             stop;
};

static void *exec_main(void *vp) {
  mg_llama_exec_t *ex = (mg_llama_exec_t *)vp;

  pthread_mutex_lock(&ex->mu);
  for (;;) {
    while (!ex->has_job && !ex->stop) {
      pthread_cond_wait(&ex->cv_job, &ex->mu);
    }
    if (!ex->has_job) {
      break; /* stop requested, nothing pending */
    }
    mg_llama_job_fn fn = ex->fn;
    void *arg = ex->arg;
    ex->has_job = 0;
    pthread_mutex_unlock(&ex->mu);

    int r = fn(arg);

    pthread_mutex_lock(&ex->mu);
    ex->result = r;
    ex->done = 1;
    pthread_cond_broadcast(&ex->cv_done);
  }
  pthread_mutex_unlock(&ex->mu);
  return NULL;
}

int mg_llama_exec_start(mg_llama_exec_t **out) {
  if (!out) return -1;
  *out = NULL;

  mg_llama_exec_t *ex = (mg_llama_exec_t *)calloc(1, sizeof(*ex));
  if (!ex) return -1;

  pthread_mutex_init(&ex->submit, NULL);
  pthread_mutex_init(&ex->mu, NULL);
  pthread_cond_init(&ex->cv_job, NULL);
  pthread_cond_init(&ex->cv_done, NULL);

  if (pthread_create(&ex->thread, NULL, exec_main, ex) != 0) {
    pthread_cond_destroy(&ex->cv_done);
    pthread_cond_destroy(&ex->cv_job);
    pthread_mutex_destroy(&ex->mu);
    pthread_mutex_destroy(&ex->submit);
    free(ex);
    return -1;
  }

  *out = ex;
  return 0;
}

int mg_llama_exec_run(mg_llama_exec_t *ex, mg_llama_job_fn fn, void *arg) {
  int r;

  if (!ex || !fn) return -1;

  pthread_mutex_lock(&ex->submit);
  pthread_mutex_lock(&ex->mu);
  if (ex->stop) {
    pthread_mutex_unlock(&ex->mu);
    pthread_mutex_unlock(&ex->submit);
    return -1;
  }
  ex->fn = fn;
  ex->arg = arg;
  ex->done = 0;
  ex->has_job = 1;
  pthread_cond_signal(&ex->cv_job);
  while (!ex->done) {
    pthread_cond_wait(&ex->cv_done, &ex->mu);
  }
  r = ex->result;
  pthread_mutex_unlock(&ex->mu);
  pthread_mutex_unlock(&ex->submit);
  return r;
}

void mg_llama_exec_stop(mg_llama_exec_t *ex) {
  if (!ex) return;

  pthread_mutex_lock(&ex->mu);
  ex->stop = 1;
  pthread_cond_signal(&ex->cv_job);
  pthread_mutex_unlock(&ex->mu);
  pthread_join(ex->thread, NULL);

  pthread_cond_destroy(&ex->cv_done);
  pthread_cond_destroy(&ex->cv_job);
  pthread_mutex_destroy(&ex->mu);
  pthread_mutex_destroy(&ex->submit);
  free(ex);
}
