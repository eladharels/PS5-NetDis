#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "log.h"


#define MAX_REQUEST 8192
// A client gets this long to send its request line and headers. Browsers
// open idle spare connections, so this must stay short.
#define REQUEST_DEADLINE_MS 3000
// Connections served at once; more are answered 503 straight away.
#define MAX_ACTIVE 16


int
http_parse_request(const char *raw, http_req_t *req) {
  const char *sp1 = strchr(raw, ' ');
  const char *sp2;
  const char *q;
  size_t n;

  memset(req, 0, sizeof(*req));
  if(!sp1 || (size_t)(sp1 - raw) >= sizeof(req->method)) {
    return -1;
  }
  memcpy(req->method, raw, sp1 - raw);
  if(!(sp2=strchr(sp1 + 1, ' '))) {
    return -1;
  }
  if((q=memchr(sp1 + 1, '?', sp2 - sp1 - 1))) {
    n = sp2 - q - 1;
    if(n >= sizeof(req->query)) {
      return -1;
    }
    memcpy(req->query, q + 1, n);
  } else {
    q = sp2;
  }
  n = q - sp1 - 1;
  if(!n || n >= sizeof(req->path)) {
    return -1;
  }
  memcpy(req->path, sp1 + 1, n);
  return 0;
}


static int
hexdigit(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}


int
http_query_get(const char *query, const char *key, char *out, size_t outlen) {
  size_t klen = strlen(key);
  const char *p = query;

  while(p && *p) {
    if(!strncmp(p, key, klen) && p[klen] == '=') {
      size_t o = 0;
      p += klen + 1;
      while(*p && *p != '&' && o + 1 < outlen) {
        if(*p == '%' && hexdigit(p[1]) >= 0 && hexdigit(p[2]) >= 0) {
          out[o++] = (char)(hexdigit(p[1]) << 4 | hexdigit(p[2]));
          p += 3;
        } else {
          out[o++] = *p == '+' ? ' ' : *p;
          p++;
        }
      }
      out[o] = 0;
      return 0;
    }
    if((p=strchr(p, '&'))) {
      p++;
    }
  }
  return -1;
}


static const char *
status_text(int status) {
  switch(status) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  default:  return "Internal Server Error";
  }
}


static void
send_all(int fd, const char *buf, size_t len) {
  while(len) {
    ssize_t n = write(fd, buf, len);
    if(n <= 0) {
      return;
    }
    buf += n;
    len -= n;
  }
}


static void
handle_conn(int fd, http_handler_t handler) {
  char buf[MAX_REQUEST + 1];
  size_t len = 0;
  http_req_t req;
  http_resp_t resp;
  char head[256];
  int n;

  struct timespec start, now;

  // Only the request line and headers matter; the API takes no bodies.
  clock_gettime(CLOCK_MONOTONIC, &start);
  while(len < MAX_REQUEST) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    long waited;
    ssize_t r;

    clock_gettime(CLOCK_MONOTONIC, &now);
    waited = (now.tv_sec - start.tv_sec) * 1000 +
             (now.tv_nsec - start.tv_nsec) / 1000000;
    if(waited >= REQUEST_DEADLINE_MS ||
       poll(&pfd, 1, REQUEST_DEADLINE_MS - waited) <= 0) {
      return;
    }
    if((r=read(fd, buf + len, MAX_REQUEST - len)) <= 0) {
      return;
    }
    len += r;
    buf[len] = 0;
    if(strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) {
      break;
    }
  }
  buf[len] = 0;

  memset(&resp, 0, sizeof(resp));
  sb_init(&resp.body);
  resp.status = 200;
  resp.type = "application/json";
  if(http_parse_request(buf, &req)) {
    resp.status = 400;
    sb_puts(&resp.body, "{\"error\":\"bad request\"}");
  } else {
    handler(&req, &resp);
  }
  if(resp.body.oom) {
    sb_free(&resp.body);
    resp.status = 500;
  }

  n = snprintf(head, sizeof(head),
               "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n"
               "Content-Length: %zu\r\nCache-Control: no-store\r\n"
               "Connection: close\r\n\r\n",
               resp.status, status_text(resp.status), resp.type,
               resp.body.len);
  send_all(fd, head, n);
  if(resp.body.len) {
    send_all(fd, resp.body.data, resp.body.len);
  }
  sb_free(&resp.body);
  if(resp.after) {
    resp.after();
  }
}


typedef struct conn {
  int            fd;
  http_handler_t handler;
} conn_t;

static pthread_mutex_t g_active_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_active;


static void *
conn_thread(void *arg) {
  conn_t *c = arg;

  handle_conn(c->fd, c->handler);
  close(c->fd);
  free(c);
  pthread_mutex_lock(&g_active_lock);
  g_active--;
  pthread_mutex_unlock(&g_active_lock);
  return NULL;
}


static void
reject_busy(int fd) {
  static const char busy[] =
    "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n"
    "Connection: close\r\n\r\n";
  send_all(fd, busy, sizeof(busy) - 1);
  close(fd);
}


int
http_listen(int port) {
  struct sockaddr_in addr;
  int one = 1;
  int srv;
  int err;

  if((srv=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(srv, (struct sockaddr*)&addr, sizeof(addr)) || listen(srv, 8)) {
    err = errno;
    close(srv);
    errno = err;
    return -1;
  }
  return srv;
}


void
http_serve(int srv, http_handler_t handler, volatile sig_atomic_t *stop) {
  struct timeval tv = {5, 0};
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  while(!*stop) {
    struct pollfd pfd = {.fd = srv, .events = POLLIN};
    pthread_t thread;
    conn_t *c;
    int fd;

    // Wake up regularly so a stop request is noticed without a new client.
    if(poll(&pfd, 1, 250) <= 0) {
      continue;
    }
    if((fd=accept(srv, NULL, NULL)) < 0) {
      continue;
    }
    // A stalled client must not hold a thread for long.
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    pthread_mutex_lock(&g_active_lock);
    if(g_active >= MAX_ACTIVE || !(c=malloc(sizeof(*c)))) {
      pthread_mutex_unlock(&g_active_lock);
      reject_busy(fd);
      continue;
    }
    g_active++;
    pthread_mutex_unlock(&g_active_lock);

    c->fd = fd;
    c->handler = handler;
    if(pthread_create(&thread, &attr, conn_thread, c)) {
      pthread_mutex_lock(&g_active_lock);
      g_active--;
      pthread_mutex_unlock(&g_active_lock);
      free(c);
      reject_busy(fd);
    }
  }
  pthread_attr_destroy(&attr);
  close(srv);
}


int
http_post_local(int port, const char *path) {
  struct sockaddr_in addr;
  struct timeval tv = {2, 0};
  char req[256];
  char buf[64];
  int fd;
  int n;

  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(connect(fd, (struct sockaddr*)&addr, sizeof(addr))) {
    close(fd);
    return -1;
  }
  n = snprintf(req, sizeof(req), "POST %s HTTP/1.0\r\nHost: localhost\r\n\r\n",
               path);
  send_all(fd, req, n);
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if(n <= 0) {
    return -1;
  }
  buf[n] = 0;
  return strncmp(buf, "HTTP/1.0 200", 12) ? -1 : 0;
}
