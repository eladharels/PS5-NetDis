#pragma once

// Console services shared by the payloads. sys_ps5.c implements them on the
// PS5; sys_host.c is a stand-in for host builds and tests.

#include <stddef.h>

// A home screen tile: an app folder in /user/app with only param.json and
// icon0.png. Its deep link opens the payload's web page in the console's
// browser.
typedef struct sys_tile {
  const char          *tid;    // e.g. "FPSU00060"
  const char          *title;  // name under the tile
  int                  port;   // web page port on 127.0.0.1
  const unsigned char *icon;   // PNG, 512x512
  unsigned long        icon_size;
} sys_tile_t;

// Shows a toast on the console.
void sys_notify(const char *msg);

// Names this process, as shown by process lists such as Payload Manager's.
void sys_set_process_name(const char *name);

// Installs the tile, or updates its files when they changed. Returns 0 when
// the tile is installed and current.
int sys_tile_install(const sys_tile_t *tile);
int sys_tile_remove(const char *tid);
int sys_tile_installed(const char *tid);
