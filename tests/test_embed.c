#include "graft/embed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float embedding_norm(const mg_embedding_t v) {
  double sum = 0.0;

  for (int i = 0; i < MG_EMBEDDING_DIM; i++) {
    sum += (double)v[i] * (double)v[i];
  }

  return (float)sqrt(sum);
}

static int near_one(float value) {
  return fabsf(value - 1.0f) <= 1.0e-3f;
}

int main(void) {
  const char *model_path = getenv("GRAFT_TEST_MODEL");
  mg_embed_ctx_t *ctx = NULL;
  mg_embedding_t hello;
  mg_embedding_t morning;
  mg_err_t err;

  if (!model_path) {
    printf("skip: no model\n");
    return 0;
  }

  err = mg_embed_init(model_path, 2, 512, false, &ctx);
  if (err != MG_OK) {
    fprintf(stderr, "mg_embed_init failed: %d\n", (int)err);
    return 1;
  }

  err = mg_embed_text(ctx, "ciao mondo", hello);
  if (err != MG_OK) {
    fprintf(stderr, "mg_embed_text(ciao mondo) failed: %d\n", (int)err);
    mg_embed_shutdown(ctx);
    return 1;
  }

  err = mg_embed_text(ctx, "buongiorno", morning);
  if (err != MG_OK) {
    fprintf(stderr, "mg_embed_text(buongiorno) failed: %d\n", (int)err);
    mg_embed_shutdown(ctx);
    return 1;
  }

  if (!near_one(embedding_norm(hello))) {
    fprintf(stderr, "embedding norm for ciao mondo is not 1\n");
    mg_embed_shutdown(ctx);
    return 1;
  }

  if (!near_one(embedding_norm(morning))) {
    fprintf(stderr, "embedding norm for buongiorno is not 1\n");
    mg_embed_shutdown(ctx);
    return 1;
  }

  if (!near_one(mg_cosine(hello, hello))) {
    fprintf(stderr, "self cosine is not 1\n");
    mg_embed_shutdown(ctx);
    return 1;
  }

  mg_embed_shutdown(ctx);
  return 0;
}
