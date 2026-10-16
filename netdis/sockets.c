#include <stdlib.h>
#include <string.h>

#include "sockets.h"


void
socklist_init(socklist_t *l) {
  memset(l, 0, sizeof(*l));
}


void
socklist_free(socklist_t *l) {
  free(l->v);
  socklist_init(l);
}


sock_t *
socklist_add(socklist_t *l) {
  sock_t *s;

  if(l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 64;
    sock_t *v = realloc(l->v, cap * sizeof(*v));
    if(!v) {
      return NULL;
    }
    l->v = v;
    l->cap = cap;
  }
  s = &l->v[l->n++];
  memset(s, 0, sizeof(*s));
  s->state = SOCK_STATE_NONE;
  s->pid = -1;
  s->fd = -1;
  return s;
}


const char *
sock_state_name(int state) {
  static const char *const names[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED",
    "CLOSE_WAIT", "FIN_WAIT_1", "CLOSING", "LAST_ACK", "FIN_WAIT_2",
    "TIME_WAIT",
  };
  if(state < 0 || state >= (int)(sizeof(names) / sizeof(names[0]))) {
    return "";
  }
  return names[state];
}


const char *
sock_scope(const sock_t *s) {
  if(!strcmp(s->laddr, "0.0.0.0") || !strcmp(s->laddr, "::") ||
     !s->laddr[0]) {
    return "any";
  }
  if(!strncmp(s->laddr, "127.", 4) || !strcmp(s->laddr, "::1") ||
     !strncmp(s->laddr, "::ffff:127.", 11)) {
    return "loopback";
  }
  return "address";
}


typedef struct known {
  int         proto;   // 0 for both
  int         lo, hi;
  const char *what;
} known_t;

// Ports homebrew tools and the system commonly use. A hint only: the
// process name on the same row is what actually owns the socket.
static const known_t KNOWN[] = {
  {SOCK_TCP,  9021,  9021, "ELF loader"},
  {SOCK_TCP,  9020,  9020, "payload loader"},
  {SOCK_TCP,  2121,  2121, "FTP server"},
  {SOCK_TCP,  1337,  1337, "FTP server"},
  {SOCK_TCP,  3232,  3232, "kernel log server"},
  {SOCK_TCP,  2159,  2159, "GDB server"},
  {SOCK_TCP,  8080,  8080, "web server"},
  {SOCK_TCP,  8084,  8084, "Payload Manager"},
  {SOCK_TCP,  8060,  8060, "fps60"},
  {SOCK_TCP,  8070,  8070, "NetDis"},
  {0,         9295,  9304, "Remote Play"},
  {SOCK_UDP,  1900,  1900, "SSDP discovery"},
  {SOCK_UDP,  5353,  5353, "mDNS"},
  {SOCK_UDP,  3478,  3480, "PSN (STUN)"},
  {SOCK_UDP,    53,    53, "DNS"},
  {SOCK_UDP,    67,    68, "DHCP"},
  {SOCK_UDP,   123,   123, "NTP"},
};


const char *
sock_known_port(int proto, int port) {
  for(size_t i=0; i<sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
    const known_t *k = &KNOWN[i];
    if((!k->proto || k->proto == proto) && port >= k->lo && port <= k->hi) {
      return k->what;
    }
  }
  return NULL;
}


void
socklist_json(const socklist_t *l, strbuf_t *sb) {
  sb_printf(sb, "{\"procs\":%d,\"procErrors\":%d,\"stateSource\":",
            l->nprocs, l->proc_errors);
  sb_json_str(sb, l->state_source);
  sb_puts(sb, ",\"sockets\":[");
  for(int i=0; i<l->n; i++) {
    const sock_t *s = &l->v[i];
    const char *known = sock_known_port(s->proto, s->lport);

    sb_printf(sb, "%s{\"proto\":\"%s\",\"ipv6\":%s,\"laddr\":", i ? "," : "",
              s->proto == SOCK_TCP ? "tcp" : "udp", s->ipv6 ? "true" : "false");
    sb_json_str(sb, s->laddr);
    sb_printf(sb, ",\"lport\":%d,\"raddr\":", s->lport);
    sb_json_str(sb, s->raddr);
    sb_printf(sb, ",\"rport\":%d,\"state\":\"%s\",\"scope\":\"%s\","
              "\"pid\":%d,\"fd\":%d,\"proc\":", s->rport,
              sock_state_name(s->state), sock_scope(s), (int)s->pid, s->fd);
    sb_json_str(sb, s->proc);
    sb_puts(sb, ",\"tid\":");
    sb_json_str(sb, s->tid);
    sb_puts(sb, ",\"known\":");
    sb_json_str(sb, known ? known : "");
    sb_puts(sb, "}");
  }
  sb_puts(sb, "]}");
}
