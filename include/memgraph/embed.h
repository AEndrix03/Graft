#ifndef MEMGRAPH_EMBED_H
#define MEMGRAPH_EMBED_H

#include "memgraph/types.h"
#include "memgraph/error.h"

typedef struct mg_embed_ctx mg_embed_ctx_t;

mg_err_t mg_embed_init(const char *model_path, int threads, int ctx_size, mg_embed_ctx_t **out);
void     mg_embed_shutdown(mg_embed_ctx_t *ctx);

/* Calcola embedding di testo. out e' L2-normalized. Thread-safe. */
mg_err_t mg_embed_text(mg_embed_ctx_t *ctx, const char *text, mg_embedding_t out);

/* Cosine similarity tra due vettori L2-normalized = dot product */
float mg_cosine(const mg_embedding_t a, const mg_embedding_t b);

#endif
