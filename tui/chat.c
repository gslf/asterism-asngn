/*
 * chat.c — chat pane of the asngn TUI with markdown-lite rendering.
 *
 * Entries wrap at the pane width. Markdown-lite: ``` fences
 * render dim on a shaded background, "- " bullets get a yellow "·",
 * *emphasis* renders bold and `code` spans dim — parsed minimally with
 * per-source-line state, so unbalanced markers can never crash or leak
 * styling across lines. Badges ([capped · /more], [clarify], …) render
 * dim-yellow at the tail of an entry.
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* prefix widths: "you  › " / "asngn ✦ " / "· " */
#define PFX_USER 7
#define PFX_ASSIST 8
#define PFX_SYSTEM 2

void chat_init(tui_chat *c) { memset(c, 0, sizeof *c); }

void chat_free(tui_chat *c) {
  size_t i;
  for (i = 0; i < c->n; i++) free(c->e[i].text);
  free(c->e);
  memset(c, 0, sizeof *c);
}

chat_entry *chat_push(tui_chat *c, chat_role role) {
  if (c->n == c->cap) {
    size_t cap = c->cap != 0 ? c->cap * 2 : 16;
    chat_entry *e = realloc(c->e, cap * sizeof *e);
    if (e == NULL) return NULL;
    c->e = e;
    c->cap = cap;
  }
  {
    chat_entry *e = &c->e[c->n++];
    memset(e, 0, sizeof *e);
    e->role = role;
    return e;
  }
}

int chat_append(tui_chat *c, chat_entry *e, const char *s, size_t n) {
  if (s == NULL || n == 0) return 0;
  if (e->len + n + 1 > e->cap) {
    size_t cap = e->cap != 0 ? e->cap : 256;
    char *p;
    while (cap < e->len + n + 1) cap *= 2;
    p = realloc(e->text, cap);
    if (p == NULL) return -1;
    e->text = p;
    e->cap = cap;
  }
  if (e->badge_off == e->len) e->badge_off += n; /* no badge yet: track */
  memcpy(e->text + e->len, s, n);
  e->len += n;
  e->text[e->len] = '\0';
  if (c->scroll > 0) c->unseen = 1;
  return 0;
}

void chat_badge(tui_chat *c, chat_entry *e, const char *badge) {
  size_t off = e->len;
  int had = e->badge_off < e->len; /* keep the first badge's start */
  if (chat_append(c, e, badge, strlen(badge)) == 0 && !had)
    e->badge_off = off;
}

void chat_systemf(tui_chat *c, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  chat_entry *e = chat_push(c, CHAT_SYSTEM);
  if (e == NULL) return;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  chat_append(c, e, buf, strlen(buf));
}

void chat_scroll_by(tui_chat *c, int lines) {
  int max = c->last_total - c->last_h;
  if (max < 0) max = 0;
  c->scroll += lines;
  if (c->scroll > max) c->scroll = max;
  if (c->scroll <= 0) {
    c->scroll = 0;
    c->unseen = 0;
  }
}

/* ── layout ───────────────────────────────────────────────────────────── */

typedef struct {
  int     entry;
  size_t  start, end;
  uint8_t first;  /* first display line of the entry: draw the prefix */
  uint8_t fence;  /* inside (or delimiter of) a ``` fence             */
  uint8_t bullet; /* 1: draw the bullet glyph; 2: bullet continuation */
  uint8_t st_bold, st_code; /* inline state at chunk start            */
} chat_line;

static chat_line *g_lines;
static size_t g_lines_n, g_lines_cap;

static chat_line *line_push(void) {
  if (g_lines_n == g_lines_cap) {
    size_t cap = g_lines_cap != 0 ? g_lines_cap * 2 : 128;
    chat_line *p = realloc(g_lines, cap * sizeof *p);
    if (p == NULL) return NULL;
    g_lines = p;
    g_lines_cap = cap;
  }
  memset(&g_lines[g_lines_n], 0, sizeof g_lines[0]);
  return &g_lines[g_lines_n++];
}

static int is_marker(char ch, int fence, uint8_t code) {
  if (fence) return 0;
  if (ch == '`') return 1;
  if (ch == '*' && !code) return 1;
  return 0;
}

/* Fit one chunk of [start,end) into `avail` cells, honoring markers and
 * preferring a break at the last space. Returns the chunk end; *next is
 * where the following chunk starts; *bold / *code are updated. */
static size_t md_fit(const char *s, size_t start, size_t end, int avail,
                     int fence, uint8_t *bold, uint8_t *code,
                     size_t *next) {
  size_t i = start;
  int cells = 0;
  size_t last_space = 0;
  int have_space = 0;
  uint8_t sp_bold = *bold, sp_code = *code; /* state at last_space + 1 */
  if (avail < 1) avail = 1;
  while (i < end) {
    size_t ni = tui_u8_next(s, i);
    char ch = s[i];
    if (is_marker(ch, fence, *code)) {
      if (ch == '`') *code = (uint8_t)!*code;
      else *bold = (uint8_t)!*bold;
      i = ni;
      continue;
    }
    if (cells + 1 > avail) {
      if (have_space) {
        /* the next chunk rescans from last_space + 1: rewind the
         * marker state too, so each marker toggles exactly once */
        *bold = sp_bold;
        *code = sp_code;
        *next = last_space + 1;
        return last_space;
      }
      *next = i;
      return i;
    }
    if (ch == ' ') {
      last_space = i;
      have_space = 1;
      sp_bold = *bold;
      sp_code = *code;
    }
    cells++;
    i = ni;
  }
  *next = end;
  return end;
}

static int entry_indent(const chat_entry *e) {
  switch (e->role) {
  case CHAT_USER: return PFX_USER;
  case CHAT_ASSISTANT: return PFX_ASSIST;
  default: return PFX_SYSTEM;
  }
}

/* Lays the whole transcript out at width w; returns the line count. */
static int chat_layout(tui_chat *c, int w) {
  size_t ei;
  g_lines_n = 0;
  for (ei = 0; ei < c->n; ei++) {
    chat_entry *e = &c->e[ei];
    const char *s = e->text != NULL ? e->text : "";
    size_t pos = 0;
    int first = 1;
    int fence = 0;
    int indent = entry_indent(e);
    int avail = w - indent;
    if (avail < 8) avail = 8;

    if (e->len == 0) { /* streaming placeholder: prefix-only line */
      chat_line *l = line_push();
      if (l == NULL) return (int)g_lines_n;
      l->entry = (int)ei;
      l->first = 1;
      continue;
    }

    for (;;) {
      /* one source line: [pos, eol) */
      size_t eol = pos;
      int bullet = 0;
      int fence_line;
      uint8_t bold = 0, code = 0;
      size_t ls;
      while (eol < e->len && s[eol] != '\n') eol++;
      fence_line = (eol - pos >= 3 && strncmp(s + pos, "```", 3) == 0);
      if (fence_line) fence = !fence;
      if (!fence_line && !fence && eol - pos >= 2 && s[pos] == '-' &&
          s[pos + 1] == ' ') {
        bullet = 1;
        pos += 2;
      }
      ls = pos;
      do {
        chat_line *l = line_push();
        size_t next = eol;
        size_t stop;
        if (l == NULL) return (int)g_lines_n;
        l->entry = (int)ei;
        l->first = (uint8_t)first;
        l->fence = (uint8_t)(fence || fence_line);
        l->bullet = (uint8_t)bullet;
        l->st_bold = bold;
        l->st_code = code;
        stop = md_fit(s, ls, eol, avail - (bullet != 0 ? 2 : 0),
                      fence || fence_line, &bold, &code, &next);
        l->start = ls;
        l->end = stop;
        first = 0;
        ls = next;
        if (bullet == 1) bullet = 2; /* continuations hang */
      } while (ls < eol);
      if (eol >= e->len) break;
      pos = eol + 1;
      if (pos >= e->len) break; /* trailing newline */
    }
  }
  return (int)g_lines_n;
}

/* ── draw ─────────────────────────────────────────────────────────────── */

static void draw_prefix(tui_frame *f, const tui_theme *th, int x, int y,
                        const chat_entry *e) {
  switch (e->role) {
  case CHAT_USER:
    x += d_put(f, x, y, "you  ", TFG_DIM, TBG_DEFAULT, 0);
    x += d_put(f, x, y, th->prompt, TFG_DIM, TBG_DEFAULT, 0);
    d_put(f, x, y, " ", TFG_DIM, TBG_DEFAULT, 0);
    break;
  case CHAT_ASSISTANT:
    x += d_put(f, x, y, "asngn ", TFG_DEFAULT, TBG_DEFAULT, 0);
    x += d_put(f, x, y, th->star, TFG_ACCENT, TBG_DEFAULT, 0);
    d_put(f, x, y, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
    break;
  default:
    x += d_put(f, x, y, th->bullet, TFG_DIM, TBG_DEFAULT, 0);
    d_put(f, x, y, " ", TFG_DIM, TBG_DEFAULT, 0);
    break;
  }
}

static void draw_line(tui_frame *f, const tui_theme *th, int x, int y,
                      int w, const tui_chat *c, const chat_line *l) {
  const chat_entry *e = &c->e[l->entry];
  const char *s = e->text != NULL ? e->text : "";
  int indent = entry_indent(e);
  uint8_t bold = l->st_bold, code = l->st_code;
  size_t i = l->start;
  int cx = x + indent;

  if (l->first) draw_prefix(f, th, x, y, e);
  if (l->bullet != 0) {
    if (l->bullet == 1)
      d_put(f, cx, y, th->bullet, TFG_ACCENT, TBG_DEFAULT, 0);
    cx += 2;
  }

  while (i < l->end && cx < x + w) {
    size_t ni = tui_u8_next(s, i);
    char ch = s[i];
    uint8_t fg = TFG_DEFAULT, bg = TBG_DEFAULT, attr = 0;
    char glyph[5];
    if (is_marker(ch, l->fence, code)) {
      if (ch == '`') code = (uint8_t)!code;
      else bold = (uint8_t)!bold;
      i = ni;
      continue;
    }
    if (e->role == CHAT_SYSTEM) {
      fg = TFG_DIM;
    }
    if (l->fence) {
      /* dim on slightly shaded; the 16-color theme maps the shade to
       * "" so this degrades to plain dim automatically */
      attr = TA_DIM;
      bg = TBG_SHADE;
    } else {
      if (code) attr |= TA_DIM;
      if (bold) attr |= TA_BOLD;
    }
    if (i >= e->badge_off) { /* badge tail: dim-yellow */
      fg = TFG_ACCENT;
      attr = TA_DIM;
      bg = TBG_DEFAULT;
    }
    {
      size_t len = ni - i;
      if (len > 4) len = 4;
      memcpy(glyph, s + i, len);
      glyph[len] = '\0';
    }
    d_cell(f, cx, y, glyph, fg, bg, attr);
    cx++;
    i = ni;
  }
}

void chat_draw(tui_chat *c, tui_frame *f, const tui_theme *th, int x,
               int y, int w, int h) {
  int total, first, row;
  if (w < 12 || h < 1) return;
  total = chat_layout(c, w);
  c->last_total = total;
  c->last_h = h;
  {
    int max = total - h;
    if (max < 0) max = 0;
    if (c->scroll > max) c->scroll = max;
  }
  first = total - h - c->scroll;
  if (first < 0) first = 0;
  for (row = 0; row < h && first + row < total; row++)
    draw_line(f, th, x, y + row, w, c, &g_lines[first + row]);

  if (c->scroll > 0 && c->unseen) {
    int mx = x + w - 6;
    mx += d_put(f, mx, y + h - 1, th->newmark, TFG_DIM, TBG_DEFAULT, 0);
    d_put(f, mx, y + h - 1, " new", TFG_DIM, TBG_DEFAULT, 0);
  }
}
