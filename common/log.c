#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"


#define LOG_CAP     1000
#define LOG_MSG     240
// Keeps the log file from filling the console's storage.
#define LOG_FILE_MAX (1024 * 1024)


typedef struct entry {
  unsigned long seq;
  time_t        when;
  int           level;
  char          msg[LOG_MSG];
} entry_t;


static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static entry_t g_ring[LOG_CAP];
static unsigned long g_next = 1;
static FILE *g_file;
static long g_file_size;

static char g_name[32] = "payload";

static const char *const LEVELS[] = {"debug", "info", "warn", "error"};


void
log_set_name(const char *name) {
  pthread_mutex_lock(&g_lock);
  snprintf(g_name, sizeof(g_name), "%s", name);
  pthread_mutex_unlock(&g_lock);
}


void
log_open(const char *path) {
  char old[512];

  snprintf(old, sizeof(old), "%s.1", path);
  pthread_mutex_lock(&g_lock);
  if(g_file) {
    fclose(g_file);
  }
  rename(path, old);
  g_file = fopen(path, "w");
  g_file_size = 0;
  pthread_mutex_unlock(&g_lock);
}


static void
format_line(char *out, size_t len, const entry_t *e) {
  struct tm tm;
  char ts[16];

  localtime_r(&e->when, &tm);
  strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
  snprintf(out, len, "%s %-5s %s", ts, LEVELS[e->level], e->msg);
}


void
log_msg(int level, const char *fmt, ...) {
  entry_t *e;
  char line[LOG_MSG + 32];
  va_list ap;

  if(level < LOG_DEBUG || level > LOG_ERROR) {
    level = LOG_INFO;
  }

  pthread_mutex_lock(&g_lock);
  e = &g_ring[g_next % LOG_CAP];
  e->seq = g_next++;
  e->when = time(NULL);
  e->level = level;
  va_start(ap, fmt);
  vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
  va_end(ap);
  // One entry per line keeps the text views aligned.
  for(char *p=e->msg; *p; p++) {
    if(*p == '\n' || *p == '\r') {
      *p = ' ';
    }
  }

  format_line(line, sizeof(line), e);
  printf("[%s] %s\n", g_name, line);
  fflush(stdout);
  if(g_file && g_file_size < LOG_FILE_MAX) {
    int n = fprintf(g_file, "%s\n", line);
    if(n > 0) {
      g_file_size += n;
    }
    if(g_file_size >= LOG_FILE_MAX) {
      fputs("log file full; later entries only on the Logs tab\n", g_file);
    }
    fflush(g_file);
  }
  pthread_mutex_unlock(&g_lock);
}


void
log_json(strbuf_t *sb, unsigned long since) {
  unsigned long oldest;
  int first = 1;

  pthread_mutex_lock(&g_lock);
  oldest = g_next > LOG_CAP ? g_next - LOG_CAP : 1;
  // The pid tells the page when a new copy replaced the one it was reading.
  sb_printf(sb, "{\"pid\":%d,\"next\":%lu,\"dropped\":%s,\"entries\":[",
            (int)getpid(), g_next - 1, since + 1 < oldest ? "true" : "false");
  for(unsigned long s=(since + 1 > oldest ? since + 1 : oldest); s<g_next; s++) {
    const entry_t *e = &g_ring[s % LOG_CAP];
    sb_printf(sb, "%s{\"seq\":%lu,\"time\":%lld,\"level\":\"%s\",\"msg\":",
              first ? "" : ",", e->seq, (long long)e->when, LEVELS[e->level]);
    sb_json_str(sb, e->msg);
    sb_puts(sb, "}");
    first = 0;
  }
  sb_puts(sb, "]}");
  pthread_mutex_unlock(&g_lock);
}


void
log_text(strbuf_t *sb) {
  unsigned long oldest;
  char line[LOG_MSG + 32];

  pthread_mutex_lock(&g_lock);
  oldest = g_next > LOG_CAP ? g_next - LOG_CAP : 1;
  for(unsigned long s=oldest; s<g_next; s++) {
    format_line(line, sizeof(line), &g_ring[s % LOG_CAP]);
    sb_puts(sb, line);
    sb_puts(sb, "\n");
  }
  pthread_mutex_unlock(&g_lock);
}
