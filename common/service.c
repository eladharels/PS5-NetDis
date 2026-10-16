#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "service.h"
#include "sys.h"


volatile sig_atomic_t service_stop;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static service_info_t g_info;
// Own copies of the strings in g_info; callers may pass temporary buffers.
static char g_name[32], g_proc_name[32], g_data_dir[200], g_tile_tid[16];
static int g_srv = -1;
static int g_replaced;
static int g_tile_installed;
static char g_no_tile[256];   // exists when the user removed the tile
static char g_log_path[256];
static char g_log_route[64];
static char g_lan_url[96];


// Payload Manager's Kill sends SIGTERM first. Payloads write their files
// with atomic renames, so exiting at any point is safe.
static void
on_stop_signal(int sig) {
  (void)sig;
  service_stop = 1;
}


void
service_exit(void) {
  static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;

  // The server and a worker thread can both get here; one exit is enough.
  if(pthread_mutex_trylock(&once)) {
    for(;;) {
      pause();
    }
  }
  LOGI("%s stopped", g_info.name);
  exit(0);
}


// Set from the quit route once its reply is on the wire, so the caller (the
// page, or a newer copy taking over) always gets an answer.
static void
request_stop(void) {
  service_stop = 1;
}


void
service_setup(const service_info_t *info) {
  g_info = *info;
  snprintf(g_name, sizeof(g_name), "%s", info->name);
  snprintf(g_proc_name, sizeof(g_proc_name), "%s", info->proc_name);
  snprintf(g_data_dir, sizeof(g_data_dir), "%s", info->data_dir);
  snprintf(g_tile_tid, sizeof(g_tile_tid), "%s", info->tile_tid);
  g_info.name = g_name;
  g_info.proc_name = g_proc_name;
  g_info.data_dir = g_data_dir;
  g_info.tile_tid = g_tile_tid;
  log_set_name(g_name);
  snprintf(g_no_tile, sizeof(g_no_tile), "%s/no-tile", info->data_dir);
  snprintf(g_log_path, sizeof(g_log_path), "%s/%s.log", info->data_dir,
           info->name);
  snprintf(g_log_route, sizeof(g_log_route), "/%s.log", info->name);
}


// Binds the web port. If an older copy holds it, asks that one to quit and
// waits for the port, so re-sending the payload replaces the running copy.
static int
take_port(int *replaced) {
  int port = g_info.port;
  int srv = http_listen(port);

  *replaced = 0;
  if(srv >= 0 || errno != EADDRINUSE) {
    return srv;
  }
  LOGI("port %d is in use; asking the running %s to stop", port, g_info.name);
  // Ask again every 2 s for up to 15 s: the running copy may be busy, and
  // it stops within a second of answering.
  for(int i=0; i<150 && srv < 0; i++) {
    if(i % 20 == 0 && http_post_local(port, "/api/quit") && !i) {
      LOGW("nothing on port %d answered as %s yet", port, g_info.name);
    }
    usleep(100 * 1000);
    srv = http_listen(port);
  }
  *replaced = srv >= 0;
  return srv;
}


int
service_start(const service_info_t *info) {
  struct sigaction sa;
  char msg[128];

  service_setup(info);
  info = &g_info;
  signal(SIGPIPE, SIG_IGN);
  // No SA_RESTART: a stop signal interrupts accept() so the server can exit.
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_stop_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  mkdir(info->data_dir, 0755);
  log_open(g_log_path);
  LOGI("%s starting (built " __DATE__ " " __TIME__ "), pid %d, log file %s",
       info->name, (int)getpid(), g_log_path);
  sys_set_process_name(info->proc_name);

  // Before anything else, so two copies never run side by side.
  if((g_srv=take_port(&g_replaced)) < 0) {
    LOGE("could not listen on port %d: %s", info->port, strerror(errno));
    snprintf(msg, sizeof(msg), "%s: port %d is busy and the running copy "
             "did not stop", info->name, info->port);
    sys_notify(msg);
    return -1;
  }
  if(g_replaced) {
    LOGI("replaced the %s that was running", info->name);
  }
  return 0;
}


// First non-loopback IPv4 address, for opening the page on a phone.
static void
lan_address(char *out, size_t len) {
  struct ifaddrs *ifs;

  snprintf(out, len, "<ps5-ip>");
  if(getifaddrs(&ifs)) {
    return;
  }
  for(struct ifaddrs *i=ifs; i; i=i->ifa_next) {
    struct sockaddr_in *sin = (struct sockaddr_in*)i->ifa_addr;
    if(!sin || sin->sin_family != AF_INET ||
       ntohl(sin->sin_addr.s_addr) >> 24 == 127) {
      continue;
    }
    inet_ntop(AF_INET, &sin->sin_addr, out, len);
    break;
  }
  freeifaddrs(ifs);
}


static sys_tile_t
tile_of(const service_info_t *info) {
  sys_tile_t tile = {
    .tid = info->tile_tid,
    .title = info->name,
    .port = info->port,
    .icon = info->icon,
    .icon_size = info->icon_size,
  };
  return tile;
}


void
service_announce(void) {
  sys_tile_t tile = tile_of(&g_info);
  struct stat st;
  char ip[64];
  char msg[160];

  pthread_mutex_lock(&g_lock);
  if(!stat(g_no_tile, &st)) {
    LOGI("tile: not installing; it was removed from the web page");
  } else {
    sys_tile_install(&tile);
  }
  g_tile_installed = sys_tile_installed(g_info.tile_tid);

  lan_address(ip, sizeof(ip));
  snprintf(g_lan_url, sizeof(g_lan_url), "http://%s:%d/", ip, g_info.port);
  pthread_mutex_unlock(&g_lock);

  LOGI("web page at %s", g_lan_url);
  snprintf(msg, sizeof(msg), "%s %s: %s", g_info.name,
           g_replaced ? "restarted" : "running", g_lan_url);
  sys_notify(msg);
}


void
service_run(http_handler_t handler) {
  http_serve(g_srv, handler, &service_stop);
  service_exit();
}


int
service_tile_set(int on) {
  sys_tile_t tile = tile_of(&g_info);
  FILE *f;
  int rc;

  pthread_mutex_lock(&g_lock);
  if(on) {
    unlink(g_no_tile);
    rc = sys_tile_install(&tile);
  } else {
    if((f=fopen(g_no_tile, "w"))) {
      fclose(f);
    }
    rc = sys_tile_remove(g_info.tile_tid);
  }
  g_tile_installed = sys_tile_installed(g_info.tile_tid);
  pthread_mutex_unlock(&g_lock);
  return rc;
}


static void
service_json(strbuf_t *sb) {
  pthread_mutex_lock(&g_lock);
  sb_puts(sb, "{\"name\":");
  sb_json_str(sb, g_info.name);
  sb_printf(sb, ",\"build\":\"%s %s\",\"port\":%d,\"tile\":%s,\"tileTid\":",
            __DATE__, __TIME__, g_info.port,
            g_tile_installed ? "true" : "false");
  sb_json_str(sb, g_info.tile_tid);
  sb_puts(sb, ",\"lanUrl\":");
  sb_json_str(sb, g_lan_url);
  sb_puts(sb, ",\"dataDir\":");
  sb_json_str(sb, g_info.data_dir);
  sb_puts(sb, ",\"logFile\":");
  sb_json_str(sb, g_log_path);
  sb_puts(sb, ",\"logRoute\":");
  sb_json_str(sb, g_log_route);
  sb_puts(sb, "}");
  pthread_mutex_unlock(&g_lock);
}


int
service_handle(const http_req_t *req, http_resp_t *resp) {
  int is_get = !strcmp(req->method, "GET");
  int is_post = !strcmp(req->method, "POST");
  const char *p = req->path;

  if(!strcmp(p, "/ui/common.css") || !strcmp(p, "/ui/common.js") ||
     !strcmp(p, "/icon.png")) {
    if(!is_get) {
      goto not_allowed;
    }
    if(!strcmp(p, "/icon.png")) {
      resp->type = "image/png";
      sb_append(&resp->body, (const char*)g_info.icon, g_info.icon_size);
    } else if(!strcmp(p, "/ui/common.css")) {
      resp->type = "text/css; charset=utf-8";
      sb_puts(&resp->body, common_ui_css);
    } else {
      resp->type = "text/javascript; charset=utf-8";
      sb_puts(&resp->body, common_ui_js);
    }
    return 1;
  }

  if(!strcmp(p, "/api/service")) {
    if(!is_get) {
      goto not_allowed;
    }
    service_json(&resp->body);
    return 1;
  }

  if(!strcmp(p, "/api/logs")) {
    char since[24] = "0";
    if(!is_get) {
      goto not_allowed;
    }
    http_query_get(req->query, "since", since, sizeof(since));
    log_json(&resp->body, strtoul(since, NULL, 10));
    return 1;
  }

  if(!strcmp(p, g_log_route)) {
    if(!is_get) {
      goto not_allowed;
    }
    resp->type = "text/plain; charset=utf-8";
    log_text(&resp->body);
    return 1;
  }

  if(!strcmp(p, "/api/quit")) {
    if(!is_post) {
      goto not_allowed;
    }
    LOGI("stop requested from the web page or a newer %s", g_info.name);
    resp->after = request_stop;
    sb_puts(&resp->body, "{\"ok\":true}");
    return 1;
  }

  if(!strcmp(p, "/api/tile")) {
    char on[4];
    if(!is_post) {
      goto not_allowed;
    }
    if(http_query_get(req->query, "on", on, sizeof(on))) {
      resp->status = 400;
      sb_puts(&resp->body, "{\"error\":\"missing on\"}");
      return 1;
    }
    if(service_tile_set(on[0] == '1')) {
      resp->status = 500;
      sb_puts(&resp->body, "{\"error\":\"see the Logs tab\"}");
      return 1;
    }
    service_json(&resp->body);
    return 1;
  }

  return 0;

 not_allowed:
  resp->status = 405;
  sb_puts(&resp->body, "{\"error\":\"method not allowed\"}");
  return 1;
}
