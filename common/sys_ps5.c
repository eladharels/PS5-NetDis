#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "log.h"
#include "sys.h"
#include "util.h"


int sceNotificationSend(int userId, bool isLogged, const char *payload);
int sceKernelSetProcessName(const char *name);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *arg);
int sceAppInstUtilAppUnInstall(const char *title_id);

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE


void
sys_set_process_name(const char *name) {
  if(sceKernelSetProcessName(name)) {
    LOGW("could not rename the process to %s", name);
  }
}


void
sys_notify(const char *msg) {
  strbuf_t sb;

  LOGI("notification: %s", msg);

  sb_init(&sb);
  sb_puts(&sb, "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
	  "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
	  "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
	  "\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":"
	  "{\"icon\":\"download\"}},\"message\":{\"body\":");
  sb_json_str(&sb, msg);
  sb_puts(&sb, "}}},\"createdDateTime\":\"2026-01-01T00:00:00.000Z\","
	  "\"localNotificationId\":\"60600060\"}");
  if(!sb.oom &&
     sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true, sb.data)) {
    LOGW("could not show the notification on screen");
  }
  sb_free(&sb);
}


// ---- home screen tile ------------------------------------------------------
//
// A tile is an app folder in /user/app with only sce_sys/param.json and
// sce_sys/icon0.png. A media-category app (applicationCategoryType 65536)
// with a deeplinkUri opens that URL in the browser when started. This is
// the approach John Törnblom's ftpsrv introduced and Payload Manager uses.

// NID of sceAppInstUtilAppInstallTitleDir, which registers one app folder.
#define NID_INSTALL_TITLE_DIR "Wudg3Xe3heE"


static int
file_matches(const char *path, const void *data, size_t len) {
  size_t got;
  char *buf = read_file(path, &got);
  int same = buf && got == len && !memcmp(buf, data, len);

  free(buf);
  return same;
}


static int
write_whole(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");

  if(!f) {
    return -1;
  }
  if(fwrite(data, 1, len, f) != len) {
    fclose(f);
    return -1;
  }
  return fclose(f);
}


static int
register_tile(const char *tid) {
  int (*install_dir)(const char *, const char *, void *) = NULL;
  uint32_t handle;

  // Not exported on every firmware, so look it up instead of linking it.
  if(!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    install_dir = (void*)kernel_dynlib_resolve(-1, handle,
                                               NID_INSTALL_TITLE_DIR);
  }
  if(install_dir) {
    return install_dir(tid, "/user/app/", NULL);
  }
  LOGD("tile: InstallTitleDir not found, using InstallAll");
  return sceAppInstUtilAppInstallAll(NULL);
}


static void
tile_paths(const char *tid, char *dir, char *sys, char *param, char *icon,
           size_t len) {
  snprintf(dir, len, "/user/app/%s", tid);
  snprintf(sys, len, "/user/app/%s/sce_sys", tid);
  snprintf(param, len, "/user/app/%s/sce_sys/param.json", tid);
  snprintf(icon, len, "/user/app/%s/sce_sys/icon0.png", tid);
}


int
sys_tile_installed(const char *tid) {
  char path[128];
  struct stat st;

  snprintf(path, sizeof(path), "/user/app/%s/sce_sys/param.json", tid);
  return !stat(path, &st);
}


int
sys_tile_install(const sys_tile_t *tile) {
  char dir[128], sys[128], param_path[128], icon_path[128];
  char param[512];
  int len;
  int err;

  tile_paths(tile->tid, dir, sys, param_path, icon_path, sizeof(dir));
  len = snprintf(param, sizeof(param),
                 "{\n"
                 "  \"titleId\": \"%s\",\n"
                 "  \"applicationCategoryType\": 65536,\n"
                 "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
                 "  \"localizedParameters\": {\n"
                 "    \"defaultLanguage\": \"en-US\",\n"
                 "    \"en-US\": { \"titleName\": \"%s\" }\n"
                 "  }\n"
                 "}\n", tile->tid, tile->port, tile->title);

  if(file_matches(param_path, param, len) &&
     file_matches(icon_path, tile->icon, tile->icon_size)) {
    LOGD("tile: %s is installed and current", tile->tid);
    return 0;
  }
  LOGI("tile: %s the home screen tile %s",
       sys_tile_installed(tile->tid) ? "updating" : "installing", tile->tid);

  if((err=sceAppInstUtilInitialize())) {
    LOGE("tile: sceAppInstUtilInitialize failed: 0x%08x", err);
    return -1;
  }
  mkdir(dir, 0755);
  mkdir(sys, 0755);
  if(write_whole(param_path, param, len) ||
     write_whole(icon_path, tile->icon, tile->icon_size)) {
    LOGE("tile: could not write %s: %s", sys, strerror(errno));
    sceAppInstUtilTerminate();
    return -1;
  }
  if((err=register_tile(tile->tid))) {
    LOGE("tile: install failed: 0x%08x", err);
    // Without param.json the next start retries, and the page doesn't
    // claim a tile that the home screen doesn't have.
    unlink(param_path);
    sceAppInstUtilTerminate();
    return -1;
  }
  sceAppInstUtilTerminate();
  LOGI("tile: installed; it opens http://127.0.0.1:%d/", tile->port);
  return 0;
}


int
sys_tile_remove(const char *tid) {
  int err;

  if((err=sceAppInstUtilInitialize())) {
    LOGE("tile: sceAppInstUtilInitialize failed: 0x%08x", err);
    return -1;
  }
  err = sceAppInstUtilAppUnInstall(tid);
  sceAppInstUtilTerminate();
  if(err) {
    LOGE("tile: uninstall failed: 0x%08x", err);
    return -1;
  }
  LOGI("tile: removed %s", tid);
  return 0;
}
