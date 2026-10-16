#pragma once

#include <stddef.h>
#include <stdint.h>

// Growable string buffer. On allocation failure it stops appending and sets
// `oom`, so callers can build a whole response and check once at the end.
typedef struct strbuf {
  char  *data;
  size_t len;
  size_t cap;
  int    oom;
} strbuf_t;

void sb_init(strbuf_t *sb);
void sb_free(strbuf_t *sb);
void sb_append(strbuf_t *sb, const char *s, size_t n);
void sb_puts(strbuf_t *sb, const char *s);
void sb_printf(strbuf_t *sb, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

// Appends `s` as a quoted JSON string.
void sb_json_str(strbuf_t *sb, const char *s);

// Decodes hex digits into `out`. Returns the byte count, or -1 on bad input.
long hex_decode(const char *hex, uint8_t *out, size_t max);

// Reads a whole file into a NUL-terminated heap buffer.
char *read_file(const char *path, size_t *len);

// Title IDs look like CUSA12345 or PPSA01234.
int is_title_id(const char *s);
