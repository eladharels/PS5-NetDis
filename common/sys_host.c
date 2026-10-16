// Host stand-in for sys_ps5.c, for tests and local demos. Records what was
// asked so tests can check it.

#include <stdio.h>
#include <string.h>

#include "sys.h"

int  sys_host_tile;
int  sys_host_tile_installs;
int  sys_host_tile_port;
char sys_host_last_notify[256];


void
sys_notify(const char *msg) {
  snprintf(sys_host_last_notify, sizeof(sys_host_last_notify), "%s", msg);
}


void
sys_set_process_name(const char *name) {
  (void)name;
}


int
sys_tile_install(const sys_tile_t *tile) {
  sys_host_tile = 1;
  sys_host_tile_port = tile->port;
  sys_host_tile_installs++;
  return 0;
}


int
sys_tile_remove(const char *tid) {
  (void)tid;
  sys_host_tile = 0;
  return 0;
}


int
sys_tile_installed(const char *tid) {
  (void)tid;
  return sys_host_tile;
}
