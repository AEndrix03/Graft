/* memgraphd — long-running daemon listening on an AF_UNIX socket.
 *
 * Wiring:
 *   parse argv -> config_load -> storage_open + apply_schema
 *                -> embed_init -> verify_init
 *                -> socket_listen
 *                -> accept-loop, thread-per-connection
 *
 * Each accepted client runs handle_client() which loops on
 *   read_frame -> mg_dispatch -> write_frame
 * until the peer closes or an I/O error occurs.
 *
 * Shutdown: SIGINT/SIGTERM set g_shutdown and close the listening fd,
 * which makes the next accept() return -1 and the main loop exits.
 * Per-client threads are detached; outstanding clients will see a
 * read EOF when their fd is closed at shutdown.
 */

#include "memgraph/ops.h"
#include "memgraph/wire.h"
#include "memgraph/storage.h"
#include "memgraph/embed.h"
#include "memgraph/verify.h"
#include "memgraph/config.h"
#include "memgraph/error.h"
#include "internal.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

static volatile sig_atomic_t g_shutdown  = 0;
static int                   g_listen_fd = -1;

static void on_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
    int fd = g_listen_fd;
    if (fd >= 0) {
        g_listen_fd = -1;
        mg_daemon_socket_close(fd);
    }
}

typedef struct {
    int       fd;
    mg_ctx_t *ctx;
} client_arg_t;

static void *handle_client(void *vp) {
    client_arg_t *ca = (client_arg_t *)vp;
    int       fd  = ca->fd;
    mg_ctx_t *ctx = ca->ctx;
    free(ca);

    for (;;) {
        if (g_shutdown) break;

        void  *req     = NULL;
        size_t req_len = 0;
        if (mg_wire_read_frame(fd, &req, &req_len) != MG_OK) break;

        void  *resp     = NULL;
        size_t resp_len = 0;
        mg_err_t e = mg_dispatch(ctx, req ? req : "", req_len,
                                  &resp, &resp_len);
        free(req);

        if (e == MG_OK && resp && resp_len > 0) {
            (void)mg_wire_write_frame(fd, resp, resp_len);
        }
        free(resp);
        if (e != MG_OK) break;
    }
    mg_daemon_socket_close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    const char *config_path = "./config.example.yaml";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (!strcmp(argv[i], "--foreground")) {
            /* default; flag accepted for forward-compat */
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "usage: memgraphd [--config PATH] [--foreground]\n");
            return 0;
        } else {
            fprintf(stderr, "unknown flag: %s\n", argv[i]);
            return 2;
        }
    }

    mg_config_t cfg;
    mg_err_t err = mg_config_load(config_path, &cfg);
    if (err != MG_OK) {
        fprintf(stderr, "config load failed: %s (path=%s)\n",
                mg_strerror(err), config_path);
        return 1;
    }

    mg_storage_t    *storage = NULL;
    mg_embed_ctx_t  *embed   = NULL;
    mg_verify_ctx_t *verify  = NULL;
    int rc = 1;

    err = mg_storage_open(cfg.db_path, &storage);
    if (err != MG_OK) {
        fprintf(stderr, "storage open failed: %s\n", mg_strerror(err));
        goto cleanup;
    }
    err = mg_storage_apply_schema(storage);
    if (err != MG_OK) {
        fprintf(stderr, "schema apply failed: %s\n", mg_strerror(err));
        goto cleanup;
    }
    err = mg_embed_init(cfg.embed_model_path, cfg.embed_threads,
                         cfg.embed_ctx_size, &embed);
    if (err != MG_OK) {
        fprintf(stderr, "embed init failed: %s\n", mg_strerror(err));
        goto cleanup;
    }
    err = mg_verify_init(&cfg, &verify);
    if (err != MG_OK) {
        fprintf(stderr, "verify init failed: %s\n", mg_strerror(err));
        goto cleanup;
    }

    mg_ctx_t ctx;
    ctx.storage = storage;
    ctx.embed   = embed;
    ctx.verify  = verify;
    ctx.config  = &cfg;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    g_listen_fd = mg_daemon_socket_listen(cfg.socket_path);
    if (g_listen_fd < 0) {
        fprintf(stderr, "socket listen failed on %s\n", cfg.socket_path);
        goto cleanup;
    }
    fprintf(stderr, "memgraphd: listening on %s\n", cfg.socket_path);

    while (!g_shutdown) {
        int cfd = mg_daemon_socket_accept(g_listen_fd);
        if (cfd < 0) break;

        client_arg_t *ca = (client_arg_t *)malloc(sizeof(*ca));
        if (!ca) { mg_daemon_socket_close(cfd); continue; }
        ca->fd  = cfd;
        ca->ctx = &ctx;

        pthread_t th;
        if (pthread_create(&th, NULL, handle_client, ca) != 0) {
            free(ca);
            mg_daemon_socket_close(cfd);
            continue;
        }
        pthread_detach(th);
    }

    fprintf(stderr, "memgraphd: shutting down\n");
    if (g_listen_fd >= 0) {
        mg_daemon_socket_close(g_listen_fd);
        g_listen_fd = -1;
    }
#ifdef _WIN32
    DeleteFileA(cfg.socket_path);
#else
    unlink(cfg.socket_path);
#endif
    rc = 0;

cleanup:
    if (verify)  mg_verify_shutdown(verify);
    if (embed)   mg_embed_shutdown(embed);
    if (storage) mg_storage_close(storage);
    mg_config_free(&cfg);
    mg_daemon_socket_shutdown();
    return rc;
}
