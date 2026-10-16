#include <pthread.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "platform.h"
#include "routes.h"
#include "service.h"


// Collecting walks every process, so answers within a second share one run.
// Requests run on their own threads; the lock also keeps two collections
// from running at once.
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static socklist_t g_cache;
static struct timespec g_cache_time;
static int g_cached;


static int
cache_fresh(void) {
  struct timespec now;
  long ms;

  if(!g_cached) {
    return 0;
  }
  clock_gettime(CLOCK_MONOTONIC, &now);
  ms = (now.tv_sec - g_cache_time.tv_sec) * 1000 +
       (now.tv_nsec - g_cache_time.tv_nsec) / 1000000;
  return ms >= 0 && ms < 1000;
}


static int
sockets_json(strbuf_t *sb) {
  pthread_mutex_lock(&g_cache_lock);
  if(!cache_fresh()) {
    socklist_t fresh;
    if(net_collect(&fresh)) {
      pthread_mutex_unlock(&g_cache_lock);
      return -1;
    }
    socklist_free(&g_cache);
    g_cache = fresh;
    g_cached = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_cache_time);
  }
  socklist_json(&g_cache, sb);
  pthread_mutex_unlock(&g_cache_lock);
  return 0;
}


void
netdis_handle(const http_req_t *req, http_resp_t *resp) {
  int is_get = !strcmp(req->method, "GET");
  int is_post = !strcmp(req->method, "POST");

  if(service_handle(req, resp)) {
    return;
  }

  if(!strcmp(req->path, "/") || !strcmp(req->path, "/index.html")) {
    if(!is_get) {
      goto not_allowed;
    }
    resp->type = "text/html; charset=utf-8";
    sb_puts(&resp->body, netdis_ui_html);
    return;
  }

  if(!strcmp(req->path, "/api/sockets")) {
    if(!is_get) {
      goto not_allowed;
    }
    if(sockets_json(&resp->body)) {
      resp->status = 500;
      sb_free(&resp->body);
      sb_puts(&resp->body, "{\"error\":\"could not list sockets; see the Logs tab\"}");
    }
    return;
  }

  if(!strcmp(req->path, "/api/diagnose")) {
    if(!is_post) {
      goto not_allowed;
    }
    LOGI("diagnose requested from the web page");
    pthread_mutex_lock(&g_cache_lock);
    net_diagnose();
    pthread_mutex_unlock(&g_cache_lock);
    sb_puts(&resp->body, "{\"ok\":true}");
    return;
  }

  resp->status = 404;
  sb_puts(&resp->body, "{\"error\":\"not found\"}");
  return;

 not_allowed:
  resp->status = 405;
  sb_puts(&resp->body, "{\"error\":\"method not allowed\"}");
}
