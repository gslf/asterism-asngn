/*
 * term.c — terminal layer of the asngn TUI.
 *
 * POSIX raw mode via termios; output is VT escape sequences over a
 * damage-tracked double buffer diffed per frame. Resize via SIGWINCH
 * (flag + self-pipe), geometry via TIOCGWINSZ, color capability from
 * $COLORTERM / $TERM. The terminal is restored on exit (atexit) and on
 * SIGINT/SIGTERM (handlers).
 *
 * Win32 (future work): the console backend — raw input records
 * plus ENABLE_VIRTUAL_TERMINAL_PROCESSING — would live behind
 * #ifdef _WIN32 here; this build targets POSIX and tui.h rejects
 * Windows at compile time.
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ── globals for signal handlers / atexit ─────────────────────────────── */

static tui_term *g_term;
static volatile sig_atomic_t g_resized;
static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_quit_count;

/* Async-signal-safe restore: write(2) + tcsetattr(2) only. */
static void term_restore_raw(void) {
  tui_term *t = g_term;
  if (t == NULL || !t->raw_active) return;
  {
    /* show cursor, reset SGR, leave the alternate screen */
    static const char bye[] = "\x1b[?25h\x1b[0m\x1b[?1049l";
    ssize_t rc = write(STDOUT_FILENO, bye, sizeof bye - 1);
    (void)rc;
  }
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->saved_tio);
  t->raw_active = 0;
}

static void term_atexit(void) { term_restore_raw(); }

static void on_winch(int sig) {
  (void)sig;
  g_resized = 1;
  if (g_term != NULL && g_term->wake_w >= 0) {
    ssize_t rc = write(g_term->wake_w, "r", 1);
    (void)rc;
  }
}

static void on_quit_signal(int sig) {
  g_quit = 1;
  g_quit_count++;
  if (g_quit_count >= 2) {
    /* the loop is stuck — restore and die now */
    term_restore_raw();
    _exit(128 + sig);
  }
  if (g_term != NULL && g_term->wake_w >= 0) {
    ssize_t rc = write(g_term->wake_w, "q", 1);
    (void)rc;
  }
}

int tui_term_quit_requested(void) { return g_quit != 0; }

/* ── color capability ─────────────────────────────────────────────────── */

static tui_colormode term_detect_colors(void) {
  const char *ct = getenv("COLORTERM");
  const char *tm = getenv("TERM");
  if (ct != NULL &&
      (strstr(ct, "truecolor") != NULL || strstr(ct, "24bit") != NULL))
    return TUI_COLOR_TRUE;
  if (tm != NULL && strstr(tm, "256color") != NULL) return TUI_COLOR_256;
  return TUI_COLOR_16;
}

/* ── frame allocation ─────────────────────────────────────────────────── */

int tui_frame_alloc(tui_frame *f, int w, int h) {
  size_t n;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  n = (size_t)w * (size_t)h;
  f->cells = calloc(n, sizeof *f->cells);
  if (f->cells == NULL) return -1;
  f->w = w;
  f->h = h;
  return 0;
}

void tui_frame_free(tui_frame *f) {
  free(f->cells);
  f->cells = NULL;
  f->w = f->h = 0;
}

static void term_invalidate_prev(tui_term *t) {
  int i, n = t->prev.w * t->prev.h;
  for (i = 0; i < n; i++) t->prev.cells[i].fg = 0xFF; /* never matches */
}

void tui_term_invalidate(tui_term *t) { term_invalidate_prev(t); }

static int term_measure(tui_term *t) {
  struct winsize ws;
  int w = 80, h = 24;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
      ws.ws_row > 0) {
    w = ws.ws_col;
    h = ws.ws_row;
  }
  if (w == t->w && h == t->h && t->cur.cells != NULL) return 0;
  tui_frame_free(&t->cur);
  tui_frame_free(&t->prev);
  if (tui_frame_alloc(&t->cur, w, h) != 0) return -1;
  if (tui_frame_alloc(&t->prev, w, h) != 0) return -1;
  t->w = w;
  t->h = h;
  term_invalidate_prev(t);
  return 0;
}

/* ── open / close ─────────────────────────────────────────────────────── */

int tui_term_open(tui_term *t, int truecolor_hint) {
  const char *tm = getenv("TERM");
  struct termios raw;
  struct sigaction sa;
  int fds[2];

  memset(t, 0, sizeof *t);
  t->wake_r = t->wake_w = -1;

  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
      (tm != NULL && strcmp(tm, "dumb") == 0)) {
    fprintf(stderr,
            "asngn: the interactive TUI needs a terminal "
            "(TERM=dumb or output is not a tty).\n"
            "       try: asngn --once \"your message\"\n");
    return -1;
  }

  t->colors = term_detect_colors();
  if (truecolor_hint == 1) t->colors = TUI_COLOR_TRUE;
  if (truecolor_hint == 0 && t->colors == TUI_COLOR_TRUE)
    t->colors = TUI_COLOR_256;

  if (pipe(fds) != 0) {
    fprintf(stderr, "asngn: pipe: %s\n", strerror(errno));
    return -1;
  }
  t->wake_r = fds[0];
  t->wake_w = fds[1];
  (void)fcntl(t->wake_r, F_SETFL, O_NONBLOCK);
  (void)fcntl(t->wake_w, F_SETFL, O_NONBLOCK);
  (void)fcntl(t->wake_r, F_SETFD, FD_CLOEXEC);
  (void)fcntl(t->wake_w, F_SETFD, FD_CLOEXEC);

  if (tcgetattr(STDIN_FILENO, &t->saved_tio) != 0) {
    fprintf(stderr, "asngn: tcgetattr: %s\n", strerror(errno));
    return -1;
  }
  raw = t->saved_tio;
  raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= (tcflag_t)~OPOST;
  raw.c_cflag |= CS8;
  raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    fprintf(stderr, "asngn: tcsetattr: %s\n", strerror(errno));
    return -1;
  }
  t->raw_active = 1;
  g_term = t;
  atexit(term_atexit);

  memset(&sa, 0, sizeof sa);
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = on_winch;
  sa.sa_flags = 0; /* interrupt poll on resize */
  sigaction(SIGWINCH, &sa, NULL);
  sa.sa_handler = on_quit_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  if (term_measure(t) != 0) return -1;

  {
    /* alternate screen, clear, hide cursor */
    static const char hi[] = "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l";
    ssize_t rc = write(STDOUT_FILENO, hi, sizeof hi - 1);
    (void)rc;
  }
  return 0;
}

void tui_term_close(tui_term *t) {
  term_restore_raw();
  if (t->wake_r >= 0) close(t->wake_r);
  if (t->wake_w >= 0) close(t->wake_w);
  t->wake_r = t->wake_w = -1;
  tui_frame_free(&t->cur);
  tui_frame_free(&t->prev);
  free(t->out);
  t->out = NULL;
  t->out_len = t->out_cap = 0;
  if (g_term == t) g_term = NULL;
}

/* ── poll / wake ──────────────────────────────────────────────────────── */

int tui_term_poll(tui_term *t, int timeout_ms, int *stdin_ready,
                  int *wake_ready) {
  struct pollfd fds[2];
  int rc;
  *stdin_ready = 0;
  *wake_ready = 0;
  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  fds[1].fd = t->wake_r;
  fds[1].events = POLLIN;
  fds[1].revents = 0;
  rc = poll(fds, 2, timeout_ms);
  if (rc < 0) {
    if (errno == EINTR) return 0; /* signal — flags checked by caller */
    return -1;
  }
  if (rc > 0) {
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) *stdin_ready = 1;
    if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
      t->in_hup = 1; /* stdin hung up: the caller should quit */
    if (fds[1].revents & (POLLIN | POLLHUP)) *wake_ready = 1;
  }
  return 0;
}

void tui_term_wake(tui_term *t) {
  if (t->wake_w >= 0) {
    ssize_t rc = write(t->wake_w, "w", 1);
    (void)rc;
  }
}

int tui_term_drain_wake(tui_term *t) {
  char buf[64];
  int resized;
  if (t->wake_r >= 0)
    while (read(t->wake_r, buf, sizeof buf) > 0) { /* drain */ }
  resized = g_resized != 0;
  if (resized) {
    g_resized = 0;
    (void)term_measure(t);
  }
  return resized;
}

/* ── output buffer ────────────────────────────────────────────────────── */

static void out_reserve(tui_term *t, size_t extra) {
  if (t->out_len + extra <= t->out_cap) return;
  {
    size_t cap = t->out_cap != 0 ? t->out_cap : 4096;
    char *p;
    while (cap < t->out_len + extra) cap *= 2;
    p = realloc(t->out, cap);
    if (p == NULL) return; /* drop output rather than crash */
    t->out = p;
    t->out_cap = cap;
  }
}

static void out_str(tui_term *t, const char *s) {
  size_t n = strlen(s);
  out_reserve(t, n);
  if (t->out_len + n <= t->out_cap) {
    memcpy(t->out + t->out_len, s, n);
    t->out_len += n;
  }
}

static void out_fmt(tui_term *t, const char *fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  out_str(t, buf);
}

/* ── frame diff → VT output ───────────────────────────────────────────── */

static int cell_eq(const tui_cell *a, const tui_cell *b) {
  return a->fg == b->fg && a->bg == b->bg && a->attr == b->attr &&
         strcmp(a->utf8, b->utf8) == 0;
}

void tui_term_render(tui_term *t, int cx, int cy) {
  int x, y;
  int last_x = -2, last_y = -2;
  uint8_t sfg = 0xFF, sbg = 0xFF, sattr = 0xFF;

  if (t->cur.cells == NULL) return;
  t->out_len = 0;
  out_str(t, "\x1b[?25l");

  for (y = 0; y < t->h; y++) {
    for (x = 0; x < t->w; x++) {
      tui_cell *c = &t->cur.cells[y * t->w + x];
      tui_cell *p = &t->prev.cells[y * t->w + x];
      if (cell_eq(c, p)) continue;
      if (y != last_y || x != last_x)
        out_fmt(t, "\x1b[%d;%dH", y + 1, x + 1);
      if (c->fg != sfg || c->bg != sbg || c->attr != sattr) {
        const char *fg = tui_fg_seq(t->colors, c->fg);
        const char *bg = tui_bg_seq(t->colors, c->bg);
        out_str(t, "\x1b[0");
        if (c->attr & TA_BOLD) out_str(t, ";1");
        if (c->attr & TA_DIM) out_str(t, ";2");
        if (c->attr & TA_REVERSE) out_str(t, ";7");
        if (fg[0] != '\0') {
          out_str(t, ";");
          out_str(t, fg);
        }
        if (bg[0] != '\0') {
          out_str(t, ";");
          out_str(t, bg);
        }
        out_str(t, "m");
        sfg = c->fg;
        sbg = c->bg;
        sattr = c->attr;
      }
      out_str(t, c->utf8[0] != '\0' ? c->utf8 : " ");
      *p = *c;
      last_y = y;
      last_x = x + 1;
    }
  }

  if (cx >= 0 && cy >= 0 && cx < t->w && cy < t->h) {
    out_fmt(t, "\x1b[%d;%dH", cy + 1, cx + 1);
    out_str(t, "\x1b[?25h");
  }
  if (t->out_len > 0) {
    size_t off = 0;
    while (off < t->out_len) {
      ssize_t n = write(STDOUT_FILENO, t->out + off, t->out_len - off);
      if (n <= 0) break;
      off += (size_t)n;
    }
  }
}

/* ── key decoding ─────────────────────────────────────────────────────── */

static int key_push(tui_key *out, int max, int n, int kind,
                    const char *utf8) {
  if (n >= max) return n;
  out[n].kind = kind;
  out[n].utf8[0] = '\0';
  if (utf8 != NULL) {
    size_t l = strlen(utf8);
    if (l > 4) l = 4;
    memcpy(out[n].utf8, utf8, l);
    out[n].utf8[l] = '\0';
  }
  return n + 1;
}

/* CSI / SS3 sequence starting at buf[i] (buf[i] == '[' or 'O'); returns
 * the byte just past the sequence and the decoded key in *kind. */
static size_t decode_csi(const unsigned char *buf, size_t i, size_t n,
                         int *kind) {
  int intro = buf[i];
  long params[4] = {0, 0, 0, 0};
  int np = 0;
  unsigned char fin = 0;
  i++;
  *kind = TK_NONE;
  if (intro == 'O') { /* SS3: one final byte */
    if (i >= n) return i;
    fin = buf[i++];
    switch (fin) {
    case 'P': *kind = TK_F1; break;
    case 'Q': *kind = TK_F2; break;
    case 'R': *kind = TK_F3; break;
    case 'S': *kind = TK_F4; break;
    case 'A': *kind = TK_UP; break;
    case 'B': *kind = TK_DOWN; break;
    case 'C': *kind = TK_RIGHT; break;
    case 'D': *kind = TK_LEFT; break;
    case 'H': *kind = TK_HOME; break;
    case 'F': *kind = TK_END; break;
    default: break;
    }
    return i;
  }
  /* CSI: parameters then a final byte in @..~ */
  while (i < n) {
    unsigned char c = buf[i];
    if (c >= '0' && c <= '9') {
      if (np < 4) params[np] = params[np] * 10 + (c - '0');
      i++;
    } else if (c == ';') {
      if (np < 3) np++;
      i++;
    } else if (c >= 0x40 && c <= 0x7E) {
      fin = c;
      i++;
      break;
    } else {
      i++; /* skip stray intermediate */
    }
  }
  switch (fin) {
  case 'A': *kind = TK_UP; break;
  case 'B': *kind = TK_DOWN; break;
  case 'C': *kind = TK_RIGHT; break;
  case 'D': *kind = TK_LEFT; break;
  case 'H': *kind = TK_HOME; break;
  case 'F': *kind = TK_END; break;
  case 'Z': *kind = TK_TAB; break; /* Shift+Tab: treat as Tab */
  case '~':
    switch (params[0]) {
    case 1: *kind = TK_HOME; break;
    case 3: *kind = TK_DELETE; break;
    case 4: *kind = TK_END; break;
    case 5: *kind = TK_PGUP; break;
    case 6: *kind = TK_PGDN; break;
    case 11: *kind = TK_F1; break;
    case 12: *kind = TK_F2; break;
    case 13: *kind = TK_F3; break;
    case 14: *kind = TK_F4; break;
    case 15: *kind = TK_F5; break;
    case 17: *kind = TK_F6; break;
    case 18: *kind = TK_F7; break;
    case 19: *kind = TK_F8; break;
    default: break;
    }
    break;
  default: break;
  }
  return i;
}

int tui_term_read_keys(tui_term *t, tui_key *out, int max) {
  unsigned char *buf = t->in_buf;
  ssize_t got;
  size_t i = 0, n;
  int nk = 0;

  /* Top up the carry buffer with whatever stdin has. Raw mode uses
   * VMIN=0/VTIME=0, so read() returns 0 when nothing is pending —
   * already-carried bytes are still decoded below. */
  if (t->in_len < sizeof t->in_buf) {
    got = read(STDIN_FILENO, buf + t->in_len,
               sizeof t->in_buf - t->in_len);
    if (got > 0) t->in_len += (size_t)got;
  }
  n = t->in_len;

  while (i < n && nk < max) {
    unsigned char c = buf[i];
    /* A full buffer means the burst continues in the next read(); a
     * short tail there may be an escape sequence cut mid-read, so hold
     * it back and reassemble next call. On a drained buffer a trailing
     * ESC is a real Esc keypress, never a split (keys arrive whole). */
    int maybe_split = n == sizeof t->in_buf && n - i < 16;
    if (c == 0x1b) {
      if (i + 1 >= n) { /* lone ESC at buffer end */
        if (maybe_split) break; /* carry: the rest is still in flight */
        nk = key_push(out, max, nk, TK_ESC, NULL);
        i++;
      } else if (buf[i + 1] == '[' || buf[i + 1] == 'O') {
        int kind = TK_NONE;
        size_t j = decode_csi(buf, i + 1, n, &kind);
        if (kind == TK_NONE && j >= n && maybe_split)
          break; /* sequence cut at buffer end: carry */
        i = j;
        if (kind != TK_NONE) nk = key_push(out, max, nk, kind, NULL);
      } else if (buf[i + 1] == '\r' || buf[i + 1] == '\n') {
        nk = key_push(out, max, nk, TK_NEWLINE, NULL); /* Alt+Enter */
        i += 2;
      } else {
        /* ESC coalesced with following input (fast typing, pastes):
         * deliver the Esc and reparse the next byte on its own —
         * losing Alt+<key> chords beats losing Esc. */
        nk = key_push(out, max, nk, TK_ESC, NULL);
        i++;
      }
    } else if (c == '\r') {
      nk = key_push(out, max, nk, TK_ENTER, NULL);
      i++;
    } else if (c == '\n') {
      nk = key_push(out, max, nk, TK_NEWLINE, NULL); /* Ctrl+J */
      i++;
    } else if (c == 0x7F || c == 0x08) {
      nk = key_push(out, max, nk, TK_BACKSPACE, NULL);
      i++;
    } else if (c == '\t') {
      nk = key_push(out, max, nk, TK_TAB, NULL);
      i++;
    } else if (c == 0x03) {
      nk = key_push(out, max, nk, TK_CTRL_C, NULL);
      i++;
    } else if (c == 0x04) {
      nk = key_push(out, max, nk, TK_CTRL_D, NULL);
      i++;
    } else if (c == 0x15) {
      nk = key_push(out, max, nk, TK_CTRL_U, NULL);
      i++;
    } else if (c == 0x0B) {
      nk = key_push(out, max, nk, TK_CTRL_K, NULL);
      i++;
    } else if (c == 0x17) {
      nk = key_push(out, max, nk, TK_CTRL_W, NULL);
      i++;
    } else if (c == 0x19) {
      nk = key_push(out, max, nk, TK_CTRL_Y, NULL);
      i++;
    } else if (c < 0x20) {
      i++; /* other control bytes: ignore */
    } else {
      /* UTF-8 character: lead byte + continuations */
      char ch[5];
      size_t l = 1;
      if ((c & 0xE0) == 0xC0) l = 2;
      else if ((c & 0xF0) == 0xE0) l = 3;
      else if ((c & 0xF8) == 0xF0) l = 4;
      if (i + l > n) break; /* codepoint cut mid-read: carry the tail */
      memcpy(ch, buf + i, l);
      ch[l] = '\0';
      nk = key_push(out, max, nk, TK_CHAR, ch);
      i += l;
    }
  }
  /* keep the undecoded remainder for the next call */
  if (i < n) memmove(buf, buf + i, n - i);
  t->in_len = n - i;
  return nk;
}

/* ── engine → main-loop queue ────────────────────────────────────────── */

void tui_queue_init(tui_queue *q, tui_term *term) {
  pthread_mutex_init(&q->mu, NULL);
  q->head = q->tail = NULL;
  q->term = term;
}

void tui_queue_free(tui_queue *q) {
  tui_msg *m = tui_queue_drain(q);
  while (m != NULL) {
    tui_msg *next = m->next;
    free(m);
    m = next;
  }
  pthread_mutex_destroy(&q->mu);
}

void tui_queue_push(tui_queue *q, tui_msg_kind kind, int level,
                    const char *text) {
  size_t len = text != NULL ? strlen(text) : 0;
  tui_msg *m = malloc(sizeof *m + len + 1);
  if (m == NULL) return; /* drop under OOM — never block the engine */
  m->next = NULL;
  m->kind = kind;
  m->level = level;
  if (len > 0) memcpy(m->text, text, len);
  m->text[len] = '\0';
  pthread_mutex_lock(&q->mu);
  if (q->tail != NULL) q->tail->next = m;
  else q->head = m;
  q->tail = m;
  pthread_mutex_unlock(&q->mu);
  if (q->term != NULL) tui_term_wake(q->term);
}

tui_msg *tui_queue_drain(tui_queue *q) {
  tui_msg *m;
  pthread_mutex_lock(&q->mu);
  m = q->head;
  q->head = q->tail = NULL;
  pthread_mutex_unlock(&q->mu);
  return m;
}
