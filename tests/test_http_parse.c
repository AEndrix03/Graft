/* HTTP/1.1 request parser: request line, headers, Content-Length body and
 * query-string decoding. Requests travel over a real loopback TCP connection
 * (socketpair() does not exist on Windows) and a writer thread feeds them, so
 * bodies larger than the socket buffer are read across several recv() calls. */

#include "../src/http/internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define close_sock closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define close_sock close
#endif

static int g_failures = 0;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "test_http_parse: %s\n", msg);
    g_failures++;
  }
}

static int str_eq(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}

/* --- loopback transport --- */

typedef struct {
  sock_t      fd;
  const char *chunks[4];   /* sent in order, NULL-terminated */
  size_t      lens[4];
} writer_t;

static void *writer_main(void *arg) {
  writer_t *w = (writer_t *)arg;
  for (int i = 0; i < 4 && w->chunks[i]; ++i) {
    size_t off = 0;
    while (off < w->lens[i]) {
      int n = send(w->fd, w->chunks[i] + off, (int)(w->lens[i] - off), 0);
      if (n <= 0) break;
      off += (size_t)n;
    }
  }
  /* Half-close so a parser waiting for more bytes sees EOF instead of hanging. */
#ifdef _WIN32
  shutdown(w->fd, SD_SEND);
#else
  shutdown(w->fd, SHUT_WR);
#endif
  return NULL;
}

static int loopback_pair(sock_t *client, sock_t *server) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  sock_t lst = socket(AF_INET, SOCK_STREAM, 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(lst, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
  if (listen(lst, 1) != 0) return -1;
  if (getsockname(lst, (struct sockaddr *)&addr, &len) != 0) return -1;
  *client = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(*client, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
  *server = accept(lst, NULL, NULL);
  close_sock(lst);
  return 0;
}

/* Send up to three chunks through the loopback and parse what arrives. */
static mg_err_t parse_chunks(mg_http_request_t *req, const char *c0, size_t l0,
                             const char *c1, size_t l1) {
  sock_t client, server;
  writer_t w;
  pthread_t th;
  mg_err_t err;
  if (loopback_pair(&client, &server) != 0) {
    expect(0, "cannot open loopback socket pair");
    return MG_ERR_IO;
  }
  memset(&w, 0, sizeof(w));
  w.fd = client;
  w.chunks[0] = c0; w.lens[0] = l0;
  w.chunks[1] = c1; w.lens[1] = l1;
  pthread_create(&th, NULL, writer_main, &w);
  err = mg_http_parse_request((int)server, req);
  pthread_join(th, NULL);
  close_sock(client);
  close_sock(server);
  return err;
}

static mg_err_t parse_str(mg_http_request_t *req, const char *raw) {
  return parse_chunks(req, raw, strlen(raw), NULL, 0);
}

/* --- request line and headers --- */

static void test_get_with_query_and_headers(void) {
  mg_http_request_t req;
  mg_err_t err = parse_str(&req,
      "GET /v1/match?text=hello+world&top_k=5 HTTP/1.1\r\n"
      "Host: localhost:9977\r\n"
      "X-Padded:   spaced value \t\r\n"
      "\r\n");
  expect(err == MG_OK, "plain GET must parse");
  expect(str_eq(req.method, "GET"), "method");
  expect(str_eq(req.path, "/v1/match"), "path is split from the query");
  expect(str_eq(req.query, "text=hello+world&top_k=5"), "raw query is kept");
  expect(req.n_headers == 2, "two headers");
  expect(str_eq(mg_http_header_get(&req, "host"), "localhost:9977"),
         "header lookup is case-insensitive");
  expect(str_eq(mg_http_header_get(&req, "X-Padded"), "spaced value"),
         "header value is trimmed on both sides");
  expect(mg_http_header_get(&req, "Missing") == NULL, "absent header is NULL");
  expect(req.body == NULL && req.body_len == 0, "GET without Content-Length has no body");
  mg_http_request_free(&req);
  printf("ok get with query and headers\n");
}

static void test_path_without_query(void) {
  mg_http_request_t req;
  expect(parse_str(&req, "DELETE /v1/nodes/abc HTTP/1.1\r\n\r\n") == MG_OK,
         "DELETE must parse");
  expect(str_eq(req.method, "DELETE"), "DELETE method");
  expect(str_eq(req.path, "/v1/nodes/abc"), "DELETE path");
  expect(req.query == NULL, "no '?' means NULL query");
  mg_http_request_free(&req);
  printf("ok path without query\n");
}

static void test_header_without_colon_is_ignored(void) {
  mg_http_request_t req;
  expect(parse_str(&req, "GET / HTTP/1.1\r\nnot a header\r\nA: 1\r\n\r\n") == MG_OK,
         "request with a junk header line must still parse");
  expect(req.n_headers == 1, "the junk line is skipped");
  expect(str_eq(mg_http_header_get(&req, "A"), "1"), "the valid header survives");
  mg_http_request_free(&req);
  printf("ok header without colon\n");
}

static void test_header_cap(void) {
  char raw[4096];
  size_t off = (size_t)snprintf(raw, sizeof(raw), "GET / HTTP/1.1\r\n");
  mg_http_request_t req;
  for (int i = 0; i < MG_HTTP_MAX_HEADERS + 5; ++i)
    off += (size_t)snprintf(raw + off, sizeof(raw) - off, "H%d: v\r\n", i);
  snprintf(raw + off, sizeof(raw) - off, "\r\n");
  expect(parse_str(&req, raw) == MG_OK, "too many headers still parses");
  expect(req.n_headers == MG_HTTP_MAX_HEADERS, "headers beyond the cap are dropped");
  mg_http_request_free(&req);
  printf("ok header cap\n");
}

/* --- body --- */

static void test_body_in_same_packet(void) {
  mg_http_request_t req;
  expect(parse_str(&req,
      "POST /v1/insert HTTP/1.1\r\n"
      "Content-Length: 13\r\n"
      "\r\n"
      "{\"title\":\"x\"}") == MG_OK, "POST must parse");
  expect(req.body_len == 13, "body length follows Content-Length");
  expect(req.body && memcmp(req.body, "{\"title\":\"x\"}", 13) == 0, "body bytes");
  expect(req.body && ((char *)req.body)[13] == '\0', "body is NUL-terminated");
  mg_http_request_free(&req);
  printf("ok body in same packet\n");
}

static void test_large_body_across_reads(void) {
  /* Bigger than any socket buffer, so the parser must loop on recv(). */
  const size_t n = 300 * 1024;
  char head[128];
  char *body = (char *)malloc(n);
  mg_http_request_t req;
  for (size_t i = 0; i < n; ++i) body[i] = (char)('a' + (i % 26));
  snprintf(head, sizeof(head), "POST /v1/insert HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", n);
  expect(parse_chunks(&req, head, strlen(head), body, n) == MG_OK,
         "large body must parse");
  expect(req.body_len == n, "large body length");
  expect(req.body && memcmp(req.body, body, n) == 0, "large body bytes intact");
  mg_http_request_free(&req);
  free(body);
  printf("ok large body across reads\n");
}

static void test_body_beyond_content_length_is_ignored(void) {
  mg_http_request_t req;
  expect(parse_str(&req,
      "POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcEXTRA") == MG_OK,
      "POST with trailing bytes must parse");
  expect(req.body_len == 3 && memcmp(req.body, "abc", 3) == 0,
         "only Content-Length bytes become the body");
  mg_http_request_free(&req);
  printf("ok body beyond content-length\n");
}

static void test_oversized_body_is_rejected(void) {
  char raw[128];
  mg_http_request_t req;
  snprintf(raw, sizeof(raw), "POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
           MG_HTTP_MAX_REQUEST_BYTES + 1);
  expect(parse_str(&req, raw) == MG_ERR_INVALID_ARG, "body over the cap is rejected");
  expect(req.method == NULL && req.n_headers == 0, "rejected request is released");
  expect(parse_str(&req, "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n")
             == MG_ERR_INVALID_ARG, "negative Content-Length is rejected");
  printf("ok oversized body rejected\n");
}

static void test_truncated_body_is_io_error(void) {
  mg_http_request_t req;
  expect(parse_str(&req, "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc")
             == MG_ERR_IO, "peer closing before the full body is an IO error");
  printf("ok truncated body\n");
}

/* --- malformed input --- */

static void test_malformed_requests(void) {
  mg_http_request_t req;
  expect(parse_str(&req, "GET / HTTP/1.1\r\nHost: x\r\n") == MG_ERR_IO,
         "EOF before the blank line is an IO error");
  expect(parse_str(&req, "GARBAGE\r\n\r\n") == MG_ERR_IO,
         "request line without spaces is rejected");
  expect(parse_str(&req, "GET /only-path\r\n\r\n") == MG_ERR_IO,
         "request line without HTTP version is rejected");
  expect(mg_http_parse_request(0, NULL) == MG_ERR_INVALID_ARG, "NULL request");
  printf("ok malformed requests\n");
}

/* --- query string --- */

static void expect_query(const char *query, const char *name, const char *want,
                         const char *msg) {
  mg_http_request_t req;
  char *got;
  memset(&req, 0, sizeof(req));
  req.query = (char *)query;
  got = mg_http_query_get(&req, name);
  if (want == NULL) expect(got == NULL, msg);
  else              expect(str_eq(got, want), msg);
  free(got);
}

static void test_query_get(void) {
  expect_query("text=hello+world", "text", "hello world", "'+' decodes to space");
  expect_query("text=caf%C3%A9%20bar", "text", "caf\xC3\xA9 bar", "percent-encoded UTF-8");
  expect_query("text=50%zz", "text", "50%zz", "invalid escape is kept verbatim");
  expect_query("text=100%", "text", "100%", "trailing '%' is kept verbatim");
  expect_query("top_k=5&top=1", "top", "1", "a prefix of another key does not match");
  expect_query("a=1&b=2&c=3", "c", "3", "last parameter");
  expect_query("a=&b=2", "a", "", "empty value is an empty string");
  expect_query("flag&x=1", "flag", NULL, "key without '=' has no value");
  expect_query("a=1", "b", NULL, "missing key is NULL");
  expect_query("a=1", NULL, NULL, "NULL name is NULL");
  expect(mg_http_query_get(NULL, "a") == NULL, "NULL request is NULL");
  printf("ok query get\n");
}

int main(void) {
#ifdef _WIN32
  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
#endif
  test_get_with_query_and_headers();
  test_path_without_query();
  test_header_without_colon_is_ignored();
  test_header_cap();
  test_body_in_same_packet();
  test_large_body_across_reads();
  test_body_beyond_content_length_is_ignored();
  test_oversized_body_is_rejected();
  test_truncated_body_is_io_error();
  test_malformed_requests();
  test_query_get();
#ifdef _WIN32
  WSACleanup();
#endif
  if (g_failures) {
    fprintf(stderr, "test_http_parse: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
