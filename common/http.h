#pragma once

#include <signal.h>

#include "util.h"

typedef struct http_req {
  char method[8];
  char path[256];
  char query[512];
} http_req_t;

typedef struct http_resp {
  int         status;
  const char *type;
  strbuf_t    body;
  void      (*after)(void);  // runs once the response has been sent
} http_resp_t;

typedef void (*http_handler_t)(const http_req_t *req, http_resp_t *resp);

// Parses the request line of `raw`. Returns 0 on success.
int http_parse_request(const char *raw, http_req_t *req);

// Copies the URL-decoded value of query parameter `key` into `out`.
// Returns 0 when the parameter is present.
int http_query_get(const char *query, const char *key, char *out, size_t outlen);

// Binds and listens on `port`. Returns the socket, or -1 with errno set.
int http_listen(int port);

// Serves requests on `srv` until `*stop` is set, then closes it. Each
// connection runs on its own thread, so handlers must be thread-safe.
void http_serve(int srv, http_handler_t handler, volatile sig_atomic_t *stop);

// Sends `POST path` to this console's own server on `port`. Returns 0 on a
// 200 response.
int http_post_local(int port, const char *path);
