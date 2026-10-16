// Socket collection on the console.
//
// Ownership and addresses come from each process's descriptor table
// (kern.proc.filedesc, the interface FreeBSD's procstat uses). Its record
// layout is a user-facing ABI, so it does not depend on Sony's kernel
// internals.
//
// TCP states come from net.inet.tcp.pcblist, whose records embed kernel
// structures. They are used as the SDK headers describe them only after the
// record sizes match; otherwise only the listening flag is taken, from the
// xsocket part, which is found by its size marker and checked.

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "platform.h"


typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  uint32_t app_type;
  char     title_id[10];
  char     unknown2[0x3c];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);


// ---- TCP states --------------------------------------------------------

typedef struct pcb_state {
  uint64_t pcb;     // kernel address of the inpcb (so_pcb)
  int      state;
  int      owned;   // seen in some process's descriptor table
} pcb_state_t;

typedef struct tcp_table {
  pcb_state_t *v;
  int          n;
  int          trusted;   // records match the SDK headers
  char        *buf;       // raw pcblist, kept for orphan sockets
  size_t       len;
} tcp_table_t;

static int g_logged_mode;


// Reads a sysctl by name into a fresh buffer.
static char *
sysctl_read(const char *name, size_t *len) {
  char *buf = NULL;

  for(int attempt=0; attempt<3; attempt++) {
    size_t size = 0;
    if(sysctlbyname(name, NULL, &size, NULL, 0) || !size) {
      return NULL;
    }
    size += size / 4 + 4096;
    if(!(buf=malloc(size))) {
      return NULL;
    }
    if(!sysctlbyname(name, buf, &size, NULL, 0)) {
      *len = size;
      return buf;
    }
    free(buf);
    buf = NULL;
    if(errno != ENOMEM) {
      return NULL;
    }
  }
  return NULL;
}


// Finds the xsocket inside a record whose layout is unknown: a size_t equal
// to sizeof(struct xsocket), followed by a sane family and protocol.
static const struct xsocket *
find_xsocket(const char *rec, size_t len) {
  long off = ((long)len - (long)sizeof(struct xsocket)) & ~7L;

  for(; off >= (long)sizeof(size_t); off -= 8) {
    const struct xsocket *xs = (const struct xsocket*)(rec + off);
    if(xs->xso_len == sizeof(struct xsocket) &&
       (xs->xso_family == AF_INET || xs->xso_family == AF_INET6) &&
       xs->xso_protocol == IPPROTO_TCP) {
      return xs;
    }
  }
  return NULL;
}


static int
tcp_table_add(tcp_table_t *t, uint64_t pcb, int state) {
  pcb_state_t *v = realloc(t->v, (t->n + 1) * sizeof(*v));
  if(!v) {
    return -1;
  }
  t->v = v;
  t->v[t->n].pcb = pcb;
  t->v[t->n].state = state;
  t->v[t->n].owned = 0;
  t->n++;
  return 0;
}


static void
tcp_table_load(tcp_table_t *t, char *mode, size_t modelen) {
  const struct xinpgen *gen;
  size_t first = 0;
  size_t len;
  char *buf;
  int fallback = 0;

  memset(t, 0, sizeof(*t));
  if(!(buf=sysctl_read("net.inet.tcp.pcblist", &len))) {
    snprintf(mode, modelen, "unavailable (%s)", strerror(errno));
    return;
  }
  gen = (const struct xinpgen*)buf;
  if(len < sizeof(*gen) || gen->xig_len != sizeof(struct xinpgen)) {
    snprintf(mode, modelen, "unreadable list header");
    free(buf);
    return;
  }

  if(gen->xig_len + sizeof(size_t) <= len) {
    first = *(const size_t*)(buf + gen->xig_len);
  }
  for(size_t off=gen->xig_len; off + sizeof(size_t) <= len;) {
    const char *rec = buf + off;
    size_t rlen = *(const size_t*)rec;

    if(rlen == sizeof(struct xinpgen) || rlen < sizeof(size_t) ||
       off + rlen > len) {
      break;  // trailing generation record, or garbage
    }
    if(rlen == sizeof(struct xtcpcb)) {
      const struct xtcpcb *xt = (const struct xtcpcb*)rec;
      t->trusted = 1;
      tcp_table_add(t, (uint64_t)(uintptr_t)xt->xt_socket.so_pcb,
                    xt->xt_tp.t_state);
    } else {
      const struct xsocket *xs = find_xsocket(rec, rlen);
      fallback = 1;
      if(xs) {
        tcp_table_add(t, (uint64_t)(uintptr_t)xs->so_pcb,
                      (xs->so_options & SO_ACCEPTCONN) ?
                      SOCK_STATE_LISTEN : SOCK_STATE_NONE);
      }
    }
    off += rlen;
  }

  if(t->trusted && !fallback) {
    snprintf(mode, modelen, "kernel TCP list (full states)");
    t->buf = buf;
    t->len = len;
  } else {
    t->trusted = 0;
    snprintf(mode, modelen, "kernel TCP list (listening only; layout differs)");
    free(buf);
  }
  if(!g_logged_mode) {
    LOGI("TCP states: %s; %d TCP sockets, record size %zu, headers expect %zu",
         mode, t->n, first, sizeof(struct xtcpcb));
    g_logged_mode = 1;
  }
}


static pcb_state_t *
tcp_table_find(tcp_table_t *t, uint64_t pcb) {
  for(int i=0; i<t->n; i++) {
    if(t->v[i].pcb == pcb) {
      return &t->v[i];
    }
  }
  return NULL;
}


// ---- addresses ---------------------------------------------------------

static void
sa_format(const struct sockaddr_storage *ss, char *addr, size_t len,
          int *port) {
  addr[0] = 0;
  *port = 0;
  if(ss->ss_family == AF_INET) {
    const struct sockaddr_in *sin = (const struct sockaddr_in*)ss;
    inet_ntop(AF_INET, &sin->sin_addr, addr, len);
    *port = ntohs(sin->sin_port);
  } else if(ss->ss_family == AF_INET6) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6*)ss;
    inet_ntop(AF_INET6, &sin6->sin6_addr, addr, len);
    *port = ntohs(sin6->sin6_port);
  }
}


// Unconnected sockets report a zero peer; show those as no remote end.
static void
clear_unconnected(sock_t *s) {
  if(!s->rport && (!strcmp(s->raddr, "0.0.0.0") || !strcmp(s->raddr, "::"))) {
    s->raddr[0] = 0;
  }
}


// ---- processes ---------------------------------------------------------

static void
proc_title(pid_t pid, char *tid, size_t len) {
  app_info_t info;

  tid[0] = 0;
  memset(&info, 0, sizeof(info));
  if(sceKernelGetAppInfo(pid, &info)) {
    return;
  }
  info.title_id[sizeof(info.title_id) - 1] = 0;
  if(is_title_id(info.title_id)) {
    snprintf(tid, len, "%s", info.title_id);
  }
}


static int
proc_sockets(const struct kinfo_proc *ki, tcp_table_t *tcp, socklist_t *out) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_FILEDESC, ki->ki_pid};
  char tid[16] = "";
  int titled = 0;
  size_t size = 0;
  char *buf;

  if(sysctl(mib, 4, NULL, &size, NULL, 0)) {
    return -1;
  }
  if(!size) {
    return 0;
  }
  size += size / 4 + 4096;
  if(!(buf=malloc(size))) {
    return -1;
  }
  if(sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }

  for(char *p=buf; p + offsetof(struct kinfo_file, kf_path) <= buf + size;) {
    const struct kinfo_file *kf = (const struct kinfo_file*)p;
    int proto;
    sock_t *s;

    if(kf->kf_structsize <= 0) {
      break;
    }
    p += kf->kf_structsize;
    if(kf->kf_type != KF_TYPE_SOCKET ||
       (kf->kf_sock_domain != AF_INET && kf->kf_sock_domain != AF_INET6)) {
      continue;
    }
    if(kf->kf_sock_protocol == IPPROTO_TCP) {
      proto = SOCK_TCP;
    } else if(kf->kf_sock_protocol == IPPROTO_UDP) {
      proto = SOCK_UDP;
    } else {
      continue;
    }
    if(!(s=socklist_add(out))) {
      break;
    }
    if(!titled) {
      proc_title(ki->ki_pid, tid, sizeof(tid));
      titled = 1;
    }
    s->proto = proto;
    s->ipv6 = kf->kf_sock_domain == AF_INET6;
    s->pid = ki->ki_pid;
    s->fd = kf->kf_fd;
    snprintf(s->proc, sizeof(s->proc), "%s", ki->ki_comm);
    snprintf(s->tid, sizeof(s->tid), "%s", tid);
    sa_format(&kf->kf_sa_local, s->laddr, sizeof(s->laddr), &s->lport);
    sa_format(&kf->kf_sa_peer, s->raddr, sizeof(s->raddr), &s->rport);
    clear_unconnected(s);

    if(proto == SOCK_TCP) {
      pcb_state_t *ps = tcp_table_find(tcp, kf->kf_un.kf_sock.kf_sock_pcb);
      if(ps) {
        s->state = ps->state;
        ps->owned = 1;
      }
    }
  }

  free(buf);
  return 0;
}


// TCP sockets no process holds any more, such as TIME_WAIT. Only with the
// trusted layout, since their addresses come from the kernel structures.
static void
orphan_sockets(tcp_table_t *tcp, socklist_t *out) {
  const struct xinpgen *gen;

  if(!tcp->trusted || !tcp->buf) {
    return;
  }
  gen = (const struct xinpgen*)tcp->buf;
  for(size_t off=gen->xig_len; off + sizeof(struct xtcpcb) <= tcp->len;) {
    const struct xtcpcb *xt = (const struct xtcpcb*)(tcp->buf + off);
    const struct inpcb *inp = &xt->xt_inp;
    pcb_state_t *ps;
    sock_t *s;

    if(xt->xt_len != sizeof(struct xtcpcb)) {
      break;
    }
    off += xt->xt_len;
    ps = tcp_table_find(tcp, (uint64_t)(uintptr_t)xt->xt_socket.so_pcb);
    if(ps && ps->owned) {
      continue;
    }
    if(!(s=socklist_add(out))) {
      return;
    }
    s->proto = SOCK_TCP;
    s->state = xt->xt_tp.t_state;
    snprintf(s->proc, sizeof(s->proc), "(no process)");
    s->lport = ntohs(inp->inp_lport);
    s->rport = ntohs(inp->inp_fport);
    if(inp->inp_vflag & INP_IPV6) {
      s->ipv6 = 1;
      inet_ntop(AF_INET6, &inp->in6p_laddr, s->laddr, sizeof(s->laddr));
      inet_ntop(AF_INET6, &inp->in6p_faddr, s->raddr, sizeof(s->raddr));
    } else {
      inet_ntop(AF_INET, &inp->inp_laddr, s->laddr, sizeof(s->laddr));
      inet_ntop(AF_INET, &inp->inp_faddr, s->raddr, sizeof(s->raddr));
    }
    clear_unconnected(s);
  }
}


int
net_collect(socklist_t *out) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  tcp_table_t tcp;
  size_t size = 0;
  char *buf;

  socklist_init(out);
  tcp_table_load(&tcp, out->state_source, sizeof(out->state_source));

  if(sysctl(mib, 4, NULL, &size, NULL, 0) ||
     !(buf=malloc(size += size / 4 + 4096))) {
    LOGE("could not list processes: %s", strerror(errno));
    free(tcp.v);
    free(tcp.buf);
    return -1;
  }
  if(sysctl(mib, 4, buf, &size, NULL, 0)) {
    LOGE("could not list processes: %s", strerror(errno));
    free(buf);
    free(tcp.v);
    free(tcp.buf);
    return -1;
  }

  for(char *p=buf; p < buf + size;) {
    const struct kinfo_proc *ki = (const struct kinfo_proc*)p;
    if(ki->ki_structsize <= 0) {
      break;
    }
    p += ki->ki_structsize;
    out->nprocs++;
    if(proc_sockets(ki, &tcp, out)) {
      out->proc_errors++;
    }
  }
  free(buf);

  orphan_sockets(&tcp, out);
  free(tcp.v);
  free(tcp.buf);
  return 0;
}


void
net_diagnose(void) {
  socklist_t l;
  int tcp = 0, udp = 0, listen = 0, unknown = 0;

  g_logged_mode = 0;  // log the TCP state source again
  if(net_collect(&l)) {
    return;
  }
  for(int i=0; i<l.n; i++) {
    const sock_t *s = &l.v[i];
    if(s->proto == SOCK_TCP) {
      tcp++;
      listen += s->state == SOCK_STATE_LISTEN;
      unknown += s->state == SOCK_STATE_NONE;
    } else {
      udp++;
    }
  }
  LOGI("diagnose: %d processes, %d unreadable; %d TCP (%d listening, %d "
       "without state), %d UDP", l.nprocs, l.proc_errors, tcp, listen, unknown,
       udp);
  LOGI("diagnose: kinfo_file %zu bytes, kinfo_proc %zu, xtcpcb %zu, "
       "xinpgen %zu, xsocket %zu", sizeof(struct kinfo_file),
       sizeof(struct kinfo_proc), sizeof(struct xtcpcb),
       sizeof(struct xinpgen), sizeof(struct xsocket));
  for(int i=0; i<l.n; i++) {
    const sock_t *s = &l.v[i];
    LOGD("%s %s:%d -> %s:%d %s pid %d fd %d %s %s",
         s->proto == SOCK_TCP ? "tcp" : "udp", s->laddr, s->lport,
         s->raddr[0] ? s->raddr : "*", s->rport, sock_state_name(s->state),
         (int)s->pid, s->fd, s->proc, s->tid);
  }
  socklist_free(&l);
}
