/*
 * draw.c — cell-buffer drawing primitives of the asngn TUI.
 *
 * Every primitive clips against the frame, so callers can lay out for
 * the ideal geometry and degrade gracefully below 80×24. One
 * codepoint per cell; the glyph set guarantees single-width glyphs.
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── UTF-8 helpers ────────────────────────────────────────────────────── */

static int u8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t tui_u8_next(const char *s, size_t i) {
  if (s[i] == '\0') return i;
  i++;
  while (u8_cont((unsigned char)s[i])) i++;
  return i;
}

size_t tui_u8_prev(const char *s, size_t i) {
  if (i == 0) return 0;
  i--;
  while (i > 0 && u8_cont((unsigned char)s[i])) i--;
  return i;
}

int tui_u8_count(const char *s, size_t len) {
  size_t i = 0;
  int n = 0;
  while (i < len && s[i] != '\0') {
    if (!u8_cont((unsigned char)s[i])) n++;
    i++;
  }
  return n;
}

void tui_u8_char(char dst[5], const char *s) {
  size_t l = 1;
  unsigned char c = (unsigned char)s[0];
  if (c == '\0') {
    dst[0] = '\0';
    return;
  }
  if ((c & 0xE0) == 0xC0) l = 2;
  else if ((c & 0xF0) == 0xE0) l = 3;
  else if ((c & 0xF8) == 0xF0) l = 4;
  {
    size_t i;
    for (i = 1; i < l; i++)
      if (!u8_cont((unsigned char)s[i])) { l = i; break; }
  }
  memcpy(dst, s, l);
  dst[l] = '\0';
}

/* ── primitives ───────────────────────────────────────────────────────── */

void d_clear(tui_frame *f) {
  int i, n = f->w * f->h;
  for (i = 0; i < n; i++) {
    f->cells[i].utf8[0] = '\0';
    f->cells[i].fg = TFG_DEFAULT;
    f->cells[i].bg = TBG_DEFAULT;
    f->cells[i].attr = 0;
  }
}

void d_cell(tui_frame *f, int x, int y, const char *glyph, uint8_t fg,
            uint8_t bg, uint8_t attr) {
  tui_cell *c;
  if (x < 0 || y < 0 || x >= f->w || y >= f->h) return;
  c = &f->cells[y * f->w + x];
  tui_u8_char(c->utf8, glyph);
  c->fg = fg;
  c->bg = bg;
  c->attr = attr;
}

int d_putn(tui_frame *f, int x, int y, const char *s, int maxcells,
           uint8_t fg, uint8_t bg, uint8_t attr) {
  size_t i = 0;
  int n = 0;
  if (s == NULL || y < 0 || y >= f->h) return 0;
  while (s[i] != '\0' && n < maxcells) {
    size_t next = tui_u8_next(s, i);
    if (s[i] == '\n') { i = next; continue; } /* never draw newlines */
    if (x + n >= 0 && x + n < f->w) {
      tui_cell *c = &f->cells[y * f->w + (x + n)];
      size_t l = next - i;
      if (l > 4) l = 4;
      memcpy(c->utf8, s + i, l);
      c->utf8[l] = '\0';
      c->fg = fg;
      c->bg = bg;
      c->attr = attr;
    }
    n++;
    i = next;
  }
  return n;
}

int d_put(tui_frame *f, int x, int y, const char *s, uint8_t fg,
          uint8_t bg, uint8_t attr) {
  return d_putn(f, x, y, s, f->w, fg, bg, attr);
}

void d_hline(tui_frame *f, int x, int y, int w, const char *glyph,
             uint8_t fg, uint8_t attr) {
  int i;
  for (i = 0; i < w; i++) d_cell(f, x + i, y, glyph, fg, TBG_DEFAULT, attr);
}

void d_box(tui_frame *f, int x, int y, int w, int h, const tui_theme *th,
           int dbl, uint8_t fg, uint8_t attr) {
  const char *tl = dbl ? th->dtl : th->tl;
  const char *tr = dbl ? th->dtr : th->tr;
  const char *bl = dbl ? th->dbl : th->bl;
  const char *br = dbl ? th->dbr : th->br;
  const char *hg = dbl ? th->dh : th->hline;
  const char *vg = dbl ? th->dv : th->vline;
  int i;
  if (w < 2 || h < 2) return;
  d_cell(f, x, y, tl, fg, TBG_DEFAULT, attr);
  d_cell(f, x + w - 1, y, tr, fg, TBG_DEFAULT, attr);
  d_cell(f, x, y + h - 1, bl, fg, TBG_DEFAULT, attr);
  d_cell(f, x + w - 1, y + h - 1, br, fg, TBG_DEFAULT, attr);
  d_hline(f, x + 1, y, w - 2, hg, fg, attr);
  d_hline(f, x + 1, y + h - 1, w - 2, hg, fg, attr);
  for (i = 1; i < h - 1; i++) {
    d_cell(f, x, y + i, vg, fg, TBG_DEFAULT, attr);
    d_cell(f, x + w - 1, y + i, vg, fg, TBG_DEFAULT, attr);
  }
}

void d_dim_rect(tui_frame *f, int x, int y, int w, int h) {
  int i, j;
  for (j = y; j < y + h; j++) {
    if (j < 0 || j >= f->h) continue;
    for (i = x; i < x + w; i++) {
      tui_cell *c;
      if (i < 0 || i >= f->w) continue;
      c = &f->cells[j * f->w + i];
      c->fg = TFG_DIM;
      c->bg = TBG_DEFAULT;
      c->attr = TA_DIM;
    }
  }
}

/* ── small formatters ─────────────────────────────────────────────────── */

void tui_fmt_count(char *buf, size_t n, long long v) {
  if (v < 0) v = 0;
  if (v < 10000) snprintf(buf, n, "%lld", v);
  else if (v < 1000000)
    snprintf(buf, n, "%.1fk", (double)v / 1000.0);
  else
    snprintf(buf, n, "%.1fM", (double)v / 1000000.0);
}

void tui_fmt_ms(char *buf, size_t n, long long ms) {
  if (ms < 0) ms = 0;
  if (ms < 1000) snprintf(buf, n, "%lldms", ms);
  else if (ms < 60000)
    snprintf(buf, n, "%.1fs", (double)ms / 1000.0);
  else
    snprintf(buf, n, "%.1fm", (double)ms / 60000.0);
}

void tui_fmt_ago(char *buf, size_t n, long long unix_s) {
  long long now, d;
  if (unix_s <= 0) {
    snprintf(buf, n, "never");
    return;
  }
  now = (long long)time(NULL);
  d = now - unix_s;
  if (d < 0) d = 0;
  if (d < 60) snprintf(buf, n, "%llds ago", d);
  else if (d < 3600)
    snprintf(buf, n, "%lldm ago", d / 60);
  else if (d < 86400)
    snprintf(buf, n, "%lldh ago", d / 3600);
  else
    snprintf(buf, n, "%lldd ago", d / 86400);
}
