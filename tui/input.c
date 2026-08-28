/*
 * input.c — the asngn TUI input editor.
 *
 * A byte-buffer editor with a UTF-8-aware cursor: history (Ctrl+P/N),
 * kill/yank (Ctrl+U/K/W/Y), and multi-row layout for
 * newlines inserted with Alt+Enter / Ctrl+J. Slash-command and slug
 * completion live in main.c (they need the engine lists).
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <stdlib.h>
#include <string.h>

static void ed_view_follow(tui_editor *e) { e->view_manual = 0; }

void ed_init(tui_editor *e) {
  memset(e, 0, sizeof *e);
  e->hist_pos = -1;
}

void ed_free(tui_editor *e) {
  size_t i;
  for (i = 0; i < e->hist_n; i++) free(e->hist[i]);
  memset(e, 0, sizeof *e);
  e->hist_pos = -1;
}

void ed_clear(tui_editor *e) {
  e->len = e->cur = 0;
  e->buf[0] = '\0';
  e->hist_pos = -1;
  e->view_top = 0;
  ed_view_follow(e);
}

void ed_set(tui_editor *e, const char *s) {
  size_t n = strlen(s);
  if (n > TUI_ED_MAX - 1) n = TUI_ED_MAX - 1;
  memcpy(e->buf, s, n);
  e->len = e->cur = n;
  e->buf[n] = '\0';
  ed_view_follow(e);
}

int ed_insert(tui_editor *e, const char *s, size_t n) {
  if (n == 0) return 0;
  if (e->len + n > TUI_ED_MAX - 1) return -1;
  memmove(e->buf + e->cur + n, e->buf + e->cur, e->len - e->cur);
  memcpy(e->buf + e->cur, s, n);
  e->len += n;
  e->cur += n;
  e->buf[e->len] = '\0';
  e->hist_pos = -1; /* editing detaches from history browsing */
  ed_view_follow(e);
  return 0;
}

void ed_backspace(tui_editor *e) {
  size_t prev;
  if (e->cur == 0) return;
  prev = tui_u8_prev(e->buf, e->cur);
  memmove(e->buf + prev, e->buf + e->cur, e->len - e->cur);
  e->len -= e->cur - prev;
  e->cur = prev;
  e->buf[e->len] = '\0';
  e->hist_pos = -1;
  ed_view_follow(e);
}

void ed_delete(tui_editor *e) {
  size_t next;
  if (e->cur >= e->len) return;
  next = tui_u8_next(e->buf, e->cur);
  memmove(e->buf + e->cur, e->buf + next, e->len - next);
  e->len -= next - e->cur;
  e->buf[e->len] = '\0';
  e->hist_pos = -1;
  ed_view_follow(e);
}

void ed_left(tui_editor *e) {
  if (e->cur > 0) e->cur = tui_u8_prev(e->buf, e->cur);
  ed_view_follow(e);
}

void ed_right(tui_editor *e) {
  if (e->cur < e->len) e->cur = tui_u8_next(e->buf, e->cur);
  ed_view_follow(e);
}

void ed_home(tui_editor *e) { e->cur = 0; ed_view_follow(e); }
void ed_end(tui_editor *e) { e->cur = e->len; ed_view_follow(e); }

static void ed_kill_range(tui_editor *e, size_t a, size_t b) {
  if (b <= a) return;
  if (b - a > sizeof e->kill - 1) b = a + sizeof e->kill - 1;
  memcpy(e->kill, e->buf + a, b - a);
  e->kill_len = b - a;
  e->kill[e->kill_len] = '\0';
  memmove(e->buf + a, e->buf + b, e->len - b);
  e->len -= b - a;
  e->cur = a;
  e->buf[e->len] = '\0';
  e->hist_pos = -1;
  ed_view_follow(e);
}

void ed_kill_to_start(tui_editor *e) { ed_kill_range(e, 0, e->cur); }
void ed_kill_to_end(tui_editor *e) { ed_kill_range(e, e->cur, e->len); }

void ed_kill_word(tui_editor *e) {
  size_t a = e->cur;
  while (a > 0 && e->buf[a - 1] == ' ') a--;
  while (a > 0 && e->buf[a - 1] != ' ' && e->buf[a - 1] != '\n') a--;
  ed_kill_range(e, a, e->cur);
}

void ed_yank(tui_editor *e) {
  if (e->kill_len > 0) {
    int hp = e->hist_pos;
    ed_insert(e, e->kill, e->kill_len);
    e->hist_pos = hp; /* yank should not count as a history detach */
  }
}

/* ── history ──────────────────────────────────────────────────────────── */

void ed_hist_add(tui_editor *e, const char *line) {
  char *dup;
  if (line == NULL || line[0] == '\0') return;
  if (e->hist_n > 0 && strcmp(e->hist[e->hist_n - 1], line) == 0) return;
  dup = malloc(strlen(line) + 1);
  if (dup == NULL) return;
  strcpy(dup, line);
  if (e->hist_n == TUI_HIST_MAX) {
    free(e->hist[0]);
    memmove(e->hist, e->hist + 1, (TUI_HIST_MAX - 1) * sizeof e->hist[0]);
    e->hist_n--;
  }
  e->hist[e->hist_n++] = dup;
}

void ed_hist_up(tui_editor *e) {
  if (e->hist_n == 0) return;
  if (e->hist_pos < 0) {
    if (e->len != 0) return; /* history only from an empty editor */
    memcpy(e->saved, e->buf, e->len);
    e->saved_len = e->len;
    e->hist_pos = (int)e->hist_n - 1;
  } else if (e->hist_pos > 0) {
    e->hist_pos--;
  }
  {
    int hp = e->hist_pos;
    ed_set(e, e->hist[hp]);
    e->hist_pos = hp; /* ed_set leaves cur at end; keep browsing state */
  }
}

void ed_hist_down(tui_editor *e) {
  if (e->hist_pos < 0) return;
  if (e->hist_pos < (int)e->hist_n - 1) {
    int hp = e->hist_pos + 1;
    ed_set(e, e->hist[hp]);
    e->hist_pos = hp;
  } else {
    memcpy(e->buf, e->saved, e->saved_len);
    e->len = e->cur = e->saved_len;
    e->buf[e->len] = '\0';
    e->hist_pos = -1;
    ed_view_follow(e);
  }
}

void ed_view_scroll(tui_editor *e, int rows) {
  int max = e->view_total - e->view_rows;
  if (max < 0) max = 0;
  e->view_top += rows;
  if (e->view_top < 0) e->view_top = 0;
  if (e->view_top > max) e->view_top = max;
  e->view_manual = max > 0;
}

/* ── layout ───────────────────────────────────────────────────────────── */

int ed_layout(const tui_editor *e, int width, ed_row *rows, int max_rows,
              int *crow, int *ccol) {
  size_t i = 0;
  int col = 0;
  size_t row_start = 0;
  int total = 0;
  if (width < 1) width = 1;
  *crow = 0;
  *ccol = 0;

  while (i <= e->len) {
    int at_cursor = (i == e->cur);
    if (at_cursor) {
      *crow = total;
      *ccol = col;
    }
    if (i == e->len) break;
    if (e->buf[i] == '\n') {
      if (total < max_rows) {
        rows[total].start = row_start;
        rows[total].end = i;
      }
      total++;
      i++;
      row_start = i;
      col = 0;
      continue;
    }
    if (col >= width) {
      if (total < max_rows) {
        rows[total].start = row_start;
        rows[total].end = i;
      }
      total++;
      row_start = i;
      col = 0;
      if (i == e->cur) {
        *crow = total;
        *ccol = 0;
      }
    }
    i = tui_u8_next(e->buf, i);
    col++;
  }
  if (total < max_rows) {
    rows[total].start = row_start;
    rows[total].end = e->len;
  }
  total++;
  return total;
}
