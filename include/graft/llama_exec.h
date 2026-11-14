#ifndef GRAFT_LLAMA_EXEC_H
#define GRAFT_LLAMA_EXEC_H

/* A single long-lived thread that runs llama work on behalf of its callers.
 *
 * Why: ggml's CPU backend parallelises each graph with OpenMP when it is built
 * with GGML_OPENMP (the default, and what the release builds ship). OpenMP
 * keeps one worker team per *calling* thread. graftd serves every connection
 * on a fresh detached thread, so every request created a new team of
 * n_threads - 1 workers. With libgomp on MinGW those workers outlive the
 * calling thread and were never reclaimed: +n_threads-1 threads per request,
 * forever. Running every encode/decode on one fixed thread per llama context
 * keeps a single team that is reused.
 *
 * mg_llama_exec_run() blocks until the job has run and returns its result.
 * Callers are serialised; the job runs on the executor's thread. */

typedef struct mg_llama_exec mg_llama_exec_t;

typedef int (*mg_llama_job_fn)(void *arg);

/* Returns 0 on success, -1 on failure (*out left NULL). */
int  mg_llama_exec_start(mg_llama_exec_t **out);

/* Runs fn(arg) on the executor thread and returns its result, or -1 when the
 * executor is NULL or stopping. */
int  mg_llama_exec_run(mg_llama_exec_t *ex, mg_llama_job_fn fn, void *arg);

/* Stops and joins the thread, then frees the executor. NULL is a no-op. */
void mg_llama_exec_stop(mg_llama_exec_t *ex);

#endif
