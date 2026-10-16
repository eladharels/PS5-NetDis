#pragma once

#include "util.h"

// Diagnostic log shared by all threads. Each line goes to stdout (streamed
// back by the loader), to an in-memory ring shown on the Logs tab, and to a
// file on the console when one is open.

enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR,
};

// Prefix for lines on stdout, e.g. "fps60".
void log_set_name(const char *name);

// Opens `path` for appending, keeping the previous run as `path`.1.
void log_open(const char *path);

void log_msg(int level, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)

// Entries newer than sequence number `since`, as JSON:
// {"next":N,"dropped":bool,"entries":[{"seq","time","level","msg"}]}.
// "dropped" is true when entries after `since` fell out of the ring.
void log_json(strbuf_t *sb, unsigned long since);

// Every entry still in the ring as plain text, one per line.
void log_text(strbuf_t *sb);
