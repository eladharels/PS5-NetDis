// Host-side tests for NetDis: the socket model, its JSON, and the routes.
// The console's socket collection is replaced by a fixed list.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "http.h"
#include "platform.h"
#include "routes.h"
#include "service.h"
#include "sockets.h"


static int failures;

#define CHECK(cond) do {                                          \
    if(!(cond)) {                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      failures++;                                                 \
    }                                                             \
  } while(0)


const char netdis_ui_html[] = "<html>NetDis</html>";
const unsigned char netdis_icon_png[] = {0x89, 'P', 'N', 'G'};
const unsigned long netdis_icon_png_size = 4;
const char common_ui_css[] = "/* css */";
const char common_ui_js[] = "/* js */";


// ---- fake console --------------------------------------------------------

static int collects;
static int diagnosed;
static int collect_fails;

static void
add(socklist_t *l, int proto, const char *laddr, int lport, const char *raddr,
    int rport, int state, int pid, const char *proc, const char *tid) {
  sock_t *s = socklist_add(l);
  s->proto = proto;
  s->ipv6 = strchr(laddr, ':') != NULL;
  snprintf(s->laddr, sizeof(s->laddr), "%s", laddr);
  s->lport = lport;
  snprintf(s->raddr, sizeof(s->raddr), "%s", raddr);
  s->rport = rport;
  s->state = state;
  s->pid = pid;
  s->fd = 3;
  snprintf(s->proc, sizeof(s->proc), "%s", proc);
  snprintf(s->tid, sizeof(s->tid), "%s", tid);
}

int
net_collect(socklist_t *out) {
  collects++;
  socklist_init(out);
  if(collect_fails) {
    return -1;
  }
  out->nprocs = 120;
  out->proc_errors = 1;
  snprintf(out->state_source, sizeof(out->state_source), "test");
  add(out, SOCK_TCP, "0.0.0.0", 9021, "", 0, SOCK_STATE_LISTEN, 50,
      "elfldr.elf", "");
  add(out, SOCK_TCP, "127.0.0.1", 8070, "", 0, SOCK_STATE_LISTEN, 60,
      "NetDis.elf", "");
  add(out, SOCK_TCP, "192.168.1.219", 8070, "192.168.1.20", 51234,
      SOCK_STATE_ESTABLISHED, 60, "NetDis.elf", "");
  add(out, SOCK_UDP, "::", 5353, "", 0, SOCK_STATE_NONE, 70,
      "SceShellCore", "NPXS40000");
  add(out, SOCK_TCP, "10.0.0.2", 443, "10.0.0.9", 40000,
      SOCK_STATE_TIME_WAIT, -1, "(no process)", "");
  return 0;
}

void
net_diagnose(void) {
  diagnosed++;
}


// ---- tests ---------------------------------------------------------------

static void
test_model(void) {
  sock_t s;

  CHECK(!strcmp(sock_state_name(SOCK_STATE_LISTEN), "LISTEN"));
  CHECK(!strcmp(sock_state_name(SOCK_STATE_TIME_WAIT), "TIME_WAIT"));
  CHECK(!strcmp(sock_state_name(SOCK_STATE_NONE), ""));
  CHECK(!strcmp(sock_state_name(99), ""));

  memset(&s, 0, sizeof(s));
  strcpy(s.laddr, "0.0.0.0");
  CHECK(!strcmp(sock_scope(&s), "any"));
  strcpy(s.laddr, "::");
  CHECK(!strcmp(sock_scope(&s), "any"));
  strcpy(s.laddr, "127.0.0.1");
  CHECK(!strcmp(sock_scope(&s), "loopback"));
  strcpy(s.laddr, "::1");
  CHECK(!strcmp(sock_scope(&s), "loopback"));
  strcpy(s.laddr, "::ffff:127.0.0.1");
  CHECK(!strcmp(sock_scope(&s), "loopback"));
  strcpy(s.laddr, "192.168.1.219");
  CHECK(!strcmp(sock_scope(&s), "address"));

  CHECK(!strcmp(sock_known_port(SOCK_TCP, 9021), "ELF loader"));
  CHECK(!strcmp(sock_known_port(SOCK_TCP, 9300), "Remote Play"));
  CHECK(!strcmp(sock_known_port(SOCK_UDP, 9300), "Remote Play"));
  CHECK(!strcmp(sock_known_port(SOCK_UDP, 5353), "mDNS"));
  CHECK(sock_known_port(SOCK_TCP, 5353) == NULL);
  CHECK(sock_known_port(SOCK_TCP, 12345) == NULL);
}


static void
test_list(void) {
  socklist_t l;
  strbuf_t sb;

  // Growth past the first allocation keeps earlier entries.
  socklist_init(&l);
  for(int i=0; i<200; i++) {
    sock_t *s = socklist_add(&l);
    CHECK(s && s->pid == -1 && s->state == SOCK_STATE_NONE);
    s->lport = i;
  }
  CHECK(l.n == 200 && l.v[0].lport == 0 && l.v[199].lport == 199);
  socklist_free(&l);
  CHECK(l.n == 0 && l.v == NULL);

  net_collect(&l);
  sb_init(&sb);
  socklist_json(&l, &sb);
  CHECK(strstr(sb.data, "\"procs\":120,\"procErrors\":1,\"stateSource\":\"test\"") != NULL);
  CHECK(strstr(sb.data, "{\"proto\":\"tcp\",\"ipv6\":false,\"laddr\":\"0.0.0.0\","
                        "\"lport\":9021,\"raddr\":\"\",\"rport\":0,"
                        "\"state\":\"LISTEN\",\"scope\":\"any\",\"pid\":50,"
                        "\"fd\":3,\"proc\":\"elfldr.elf\",\"tid\":\"\","
                        "\"known\":\"ELF loader\"}") != NULL);
  CHECK(strstr(sb.data, "\"proto\":\"udp\",\"ipv6\":true,\"laddr\":\"::\"") != NULL);
  CHECK(strstr(sb.data, "\"tid\":\"NPXS40000\",\"known\":\"mDNS\"") != NULL);
  CHECK(strstr(sb.data, "\"state\":\"TIME_WAIT\",\"scope\":\"address\",\"pid\":-1") != NULL);
  sb_free(&sb);
  socklist_free(&l);
}


static void
request(const char *method, const char *target, http_resp_t *resp) {
  char raw[600];
  http_req_t req;

  snprintf(raw, sizeof(raw), "%s %s HTTP/1.1\r\nHost: ps5\r\n\r\n", method, target);
  memset(resp, 0, sizeof(*resp));
  sb_init(&resp->body);
  resp->status = 200;
  resp->type = "application/json";
  CHECK(http_parse_request(raw, &req) == 0);
  netdis_handle(&req, resp);
  // What the HTTP layer does once the reply is sent.
  if(resp->after) {
    resp->after();
  }
}


static void
test_routes(void) {
  char dir[] = "/tmp/netdis-test-XXXXXX";
  service_info_t info = {
    .name = "NetDis", .proc_name = "NetDis.elf", .port = 8070,
    .tile_tid = "NETD00001", .icon = netdis_icon_png,
    .icon_size = sizeof(netdis_icon_png),
  };
  http_resp_t resp;
  int before;

  CHECK(mkdtemp(dir) != NULL);
  info.data_dir = dir;
  service_setup(&info);

  request("GET", "/", &resp);
  CHECK(resp.status == 200 && strstr(resp.body.data, "NetDis") &&
        !strncmp(resp.type, "text/html", 9));
  sb_free(&resp.body);

  // A second request within a second reuses the collection.
  before = collects;
  request("GET", "/api/sockets", &resp);
  CHECK(resp.status == 200 && strstr(resp.body.data, "\"lport\":9021"));
  sb_free(&resp.body);
  request("GET", "/api/sockets", &resp);
  CHECK(resp.status == 200 && collects == before + 1);
  sb_free(&resp.body);
  sleep(1);
  usleep(100 * 1000);
  request("GET", "/api/sockets", &resp);
  CHECK(resp.status == 200 && collects == before + 2);
  sb_free(&resp.body);

  // A failed collection is an error, not an empty list.
  sleep(1);
  usleep(100 * 1000);
  collect_fails = 1;
  request("GET", "/api/sockets", &resp);
  CHECK(resp.status == 500 && strstr(resp.body.data, "Logs tab"));
  sb_free(&resp.body);
  collect_fails = 0;

  request("POST", "/api/sockets", &resp);
  CHECK(resp.status == 405);
  sb_free(&resp.body);
  request("GET", "/api/diagnose", &resp);
  CHECK(resp.status == 405 && diagnosed == 0);
  sb_free(&resp.body);
  request("POST", "/api/diagnose", &resp);
  CHECK(resp.status == 200 && diagnosed == 1);
  sb_free(&resp.body);
  request("GET", "/nope", &resp);
  CHECK(resp.status == 404);
  sb_free(&resp.body);

  // Shared routes carry NetDis's own name, log and tile.
  request("GET", "/api/service", &resp);
  CHECK(resp.status == 200 && strstr(resp.body.data, "\"name\":\"NetDis\""));
  CHECK(strstr(resp.body.data, "\"port\":8070") != NULL);
  CHECK(strstr(resp.body.data, "\"tileTid\":\"NETD00001\"") != NULL);
  CHECK(strstr(resp.body.data, "\"logRoute\":\"/NetDis.log\"") != NULL);
  sb_free(&resp.body);
  request("GET", "/NetDis.log", &resp);
  CHECK(resp.status == 200 && !strncmp(resp.type, "text/plain", 10));
  sb_free(&resp.body);
  request("GET", "/icon.png", &resp);
  CHECK(resp.status == 200 && resp.body.len == 4);
  sb_free(&resp.body);
  request("POST", "/api/quit", &resp);
  CHECK(resp.status == 200 && service_stop);
  sb_free(&resp.body);
  service_stop = 0;

  rmdir(dir);
}


static int serve_srv;
static int serve_done;

static void *
serve_thread(void *arg) {
  (void)arg;
  http_serve(serve_srv, netdis_handle, &service_stop);
  serve_done = 1;
  return NULL;
}


static long
ms_since(const struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - start->tv_sec) * 1000 +
         (now.tv_nsec - start->tv_nsec) / 1000000;
}


static int
connect_local(int port) {
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  struct timeval tv = {2, 0};  // fail instead of hanging on a stuck server

  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(fd >= 0) {
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  if(fd >= 0 && connect(fd, (struct sockaddr*)&addr, sizeof(addr))) {
    close(fd);
    return -1;
  }
  return fd;
}


// The real server: an idle client must not hold up others, and a quit is
// answered before the server stops.
static void
test_server(void) {
  const int port = 18070;
  struct timespec start;
  pthread_t thread;
  char buf[256];
  int idle, fd, n;

  CHECK((serve_srv=http_listen(port)) >= 0);
  if(serve_srv < 0) {
    return;
  }
  service_stop = 0;
  pthread_create(&thread, NULL, serve_thread, NULL);

  CHECK((idle=connect_local(port)) >= 0);  // connects, never sends
  usleep(100 * 1000);

  clock_gettime(CLOCK_MONOTONIC, &start);
  CHECK((fd=connect_local(port)) >= 0);
  if(fd >= 0) {
    static const char req[] = "GET /api/service HTTP/1.0\r\n\r\n";
    CHECK(write(fd, req, sizeof(req) - 1) == sizeof(req) - 1);
    n = read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    CHECK(!strncmp(buf, "HTTP/1.0 200", 12));
    CHECK(ms_since(&start) < 1000);
    close(fd);
  }

  CHECK(http_post_local(port, "/api/quit") == 0);  // got its 200
  clock_gettime(CLOCK_MONOTONIC, &start);
  while(!serve_done && ms_since(&start) < 2000) {
    usleep(20 * 1000);
  }
  CHECK(serve_done);
  if(serve_done) {
    pthread_join(thread, NULL);
  } else {
    pthread_detach(thread);  // report the failure instead of hanging
  }
  if(idle >= 0) {
    close(idle);
  }
  service_stop = 0;
}


int
main(void) {
  signal(SIGPIPE, SIG_IGN);  // as service_start() does on the console
  test_model();
  test_list();
  test_routes();
  test_server();
  if(failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("netdis tests passed\n");
  return 0;
}
