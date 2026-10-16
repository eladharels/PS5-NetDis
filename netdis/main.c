#include "log.h"
#include "platform.h"
#include "routes.h"
#include "service.h"


#ifndef APP_VERSION
#define APP_VERSION "dev"  // set from VERSION by the Makefile
#endif

#ifndef NETDIS_DIR
#define NETDIS_DIR "/data/netdis"
#endif


int
main(void) {
  service_info_t info = {
    .name = "NetDis",
    .version = APP_VERSION,
    .proc_name = "NetDis.elf",
    .data_dir = NETDIS_DIR,
    .port = 8070,
    .tile_tid = "NETD00001",
    .icon = netdis_icon_png,
    .icon_size = netdis_icon_png_size,
  };

  if(service_start(&info)) {
    return -1;
  }
  // One collection at startup puts the state source in the log.
  net_diagnose();
  service_announce();
  service_run(netdis_handle);
  return 0;
}
