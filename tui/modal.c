/*
 * modal.c — confirmation modal, tool-permissions overlay, and help
 * overlay of the asngn TUI.
 *
 * The confirmation modal centers over a dimmed backdrop with a yellow
 * double-line border: tool.command in bold, pretty-printed args (max 8
 * lines), annotation badges, and the key line. Keys are resolved in
 * main.c via asngn_confirm. The help overlay lists the keymap and the
 * slash commands; both render through the same cell-buffer path so
 * frames can be golden-tested.
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <stdio.h>
#include <string.h>

/* Wraps s into rows of at most `width` cells; returns rows written. */
static int wrap_text(const char *s, int width, int max_rows,
                     const char **starts, int *lens) {
  size_t i = 0, len = strlen(s);
  int rows = 0;
  if (width < 4) width = 4;
  while (i < len && rows < max_rows) {
    size_t j = i, last_space = 0, cells = 0;
    int have_space = 0;
    while (j < len && s[j] != '\n' && cells < (size_t)width) {
      if (s[j] == ' ') {
        last_space = j;
        have_space = 1;
      }
      j = tui_u8_next(s, j);
      cells++;
    }
    if (j < len && s[j] != '\n' && have_space && last_space > i)
      j = last_space;
    starts[rows] = s + i;
    lens[rows] = (int)(j - i);
    rows++;
    if (j < len && (s[j] == '\n' || s[j] == ' ')) j++;
    i = j;
  }
  return rows;
}

static void put_clipped(tui_frame *f, int x, int y, const char *s,
                        int len, int maxcells, uint8_t fg, uint8_t attr) {
  char buf[256];
  if (len > (int)sizeof buf - 1) len = (int)sizeof buf - 1;
  if (len < 0) len = 0;
  memcpy(buf, s, (size_t)len);
  buf[len] = '\0';
  d_putn(f, x, y, buf, maxcells, fg, TBG_DEFAULT, attr);
}

void modal_draw_confirm(tui_app *a, tui_frame *f) {
  const tui_theme *th = &a->theme;
  const char *starts[8];
  int lens[8];
  int arg_rows;
  int bw = f->w - 8;
  int bh, bx, by, row;
  char title[96];

  if (bw > 58) bw = 58;
  if (bw < 24) bw = f->w - 2;
  if (bw < 20) return; /* hopeless geometry: keys still work */

  arg_rows = wrap_text(a->confirm.args, bw - 4, 8, starts, lens);
  bh = 6 + arg_rows + (a->confirm.destructive ? 1 : 0);
  if (bh > f->h - 2) bh = f->h - 2;
  bx = (f->w - bw) / 2;
  by = (f->h - bh) / 2;
  if (by < 0) by = 0;

  d_dim_rect(f, 0, 0, f->w, f->h);
  {
    int x1, y1;
    for (y1 = by; y1 < by + bh; y1++)
      for (x1 = bx; x1 < bx + bw; x1++)
        d_cell(f, x1, y1, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  d_box(f, bx, by, bw, bh, th, 1, TFG_ACCENT, 0);
  snprintf(title, sizeof title, " confirm %s ", th->star);
  d_put(f, bx + 2, by, title, TFG_ACCENT, TBG_DEFAULT, TA_BOLD);

  row = by + 2;
  {
    char head[136];
    snprintf(head, sizeof head, "%s.%s", a->confirm.tool,
             a->confirm.cmd);
    d_putn(f, bx + 2, row, head, bw - 4, TFG_BRIGHT, TBG_DEFAULT,
           TA_BOLD);
    row++;
  }
  if (a->confirm.destructive) {
    d_putn(f, bx + 2, row, "[destructive]", bw - 4, TFG_AMBER,
           TBG_DEFAULT, 0);
    row++;
  }
  {
    int i;
    for (i = 0; i < arg_rows && row < by + bh - 2; i++, row++)
      put_clipped(f, bx + 2, row, starts[i], lens[i], bw - 4, TFG_DIM,
                  0);
  }
  {
    char keys[96];
    snprintf(keys, sizeof keys,
             "y allow %s n deny %s a always this session %s Esc deny",
             th->bullet, th->bullet, th->bullet);
    d_putn(f, bx + 2, by + bh - 2, keys, bw - 4, TFG_DIM, TBG_DEFAULT,
           0);
  }
}

/* ── tool permissions overlay (/perms) ────────────────────────────────── */

void modal_draw_perms(tui_app *a, tui_frame *f) {
  const tui_theme *th = &a->theme;
  tui_perms *p = &a->perms;
  int bw = f->w - 8;
  int bh, bx, by, rows, i;
  char title[64];

  if (bw > 48) bw = 48;
  if (bw < 24) bw = f->w - 2;
  if (bw < 20) return; /* hopeless geometry: keys still work */

  rows = (int)p->n > 0 ? (int)p->n : 1; /* 1: the "no tools" line */
  bh = rows + 4;
  if (bh > f->h - 2) bh = f->h - 2;
  if (bh < 5) bh = 5;
  rows = bh - 4; /* visible list rows */
  bx = (f->w - bw) / 2;
  by = (f->h - bh) / 2;
  if (bx < 0) bx = 0;
  if (by < 0) by = 0;

  /* keep the cursor visible */
  if (p->cur < p->top) p->top = p->cur;
  if (p->cur >= p->top + rows) p->top = p->cur - rows + 1;
  if (p->top < 0) p->top = 0;

  d_dim_rect(f, 0, 0, f->w, f->h);
  {
    int x1, y1;
    for (y1 = by; y1 < by + bh; y1++)
      for (x1 = bx; x1 < bx + bw; x1++)
        d_cell(f, x1, y1, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  d_box(f, bx, by, bw, bh, th, 0, TFG_DIM, 0);
  snprintf(title, sizeof title, " tools %s ", th->star);
  d_put(f, bx + 2, by, title, TFG_ACCENT, TBG_DEFAULT, TA_BOLD);

  if (p->n == 0) {
    d_putn(f, bx + 2, by + 2, "no tools in the registry", bw - 4,
           TFG_DIM, TBG_DEFAULT, TA_DIM);
  }
  for (i = 0; i < rows && p->top + i < (int)p->n; i++) {
    const asngn_tool_info *t = &p->tools[p->top + i];
    int sel = p->top + i == p->cur;
    int y = by + 2 + i;
    int cx = bx + 2;
    uint8_t fg = t->available ? TFG_DEFAULT : TFG_DIM;
    cx += d_put(f, cx, y, sel ? th->marker : " ", TFG_ACCENT,
                TBG_DEFAULT, 0);
    cx += d_put(f, cx, y, " [", TFG_DIM, TBG_DEFAULT, 0);
    cx += d_put(f, cx, y, t->enabled ? th->check : " ",
                t->enabled ? TFG_ACCENT : TFG_DIM, TBG_DEFAULT, 0);
    cx += d_put(f, cx, y, "] ", TFG_DIM, TBG_DEFAULT, 0);
    cx += d_putn(f, cx, y, t->ref, bw - 4 - 6 - 14, fg, TBG_DEFAULT,
                 sel ? TA_BOLD : 0);
    if (!t->available)
      d_putn(f, bx + bw - 2 - 13, y, "(unavailable)", 13, TFG_AMBER,
             TBG_DEFAULT, 0);
  }
  {
    char keys[80];
    snprintf(keys, sizeof keys, "Space toggle %s Esc close", th->bullet);
    d_putn(f, bx + 2, by + bh - 2, keys, bw - 4, TFG_DIM, TBG_DEFAULT,
           TA_DIM);
  }
}

/* ── session picker overlay (/session) ────────────────────────────────── */

void modal_draw_sessions(tui_app *a, tui_frame *f) {
  const tui_theme *th = &a->theme;
  tui_sess *p = &a->sess;
  int bw = f->w - 8;
  int bh, bx, by, rows, i;
  char title[64];

  if (bw > 64) bw = 64;
  if (bw < 24) bw = f->w - 2;
  if (bw < 20) return; /* hopeless geometry: keys still work */

  rows = (int)p->n > 0 ? (int)p->n : 1; /* 1: the "no sessions" line */
  bh = rows + 4;
  if (bh > f->h - 2) bh = f->h - 2;
  if (bh < 5) bh = 5;
  rows = bh - 4; /* visible list rows */
  bx = (f->w - bw) / 2;
  by = (f->h - bh) / 2;
  if (bx < 0) bx = 0;
  if (by < 0) by = 0;

  /* keep the cursor visible */
  if (p->cur < p->top) p->top = p->cur;
  if (p->cur >= p->top + rows) p->top = p->cur - rows + 1;
  if (p->top < 0) p->top = 0;

  d_dim_rect(f, 0, 0, f->w, f->h);
  {
    int x1, y1;
    for (y1 = by; y1 < by + bh; y1++)
      for (x1 = bx; x1 < bx + bw; x1++)
        d_cell(f, x1, y1, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  d_box(f, bx, by, bw, bh, th, 0, TFG_DIM, 0);
  snprintf(title, sizeof title, " sessions %s ", th->star);
  d_put(f, bx + 2, by, title, TFG_ACCENT, TBG_DEFAULT, TA_BOLD);

  if (p->n == 0) {
    d_putn(f, bx + 2, by + 2, "no sessions yet", bw - 4, TFG_DIM,
           TBG_DEFAULT, TA_DIM);
  }
  for (i = 0; i < rows && p->top + i < (int)p->n; i++) {
    const tui_sess_row *r = &p->rows[p->top + i];
    int sel = p->top + i == p->cur;
    int y = by + 2 + i;
    int cx = bx + 2;
    char meta[192];
    cx += d_put(f, cx, y, sel ? th->marker : " ", TFG_ACCENT,
                TBG_DEFAULT, 0);
    cx += d_put(f, cx, y, " ", TFG_DIM, TBG_DEFAULT, 0);
    cx += d_putn(f, cx, y, r->slug, 20,
                 r->unreadable ? TFG_DIM : TFG_DEFAULT, TBG_DEFAULT,
                 sel ? TA_BOLD : 0);
    if (r->unreadable)
      snprintf(meta, sizeof meta, "  (unreadable)");
    else
      snprintf(meta, sizeof meta,
               "  %zu turns %.3s %.15s tok %.3s %.31s%s%.64s%s",
               r->turns, th->bullet, r->tok, th->bullet, r->age,
               r->project[0] != '\0' ? " [" : "",
               r->project[0] != '\0' ? r->project : "",
               r->project[0] != '\0' ? "]" : "");
    cx += d_putn(f, cx, y, meta, bx + bw - 2 - cx - 10, TFG_DIM,
                 TBG_DEFAULT, 0);
    if (r->current)
      d_putn(f, bx + bw - 2 - 9, y, "(current)", 9, TFG_ACCENT,
             TBG_DEFAULT, 0);
  }
  if (p->note[0] != '\0') {
    d_putn(f, bx + 2, by + bh - 2, p->note, bw - 4, TFG_AMBER,
           TBG_DEFAULT, 0);
  } else {
    char keys[96];
    snprintf(keys, sizeof keys,
             "Enter switch %s n new %s d d delete %s Esc close",
             th->bullet, th->bullet, th->bullet);
    d_putn(f, bx + 2, by + bh - 2, keys, bw - 4, TFG_DIM, TBG_DEFAULT,
           TA_DIM);
  }
}

/* ── help overlay (F1) ────────────────────────────────────────────────── */

static const char *const HELP_KEYS[] = {
    "Enter        send; Alt+Enter / Ctrl+J newline",
    "Esc          cancel the running turn / close",
    "Tab          cycle sidebar panes; complete /commands",
    "F1           this help",
    "F2..F6       trace / stats / memory / tools / cache",
    "F7 / F8      rate the last answer good / poor",
    "Up / Down    scroll chat one line",
    "PgUp / PgDn  scroll chat one page",
    "Alt+Up/Down  scroll prompt one line",
    "Alt+PgUp/Dn  scroll prompt one page",
    "Ctrl+P / N   previous / next input history",
    "Home / End   prompt beginning / end",
    "Ctrl+U/K/W/Y kill to start / end / word; yank",
    "Ctrl+D       quit (empty editor)",
};

static const char *const HELP_CMDS[] = {
    "/help /quit /more /retry /pin [n] /compact",
    "/session picker; /session <slug>|new|delete <slug>",
    "/project <slug>|none",
    "/detail terse|normal|rich|auto",
    "/cache stats|clear, /redact on|off",
    "/memory <question>, /export",
    "/perms enable/disable tools",
    "/tools /stats",
};

void modal_draw_help(tui_app *a, tui_frame *f) {
  const tui_theme *th = &a->theme;
  int nk = (int)(sizeof HELP_KEYS / sizeof HELP_KEYS[0]);
  int nc = (int)(sizeof HELP_CMDS / sizeof HELP_CMDS[0]);
  int bw = f->w - 8;
  int bh = nk + nc + 6;
  int bx, by, row, i;
  char title[64];

  if (bw > 56) bw = 56;
  if (bh > f->h) bh = f->h;
  bx = (f->w - bw) / 2;
  by = (f->h - bh) / 2;
  if (bx < 0) bx = 0;
  if (by < 0) by = 0;

  d_dim_rect(f, 0, 0, f->w, f->h);
  {
    int x1, y1;
    for (y1 = by; y1 < by + bh; y1++)
      for (x1 = bx; x1 < bx + bw; x1++)
        d_cell(f, x1, y1, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  d_box(f, bx, by, bw, bh, th, 0, TFG_DIM, 0);
  snprintf(title, sizeof title, " help %s ", th->star);
  d_put(f, bx + 2, by, title, TFG_ACCENT, TBG_DEFAULT, TA_BOLD);

  row = by + 2;
  for (i = 0; i < nk && row < by + bh - 2; i++, row++)
    d_putn(f, bx + 2, row, HELP_KEYS[i], bw - 4, TFG_DEFAULT,
           TBG_DEFAULT, 0);
  row++;
  for (i = 0; i < nc && row < by + bh - 2; i++, row++)
    d_putn(f, bx + 2, row, HELP_CMDS[i], bw - 4, TFG_DIM, TBG_DEFAULT,
           0);
  d_putn(f, bx + 2, by + bh - 2, "Esc closes", bw - 4, TFG_DIM,
         TBG_DEFAULT, TA_DIM);
}
