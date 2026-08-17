/*
 * tui.h — internal contracts of the asngn terminal application.
 *
 * One header for the whole TUI: terminal layer (term.c), palette and
 * glyphs (theme.c), cell-buffer drawing (draw.c), chat pane (chat.c),
 * sidebar panes (panes.c), input editor (input.c), and modal overlays
 * (modal.c). main.c owns the app state and the event loop.
 *
 * Convention: every TUI source file includes this header FIRST — it
 * defines the POSIX feature-test macros before any system header.
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASNGN_TUI_H
#define ASNGN_TUI_H

#ifdef _WIN32
/* Future work: the Win32 console backend (raw input records +
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING) is not implemented yet; this build
 * targets POSIX terminals only. */
#error "the asngn TUI targets POSIX terminals; Win32 support is future work"
#endif

/* Feature-test macros must precede every include (see src/os_posix.c). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#include "asngn.h"

/* ── palette (theme.c) ────────────────────────────────────────────────── */

/* Foreground palette indices; the theme maps them to SGR fragments per
 * color capability. Yellow is the identity: TFG_ACCENT everywhere a
 * single contrast color is called for. */
enum {
  TFG_DEFAULT = 0, /* terminal default foreground                       */
  TFG_DIM,         /* gray — chrome, hints, system lines                */
  TFG_ACCENT,      /* warm star-yellow (255,198,77) / 221 / bright yel. */
  TFG_AMBER,       /* soft amber (214,158,46) / 179 — warnings          */
  TFG_RED,         /* errors, over-budget — 203-ish                     */
  TFG_BRIGHT,      /* bright white — session name                       */
  TFG_COUNT
};
enum { TBG_DEFAULT = 0, TBG_SHADE, TBG_COUNT };

/* Cell attribute bits. */
enum { TA_BOLD = 1, TA_DIM = 2, TA_REVERSE = 4 };

typedef enum {
  TUI_COLOR_16 = 0,
  TUI_COLOR_256,
  TUI_COLOR_TRUE
} tui_colormode;

typedef struct {
  int ascii; /* theme "plain": monochrome, no glyphs */
  const char *star;      /* "✦" / "*"  */
  const char *bullet;    /* "·" / "-"  */
  const char *marker;    /* "▸" / ">"  */
  const char *prompt;    /* "›" / ">"  */
  const char *bar_full;  /* "▓" / "#"  */
  const char *bar_empty; /* "░" / "-"  */
  const char *newmark;   /* "▼" / "v"  */
  const char *check;     /* "✓" / "+"  */
  const char *cross;     /* "✗" / "x"  */
  const char *dot_on;    /* "●" / "*"  */
  const char *dot_off;   /* "○" / "."  */
  const char *ellipsis;  /* "…" / ".." */
  const char *spinner[10];
  int spinner_n;
  /* single (rounded) box */
  const char *tl, *tr, *bl, *br, *hline, *vline;
  /* double box (confirmation modal) */
  const char *dtl, *dtr, *dbl, *dbr, *dh, *dv;
  /* frame separator joints ├ ┤ */
  const char *sep_l, *sep_r;
  /* braille sparkline levels, low → high */
  const char *spark[4];
} tui_theme;

void tui_theme_init(tui_theme *t, int plain);
/* SGR fragment (no ESC[, no m) for a palette index; "" = terminal
 * default. mode selects the truecolor / 256 / 16 rendering. */
const char *tui_fg_seq(tui_colormode m, uint8_t fg);
const char *tui_bg_seq(tui_colormode m, uint8_t bg);
/* Non-zero when the locale env (LC_ALL / LC_CTYPE / LANG) is UTF-8. */
int tui_locale_utf8(void);

/* ── cells and frames (draw.c) ────────────────────────────────────────── */

typedef struct {
  char utf8[5]; /* one codepoint, NUL-terminated; "" = blank */
  uint8_t fg, bg, attr;
} tui_cell;

typedef struct {
  int w, h;
  tui_cell *cells; /* w*h, row-major */
} tui_frame;

int  tui_frame_alloc(tui_frame *f, int w, int h);
void tui_frame_free(tui_frame *f);

/* UTF-8 helpers: byte offset of next/previous codepoint, codepoint
 * count, copy of one codepoint into dst[5]. */
size_t tui_u8_next(const char *s, size_t i);
size_t tui_u8_prev(const char *s, size_t i);
int    tui_u8_count(const char *s, size_t len);
void   tui_u8_char(char dst[5], const char *s);

void d_clear(tui_frame *f);
void d_cell(tui_frame *f, int x, int y, const char *glyph,
            uint8_t fg, uint8_t bg, uint8_t attr);
/* Writes s one codepoint per cell, clipped; returns cells advanced. */
int  d_put(tui_frame *f, int x, int y, const char *s,
           uint8_t fg, uint8_t bg, uint8_t attr);
/* As d_put but at most maxcells cells. */
int  d_putn(tui_frame *f, int x, int y, const char *s, int maxcells,
            uint8_t fg, uint8_t bg, uint8_t attr);
void d_hline(tui_frame *f, int x, int y, int w, const char *glyph,
             uint8_t fg, uint8_t attr);
/* Rounded (dbl == 0) or double (dbl != 0) box outline. */
void d_box(tui_frame *f, int x, int y, int w, int h,
           const tui_theme *th, int dbl, uint8_t fg, uint8_t attr);
/* Repaint a region as dim chrome (modal backdrop). */
void d_dim_rect(tui_frame *f, int x, int y, int w, int h);

/* "236" / "12.4k" / "1.2M" into buf (>= 8 bytes). */
void tui_fmt_count(char *buf, size_t n, long long v);
/* "12ms" / "6.2s" into buf (>= 12 bytes). */
void tui_fmt_ms(char *buf, size_t n, long long ms);
/* "never" / "12s ago" / "3m ago" / "2h ago" into buf (>= 16 bytes). */
void tui_fmt_ago(char *buf, size_t n, long long unix_s);

/* ── terminal layer (term.c) ──────────────────────────────────────────── */

typedef struct {
  int w, h;
  tui_frame cur, prev;
  tui_colormode colors;
  int raw_active;
  int wake_r, wake_w; /* self-pipe: engine callbacks + signals wake poll */
  struct termios saved_tio;
  char  *out;
  size_t out_len, out_cap;
  /* stdin bytes not yet decoded: the tail of a read that filled the key
   * budget, or an escape sequence / UTF-8 codepoint cut mid-read. */
  unsigned char in_buf[512];
  size_t in_len;
  int in_hup; /* POLLHUP/POLLERR seen on stdin: the terminal is gone */
} tui_term;

/* 0 on success; -1 when stdin/stdout is not an interactive terminal
 * (a message advising --once is printed to stderr).
 * truecolor_hint: -1 auto-detect, 0 force off, 1 force on. */
int  tui_term_open(tui_term *t, int truecolor_hint);
void tui_term_close(tui_term *t);
/* Diff cur against prev and emit minimal VT output; cursor shown at
 * (cx,cy) or hidden when cx < 0. */
void tui_term_render(tui_term *t, int cx, int cy);
void tui_term_invalidate(tui_term *t); /* force full redraw next frame */
/* poll(2) on stdin + the wake pipe. Returns -1 on error, else 0;
 * *stdin_ready / *wake_ready are set. A hangup on stdin latches
 * t->in_hup (and reports stdin_ready so pending bytes get drained). */
int  tui_term_poll(tui_term *t, int timeout_ms,
                   int *stdin_ready, int *wake_ready);
void tui_term_wake(tui_term *t); /* one byte down the self-pipe */
/* Drain the wake pipe; returns non-zero when a resize was pending (the
 * frames are re-allocated and invalidated). */
int  tui_term_drain_wake(tui_term *t);
/* Non-zero once a quit signal (SIGINT/SIGTERM) was received. */
int  tui_term_quit_requested(void);

/* decoded keys */
enum {
  TK_NONE = 0, TK_CHAR,
  TK_ENTER, TK_NEWLINE, /* Enter vs Ctrl+J / Alt+Enter */
  TK_ESC, TK_TAB, TK_BACKSPACE, TK_DELETE,
  TK_UP, TK_DOWN, TK_LEFT, TK_RIGHT, TK_HOME, TK_END, TK_PGUP, TK_PGDN,
  TK_F1, TK_F2, TK_F3, TK_F4, TK_F5, TK_F6, TK_F7, TK_F8,
  TK_CTRL_C, TK_CTRL_D, TK_CTRL_U, TK_CTRL_K, TK_CTRL_W, TK_CTRL_Y
};
typedef struct {
  int kind;
  char utf8[5]; /* TK_CHAR payload */
} tui_key;
/* Reads whatever is pending on stdin and decodes up to max keys.
 * Undecoded bytes are carried in t->in_buf: call again until 0 is
 * returned to drain a burst larger than max (nothing is discarded). */
int tui_term_read_keys(tui_term *t, tui_key *out, int max);

/* ── input editor (input.c) ───────────────────────────────────────────── */

#define TUI_ED_MAX   4096
#define TUI_HIST_MAX 64

typedef struct {
  char   buf[TUI_ED_MAX];
  size_t len, cur; /* byte length, byte cursor */
  char   kill[TUI_ED_MAX];
  size_t kill_len;
  char  *hist[TUI_HIST_MAX];
  size_t hist_n;
  int    hist_pos; /* -1 = not browsing */
  char   saved[TUI_ED_MAX];
  size_t saved_len;
} tui_editor;

void ed_init(tui_editor *e);
void ed_free(tui_editor *e);
void ed_clear(tui_editor *e);
void ed_set(tui_editor *e, const char *s);
int  ed_insert(tui_editor *e, const char *s, size_t n);
void ed_backspace(tui_editor *e);
void ed_delete(tui_editor *e);
void ed_left(tui_editor *e);
void ed_right(tui_editor *e);
void ed_home(tui_editor *e);
void ed_end(tui_editor *e);
void ed_kill_to_start(tui_editor *e); /* Ctrl+U */
void ed_kill_to_end(tui_editor *e);   /* Ctrl+K */
void ed_kill_word(tui_editor *e);     /* Ctrl+W */
void ed_yank(tui_editor *e);          /* Ctrl+Y */
void ed_hist_add(tui_editor *e, const char *line);
void ed_hist_up(tui_editor *e);
void ed_hist_down(tui_editor *e);

typedef struct {
  size_t start, end; /* byte range of one display row */
} ed_row;
/* Wraps the editor into rows of `width` cells (newlines break rows).
 * Fills at most max_rows rows; returns the total row count (>= 1) and
 * the cursor's row/column in *crow / *ccol. */
int ed_layout(const tui_editor *e, int width, ed_row *rows, int max_rows,
              int *crow, int *ccol);

/* ── chat pane (chat.c) ───────────────────────────────────────────────── */

typedef enum { CHAT_USER = 0, CHAT_ASSISTANT, CHAT_SYSTEM } chat_role;

typedef struct {
  chat_role role;
  char  *text;
  size_t len, cap;
  size_t badge_off; /* bytes >= badge_off render dim-yellow; ==len: none */
  size_t turn;      /* engine turn number, 0 = unknown */
} chat_entry;

typedef struct {
  chat_entry *e;
  size_t n, cap;
  int scroll;     /* display lines above the bottom; 0 = follow tail */
  int unseen;     /* content arrived while scrolled up */
  int last_total; /* display-line count of the last layout */
  int last_h;     /* pane height of the last draw */
} tui_chat;

void        chat_init(tui_chat *c);
void        chat_free(tui_chat *c);
chat_entry *chat_push(tui_chat *c, chat_role role);
int         chat_append(tui_chat *c, chat_entry *e, const char *s, size_t n);
void        chat_badge(tui_chat *c, chat_entry *e, const char *badge);
void        chat_systemf(tui_chat *c, const char *fmt, ...);
void        chat_scroll_by(tui_chat *c, int lines);
void        chat_draw(tui_chat *c, tui_frame *f, const tui_theme *th,
                      int x, int y, int w, int h);

/* ── telemetry ring + panes (panes.c) ─────────────────────────────────── */

enum {
  PANE_TRACE = 0, PANE_STATS, PANE_MEMORY, PANE_TOOLS, PANE_CACHE,
  PANE_COUNT
};

typedef struct {
  char   kind[16];  /* event kind, or "log" */
  char   name[40];  /* classify / tool.command / guard: x / ...        */
  char   tier[16];  /* model id, "tool", outcome, ...                  */
  char   extra[32]; /* "0.3s", "12ms · 236 tok", cos, ...              */
  int    ok;        /* tool_call: 1/0; -1 not applicable               */
  int    amber, red;
  int    repeats;   /* log rows: extra consecutive identical lines     */
  double cos;       /* cache_probe cosine; < 0 none                    */
} tui_ev;

#define TUI_EV_CAP 256

typedef struct {
  tui_ev   ev[TUI_EV_CAP]; /* ring; ev[seq % CAP]                      */
  uint64_t seq;            /* total events pushed                      */
  uint64_t turn_seq;       /* seq at the last turn_start (trace reset) */
} tui_evring;

struct tui_app; /* below */

/* Parse one #asngn_event xCDN line and fold it into the app state
 * (ring, counters, confirm slot). Tolerates unknown kinds. */
void tui_events_ingest(struct tui_app *a, const char *line_xcdn);
/* WARN/ERROR log records enter the trace as amber/red lines. */
void tui_events_log(struct tui_app *a, int level, const char *msg);
void panes_draw(struct tui_app *a, tui_frame *f, int x, int y,
                int w, int h);

/* ── modal overlays (modal.c) ─────────────────────────────────────────── */

typedef struct {
  int  active;
  char id[40];
  char tool[64], cmd[64];
  char args[512];
  int  destructive;
} tui_confirm;

void modal_draw_confirm(struct tui_app *a, tui_frame *f);
void modal_draw_help(struct tui_app *a, tui_frame *f);

/* ── engine → main-loop queue ────────────────────────────────────────── */

/* Engine callbacks (token / event / log) run on engine threads: they
 * only append here and write one byte to the self-pipe. The main loop
 * drains under the same mutex; asngn_* is never called from callbacks. */
typedef enum { TMSG_TOKEN = 0, TMSG_EVENT, TMSG_LOG } tui_msg_kind;

typedef struct tui_msg {
  struct tui_msg *next;
  tui_msg_kind    kind;
  int             level; /* TMSG_LOG */
  char            text[];
} tui_msg;

typedef struct {
  pthread_mutex_t mu;
  tui_msg *head, *tail;
  tui_term *term; /* wake target; NULL in --frame-dump */
} tui_queue;

void     tui_queue_init(tui_queue *q, tui_term *term);
void     tui_queue_free(tui_queue *q);
void     tui_queue_push(tui_queue *q, tui_msg_kind kind, int level,
                        const char *text);
tui_msg *tui_queue_drain(tui_queue *q); /* linked list, oldest first */

/* ── app state (main.c) ───────────────────────────────────────────────── */

#define TUI_PENDING_MAX 16
#define TUI_SPARK_N     20

typedef struct tui_app {
  asngn_ctx     *ctx;
  asngn_session *ses;
  asngn_task    *task;

  tui_term   term;
  tui_theme  theme;
  tui_chat   chat;
  tui_editor ed;
  tui_evring evs;
  tui_queue  q;

  char slug[72];
  int  pane;       /* active sidebar pane */
  int  sidebar_on;
  int  help_on;
  int  quit;

  tui_confirm  confirm;
  asngn_detail detail; /* submit detail for future turns */

  /* cached engine introspection */
  asngn_stats         stats;
  asngn_session_stats sstats;
  asngn_sibling_stats sib;
  asngn_model_info    models[8];
  size_t              models_n;
  int stats_ok, sstats_ok, sib_ok;

  /* config snapshot (read once after asngn_open; see main.c) */
  long long budget_session;
  double    warn_at;

  /* live turn bookkeeping */
  char      tier_last[16];
  size_t    live_tokens;     /* streamed chunks this turn */
  long long live_t0_ms;      /* first streamed chunk; 0 = none yet */
  double    tps_last;        /* tok/s of the last answer model_call */
  char      now_what[48];    /* live phase ("answer @ light"); "" idle */
  long long now_t0_ms;       /* when the phase started */
  long long turn_tokens;     /* model_call in+out this turn */
  long long aux_tokens;      /* model_call tokens, task != answer */
  int       turn_guard_stop; /* step_budget / tool_cap tripped */
  int       live_idx;        /* streaming assistant entry, -1 none */
  size_t    last_turn;       /* last committed assistant turn */

  /* local pending FIFO while a turn runs */
  char  *pending[TUI_PENDING_MAX];
  size_t pending_n;
  long long pending_retry_ms; /* next retry while the engine folds */
  int       fold_note;        /* "waiting on fold" printed once     */

  int spark[TUI_SPARK_N]; /* tokens per turn, oldest first */
  int spark_n;

  int       spin;
  long long last_render_ms;
  int       dirty, structural;
} tui_app;

/* Renders one full frame of the UI into f (shared by the interactive
 * loop and the --frame-dump golden hook). Cursor position for the
 * input editor is returned in *cx / *cy (-1 = hidden). */
void tui_render_frame(tui_app *a, tui_frame *f, int *cx, int *cy);

#endif /* ASNGN_TUI_H */
