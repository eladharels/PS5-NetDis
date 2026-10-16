#pragma once

#include <stdint.h>
#include <sys/types.h>

#include "util.h"

// One TCP or UDP socket, with the process that has it open.

enum { SOCK_TCP = 1, SOCK_UDP = 2 };

// TCP states, numbered as in FreeBSD's tcp_fsm.h. SOCK_STATE_NONE for UDP
// and for TCP sockets whose state could not be read.
enum {
  SOCK_STATE_NONE = -1,
  SOCK_STATE_CLOSED = 0, SOCK_STATE_LISTEN, SOCK_STATE_SYN_SENT,
  SOCK_STATE_SYN_RECEIVED, SOCK_STATE_ESTABLISHED, SOCK_STATE_CLOSE_WAIT,
  SOCK_STATE_FIN_WAIT_1, SOCK_STATE_CLOSING, SOCK_STATE_LAST_ACK,
  SOCK_STATE_FIN_WAIT_2, SOCK_STATE_TIME_WAIT,
};

typedef struct sock {
  int      proto;       // SOCK_TCP or SOCK_UDP
  int      ipv6;
  char     laddr[48];
  int      lport;
  char     raddr[48];   // "" when not connected
  int      rport;
  int      state;       // SOCK_STATE_*
  pid_t    pid;         // -1 when no process has it open (e.g. TIME_WAIT)
  int      fd;
  char     proc[32];    // process name
  char     tid[16];     // title ID of the owning app, if any
} sock_t;

typedef struct socklist {
  sock_t *v;
  int     n;
  int     cap;
  int     nprocs;       // processes inspected
  int     proc_errors;  // processes whose descriptors could not be read
  char    state_source[64];  // how TCP states were obtained, for the page
} socklist_t;

void    socklist_init(socklist_t *l);
void    socklist_free(socklist_t *l);
sock_t *socklist_add(socklist_t *l);

const char *sock_state_name(int state);

// "any" for wildcard addresses, "loopback" for 127.0.0.0/8 and ::1, else
// "address" (bound to one interface).
const char *sock_scope(const sock_t *s);

// What usually listens on a local port, or NULL.
const char *sock_known_port(int proto, int port);

// Writes {"sockets":[...],"procs":N,...} for the page.
void socklist_json(const socklist_t *l, strbuf_t *sb);
