#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"


void
sb_init(strbuf_t *sb) {
  memset(sb, 0, sizeof(*sb));
}


void
sb_free(strbuf_t *sb) {
  free(sb->data);
  sb_init(sb);
}


static int
sb_reserve(strbuf_t *sb, size_t extra) {
  size_t need = sb->len + extra + 1;
  size_t cap;
  char *p;

  if(sb->oom) {
    return -1;
  }
  if(need <= sb->cap) {
    return 0;
  }
  cap = sb->cap ? sb->cap : 256;
  while(cap < need) {
    cap *= 2;
  }
  if(!(p=realloc(sb->data, cap))) {
    sb->oom = 1;
    return -1;
  }
  sb->data = p;
  sb->cap = cap;
  return 0;
}


void
sb_append(strbuf_t *sb, const char *s, size_t n) {
  if(sb_reserve(sb, n)) {
    return;
  }
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = 0;
}


void
sb_puts(strbuf_t *sb, const char *s) {
  sb_append(sb, s, strlen(s));
}


void
sb_printf(strbuf_t *sb, const char *fmt, ...) {
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if(n < 0 || sb_reserve(sb, n)) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(sb->data + sb->len, n + 1, fmt, ap);
  va_end(ap);
  sb->len += n;
}


void
sb_json_str(strbuf_t *sb, const char *s) {
  char esc[8];

  sb_puts(sb, "\"");
  for(; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if(c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = c;
      sb_append(sb, esc, 2);
    } else if(c < 0x20) {
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      sb_puts(sb, esc);
    } else {
      sb_append(sb, (const char*)&c, 1);
    }
  }
  sb_puts(sb, "\"");
}


static int
hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}


long
hex_decode(const char *hex, uint8_t *out, size_t max) {
  size_t n = 0;

  while(hex[0] && hex[1]) {
    int hi = hexval(hex[0]);
    int lo = hexval(hex[1]);
    if(hi < 0 || lo < 0 || n >= max) {
      return -1;
    }
    out[n++] = (uint8_t)(hi << 4 | lo);
    hex += 2;
  }
  return hex[0] ? -1 : (long)n;
}


char *
read_file(const char *path, size_t *len) {
  FILE *f;
  char *buf = NULL;
  long size;

  if(!(f=fopen(path, "rb"))) {
    return NULL;
  }
  if(fseek(f, 0, SEEK_END) || (size=ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
    goto out;
  }
  if(!(buf=malloc(size + 1))) {
    goto out;
  }
  if(fread(buf, 1, size, f) != (size_t)size) {
    free(buf);
    buf = NULL;
    goto out;
  }
  buf[size] = 0;
  if(len) {
    *len = size;
  }

 out:
  fclose(f);
  return buf;
}


int
is_title_id(const char *s) {
  for(int i=0; i<4; i++) {
    if(!isupper((unsigned char)s[i])) {
      return 0;
    }
  }
  for(int i=4; i<9; i++) {
    if(!isdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return s[9] == 0;
}
