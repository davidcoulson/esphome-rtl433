// Replaces src/term_ctl.c: no TTY on the ESP32, so no colours, no bell, fixed width.
#include <stdarg.h>
#include <stdio.h>

#include "term_ctl.h"

void *term_init(FILE *fp) { return fp; }
void term_free(void *ctx) { (void) ctx; }
int term_get_columns(void *ctx) { (void) ctx; return 80; }
int term_has_color(void *ctx) { (void) ctx; return 0; }
void term_ring_bell(void *ctx) { (void) ctx; }
void term_set_fg(void *ctx, term_color_t color) { (void) ctx; (void) color; }
void term_set_bg(void *ctx, term_color_t bg, term_color_t fg) { (void) ctx; (void) bg; (void) fg; }
int term_set_color_map(int idx, term_color_t color) { (void) idx; (void) color; return 0; }
int term_get_color_map(int idx) { (void) idx; return 0; }

int term_printf(void *ctx, char const *format, ...) {
  va_list args;
  va_start(args, format);
  int len = vfprintf(ctx ? (FILE *) ctx : stdout, format, args);
  va_end(args);
  return len;
}

int term_help_fprintf(FILE *fp, char const *format, ...) {
  va_list args;
  va_start(args, format);
  int len = vfprintf(fp, format, args);
  va_end(args);
  return len;
}

int term_puts(void *ctx, const char *buf) { return fputs(buf, ctx ? (FILE *) ctx : stdout); }
int term_help_fputs(void *ctx, const char *buf, FILE *fp) { (void) ctx; return fputs(buf, fp); }
