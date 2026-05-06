/* requires llama built */
#include "memgraph/embed.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <llama.h>

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
typedef SRWLOCK mg_mutex_t;
#define MG_MUTEX_INITIALIZER SRWLOCK_INIT
static int mg_mutex_init(mg_mutex_t *lock) {
  InitializeSRWLock(lock);
  return 0;
}
static int mg_mutex_destroy(mg_mutex_t *lock) {
  (void)lock;
  return 0;
}
static int mg_mutex_lock(mg_mutex_t *lock) {
  AcquireSRWLockExclusive(lock);
  return 0;
}
static int mg_mutex_unlock(mg_mutex_t *lock) {
  ReleaseSRWLockExclusive(lock);
  return 0;
}
#else
typedef pthread_mutex_t mg_mutex_t;
#define MG_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static int mg_mutex_init(mg_mutex_t *lock) {
  return pthread_mutex_init(lock, NULL);
}
static int mg_mutex_destroy(mg_mutex_t *lock) {
  return pthread_mutex_destroy(lock);
}
static int mg_mutex_lock(mg_mutex_t *lock) {
  return pthread_mutex_lock(lock);
}
static int mg_mutex_unlock(mg_mutex_t *lock) {
  return pthread_mutex_unlock(lock);
}
#endif

struct mg_embed_ctx {
  struct llama_model   *model;
  struct llama_context *ctx;
  mg_mutex_t            lock;
  int                   n_ctx;
};

static mg_mutex_t g_backend_lock = MG_MUTEX_INITIALIZER;
static int g_backend_refs = 0;

static mg_err_t mg_backend_acquire(void) {
  if (mg_mutex_lock(&g_backend_lock) != 0) {
    return MG_ERR_INTERNAL;
  }
  if (g_backend_refs == 0) {
    llama_backend_init();
  }
  g_backend_refs++;
  if (mg_mutex_unlock(&g_backend_lock) != 0) {
    return MG_ERR_INTERNAL;
  }
  return MG_OK;
}

static void mg_backend_release(void) {
  if (mg_mutex_lock(&g_backend_lock) != 0) {
    return;
  }
  if (g_backend_refs > 0) {
    g_backend_refs--;
    if (g_backend_refs == 0) {
      llama_backend_free();
    }
  }
  (void)mg_mutex_unlock(&g_backend_lock);
}

static void mg_embed_ctx_free_partial(mg_embed_ctx_t *ctx, int lock_initialized) {
  if (!ctx) {
    return;
  }
  if (ctx->ctx) {
    llama_free(ctx->ctx);
  }
  if (ctx->model) {
    llama_model_free(ctx->model);
  }
  if (lock_initialized) {
    (void)mg_mutex_destroy(&ctx->lock);
  }
  free(ctx);
}

static mg_err_t mg_normalize(const float *src, int dim, mg_embedding_t out) {
  double sum = 0.0;

  if (!src || !out || dim != MG_EMBEDDING_DIM) {
    return MG_ERR_INVALID_ARG;
  }

  for (int i = 0; i < dim; i++) {
    sum += (double)src[i] * (double)src[i];
  }

  if (sum <= 0.0) {
    return MG_ERR_EMBED;
  }

  const float inv_norm = (float)(1.0 / sqrt(sum));
  for (int i = 0; i < dim; i++) {
    out[i] = src[i] * inv_norm;
  }

  return MG_OK;
}

mg_err_t mg_embed_init(const char *model_path, int threads, int ctx_size, mg_embed_ctx_t **out) {
  mg_err_t err;
  mg_embed_ctx_t *ctx;
  struct llama_model_params model_params;
  struct llama_context_params ctx_params;
  int n_embd;

  if (!model_path || !out || threads <= 0 || ctx_size <= 0) {
    return MG_ERR_INVALID_ARG;
  }

  *out = NULL;

  err = mg_backend_acquire();
  if (err != MG_OK) {
    return err;
  }

  ctx = (mg_embed_ctx_t *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    mg_backend_release();
    return MG_ERR_OOM;
  }

  if (mg_mutex_init(&ctx->lock) != 0) {
    free(ctx);
    mg_backend_release();
    return MG_ERR_INTERNAL;
  }

  model_params = llama_model_default_params();
  ctx->model = llama_model_load_from_file(model_path, model_params);
  if (!ctx->model) {
    mg_embed_ctx_free_partial(ctx, 1);
    mg_backend_release();
    return MG_ERR_EMBED;
  }

  n_embd = llama_model_n_embd_out(ctx->model);
  if (n_embd <= 0) {
    n_embd = llama_model_n_embd(ctx->model);
  }
  if (n_embd != MG_EMBEDDING_DIM) {
    mg_embed_ctx_free_partial(ctx, 1);
    mg_backend_release();
    return MG_ERR_EMBED;
  }

  ctx_params = llama_context_default_params();
  ctx_params.embeddings = true;
  ctx_params.pooling_type = LLAMA_POOLING_TYPE_MEAN;
  ctx_params.n_ctx = (uint32_t)ctx_size;
  ctx_params.n_batch = (uint32_t)ctx_size;
  ctx_params.n_ubatch = (uint32_t)ctx_size;
  ctx_params.n_seq_max = 1;
  ctx_params.n_threads = threads;
  ctx_params.n_threads_batch = threads;

  ctx->ctx = llama_init_from_model(ctx->model, ctx_params);
  if (!ctx->ctx) {
    mg_embed_ctx_free_partial(ctx, 1);
    mg_backend_release();
    return MG_ERR_EMBED;
  }

  ctx->n_ctx = ctx_size;
  *out = ctx;
  return MG_OK;
}

void mg_embed_shutdown(mg_embed_ctx_t *ctx) {
  if (!ctx) {
    return;
  }

  mg_embed_ctx_free_partial(ctx, 1);
  mg_backend_release();
}

mg_err_t mg_embed_text(mg_embed_ctx_t *ctx, const char *text, mg_embedding_t out) {
  const struct llama_vocab *vocab;
  llama_token *tokens;
  int32_t text_len;
  int32_t n_tokens;
  struct llama_batch batch;
  float *embedding;
  mg_err_t err = MG_OK;

  if (!ctx || !text || !out) {
    return MG_ERR_INVALID_ARG;
  }

  if (strlen(text) > (size_t)INT32_MAX) {
    return MG_ERR_INVALID_ARG;
  }
  text_len = (int32_t)strlen(text);

  tokens = (llama_token *)calloc((size_t)ctx->n_ctx, sizeof(*tokens));
  if (!tokens) {
    return MG_ERR_OOM;
  }

  if (mg_mutex_lock(&ctx->lock) != 0) {
    free(tokens);
    return MG_ERR_INTERNAL;
  }

  llama_memory_clear(llama_get_memory(ctx->ctx), true);

  vocab = llama_model_get_vocab(ctx->model);
  n_tokens = llama_tokenize(vocab, text, text_len, tokens, ctx->n_ctx, true, false);
  if (n_tokens < 0 || n_tokens > ctx->n_ctx) {
    err = MG_ERR_EMBED;
    goto done;
  }
  if (n_tokens == 0) {
    err = MG_ERR_INVALID_ARG;
    goto done;
  }

  batch = llama_batch_get_one(tokens, n_tokens);
  if (llama_model_has_encoder(ctx->model)) {
    if (llama_encode(ctx->ctx, batch) != 0) {
      err = MG_ERR_EMBED;
      goto done;
    }
  } else if (llama_decode(ctx->ctx, batch) != 0) {
    err = MG_ERR_EMBED;
    goto done;
  }

  embedding = llama_get_embeddings_seq(ctx->ctx, 0);
  if (!embedding) {
    err = MG_ERR_EMBED;
    goto done;
  }

  err = mg_normalize(embedding, MG_EMBEDDING_DIM, out);

done:
  if (mg_mutex_unlock(&ctx->lock) != 0 && err == MG_OK) {
    err = MG_ERR_INTERNAL;
  }
  free(tokens);
  return err;
}

float mg_cosine(const mg_embedding_t a, const mg_embedding_t b) {
  float sum = 0.0f;

  if (!a || !b) {
    return 0.0f;
  }

  for (int i = 0; i < MG_EMBEDDING_DIM; i++) {
    sum += a[i] * b[i];
  }

  return sum;
}
