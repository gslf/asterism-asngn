/*
 * term.c — terminal layer of the asngn TUI.
 *
 * POSIX: raw mode via termios; output is VT escape sequences over a
 * damage-tracked double buffer diffed per frame. Resize via SIGWINCH
 * (flag + self-pipe), geometry via TIOCGWINSZ, color capability from
 * $COLORTERM / $TERM. The terminal is restored on exit (atexit) and on
 * SIGINT/SIGTERM (handlers).
 *
 * Win32: the same VT byte stream rides on the Windows console with
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING and a UTF-8 output code page; keys
 * arrive as raw INPUT_RECORDs (decoded natively, no escape parsing),
 * resize as WINDOW_BUFFER_SIZE_EVENT records, and the self-pipe becomes
 * a manual-reset event that both the engine callbacks and the console
 * ctrl handler signal. Requires a VT-capable console (Windows 10+).
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

/* ── globals for signal handlers / atexit ─────────────────────────────── */

static tui_term *g_term;
static volatile sig_atomic_t g_resized;
static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_quit_count;

#ifndef _WIN32

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

#else /* _WIN32 */

/* Restore console modes and code page; safe from the ctrl-handler
 * thread (console APIs only). */
static void term_restore_raw(void) {
  tui_term *t = g_term;
  DWORD wr;
  if (t == NULL || !t->raw_active) return;
  {
    /* show cursor, reset SGR, leave the alternate screen */
    static const char bye[] = "\x1b[?25h\x1b[0m\x1b[?1049l";
    (void)WriteFile((HANDLE)t->h_out, bye, (DWORD)(sizeof bye - 1), &wr,
                    NULL);
  }
  (void)SetConsoleMode((HANDLE)t->h_in, t->saved_in_mode);
  (void)SetConsoleMode((HANDLE)t->h_out, t->saved_out_mode);
  (void)SetConsoleOutputCP(t->saved_out_cp);
  t->raw_active = 0;
}

static void term_atexit(void) { term_restore_raw(); }

/* Ctrl+C arrives as a plain key event (processed input is off); this
 * handler sees Ctrl+Break plus console close / logoff / shutdown. */
static BOOL WINAPI on_ctrl_event(DWORD type) {
  (void)type;
  g_quit = 1;
  g_quit_count++;
  if (g_quit_count >= 2) {
    term_restore_raw();
    _exit(130);
  }
  if (g_term != NULL && g_term->wake_ev != NULL)
    SetEvent((HANDLE)g_term->wake_ev);
  return TRUE;
}

#endif /* _WIN32 */

int tui_term_quit_requested(void) { return g_quit != 0; }

/* ── color capability ─────────────────────────────────────────────────── */

static tui_colormode term_detect_colors(void) {
#ifdef _WIN32
  /* VT output implies at least the 256/24-bit mapping (Windows 10);
   * Windows Terminal renders true color faithfully. */
  const char *ct = getenv("COLORTERM");
  if (ct != NULL &&
      (strstr(ct, "truecolor") != NULL || strstr(ct, "24bit") != NULL))
    return TUI_COLOR_TRUE;
  if (getenv("WT_SESSION") != NULL) return TUI_COLOR_TRUE;
  return TUI_COLOR_256;
#else
  const char *ct = getenv("COLORTERM");
  const char *tm = getenv("TERM");
  if (ct != NULL &&
      (strstr(ct, "truecolor") != NULL || strstr(ct, "24bit") != NULL))
    return TUI_COLOR_TRUE;
  if (tm != NULL && strstr(tm, "256color") != NULL) return TUI_COLOR_256;
  return TUI_COLOR_16;
#endif
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

static int term_apply_size(tui_term *t, int w, int h) {
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

static int term_measure(tui_term *t) {
  int w = 80, h = 24;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO sbi;
  if (GetConsoleScreenBufferInfo((HANDLE)t->h_out, &sbi)) {
    int cw = sbi.srWindow.Right - sbi.srWindow.Left + 1;
    int ch = sbi.srWindow.Bottom - sbi.srWindow.Top + 1;
    if (cw > 0 && ch > 0) {
      w = cw;
      h = ch;
    }
  }
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
      ws.ws_row > 0) {
    w = ws.ws_col;
    h = ws.ws_row;
  }
#endif
  return term_apply_size(t, w, h);
}

/* ── open / close ─────────────────────────────────────────────────────── */

#ifndef _WIN32

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

#else /* _WIN32 */

int tui_term_open(tui_term *t, int truecolor_hint) {
  HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD im = 0, om = 0, wr;

  memset(t, 0, sizeof *t);

  /* GetConsoleMode doubles as the isatty check: it fails on pipes. */
  if (hin == INVALID_HANDLE_VALUE || hout == INVALID_HANDLE_VALUE ||
      !GetConsoleMode(hin, &im) || !GetConsoleMode(hout, &om)) {
    fprintf(stderr,
            "asngn: the interactive TUI needs a terminal "
            "(output is not a console).\n"
            "       try: asngn --once \"your message\"\n");
    return -1;
  }
  t->h_in = hin;
  t->h_out = hout;
  t->saved_in_mode = im;
  t->saved_out_mode = om;
  t->saved_out_cp = GetConsoleOutputCP();

  t->colors = term_detect_colors();
  if (truecolor_hint == 1) t->colors = TUI_COLOR_TRUE;
  if (truecolor_hint == 0 && t->colors == TUI_COLOR_TRUE)
    t->colors = TUI_COLOR_256;

  t->wake_ev = CreateEventW(NULL, TRUE, FALSE, NULL); /* manual reset */
  if (t->wake_ev == NULL) {
    fprintf(stderr, "asngn: CreateEvent failed (%lu)\n",
            (unsigned long)GetLastError());
    return -1;
  }

  if (!SetConsoleMode(hout, ENABLE_PROCESSED_OUTPUT |
                                ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                DISABLE_NEWLINE_AUTO_RETURN)) {
    /* pre-Win10 conhost: no VT processing, the UI cannot render */
    fprintf(stderr,
            "asngn: this console has no VT support; use Windows 10+ "
            "(Windows Terminal recommended), or: asngn --once \"...\"\n");
    CloseHandle((HANDLE)t->wake_ev);
    t->wake_ev = NULL;
    return -1;
  }
  /* Raw keys: no line input, echo or processed input (Ctrl+C is a key).
   * WINDOW_INPUT delivers resize records; EXTENDED_FLAGS without
   * QUICK_EDIT stops accidental text selection from freezing output. */
  (void)SetConsoleMode(hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
  (void)SetConsoleOutputCP(CP_UTF8);
  t->raw_active = 1;
  g_term = t;
  atexit(term_atexit);
  (void)SetConsoleCtrlHandler(on_ctrl_event, TRUE);

  if (term_measure(t) != 0) return -1;

  {
    /* alternate screen, clear, hide cursor */
    static const char hi[] = "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l";
    (void)WriteFile(hout, hi, (DWORD)(sizeof hi - 1), &wr, NULL);
  }
  return 0;
}

void tui_term_close(tui_term *t) {
  term_restore_raw();
  if (t->wake_ev != NULL) {
    CloseHandle((HANDLE)t->wake_ev);
    t->wake_ev = NULL;
  }
  tui_frame_free(&t->cur);
  tui_frame_free(&t->prev);
  free(t->out);
  t->out = NULL;
  t->out_len = t->out_cap = 0;
  if (g_term == t) g_term = NULL;
}

#endif /* _WIN32 */

/* ── poll / wake ──────────────────────────────────────────────────────── */

#ifndef _WIN32

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

#else /* _WIN32 */

int tui_term_poll(tui_term *t, int timeout_ms, int *stdin_ready,
                  int *wake_ready) {
  HANDLE hs[2];
  DWORD rc, tmo;
  *stdin_ready = 0;
  *wake_ready = 0;
  hs[0] = (HANDLE)t->h_in;   /* signaled while input records are queued */
  hs[1] = (HANDLE)t->wake_ev;
  tmo = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
  rc = WaitForMultipleObjects(2, hs, FALSE, tmo);
  if (rc == WAIT_OBJECT_0) *stdin_ready = 1;
  else if (rc == WAIT_OBJECT_0 + 1) *wake_ready = 1;
  else if (rc == WAIT_FAILED) return -1;
  return 0;
}

void tui_term_wake(tui_term *t) {
  if (t->wake_ev != NULL) SetEvent((HANDLE)t->wake_ev);
}

int tui_term_drain_wake(tui_term *t) {
  int resized;
  if (t->wake_ev != NULL) ResetEvent((HANDLE)t->wake_ev);
  resized = g_resized != 0;
  if (resized) {
    g_resized = 0;
    (void)term_measure(t);
  }
  return resized;
}

#endif /* _WIN32 */

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
#ifdef _WIN32
      DWORD n = 0;
      if (!WriteFile((HANDLE)t->h_out, t->out + off,
                     (DWORD)(t->out_len - off), &n, NULL) ||
          n == 0)
        break;
#else
      ssize_t n = write(STDOUT_FILENO, t->out + off, t->out_len - off);
      if (n <= 0) break;
#endif
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

#ifndef _WIN32

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
  if (np >= 1 && (params[1] == 3 || params[1] == 4 ||
                  params[1] == 7 || params[1] == 8)) {
    switch (*kind) {
    case TK_UP: *kind = TK_ALT_UP; break;
    case TK_DOWN: *kind = TK_ALT_DOWN; break;
    case TK_PGUP: *kind = TK_ALT_PGUP; break;
    case TK_PGDN: *kind = TK_ALT_PGDN; break;
    default: break;
    }
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
    } else if (c == 0x10) {
      nk = key_push(out, max, nk, TK_CTRL_P, NULL);
      i++;
    } else if (c == 0x0E) {
      nk = key_push(out, max, nk, TK_CTRL_N, NULL);
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

#else /* _WIN32 */

/* One UTF-32 codepoint -> UTF-8 into dst[5]. */
static void u32_to_u8(unsigned long cp, char dst[5]) {
  if (cp < 0x80) {
    dst[0] = (char)cp;
    dst[1] = '\0';
  } else if (cp < 0x800) {
    dst[0] = (char)(0xC0 | (cp >> 6));
    dst[1] = (char)(0x80 | (cp & 0x3F));
    dst[2] = '\0';
  } else if (cp < 0x10000) {
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    dst[3] = '\0';
  } else {
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    dst[4] = '\0';
  }
}

/* One key-down record -> tui_key kind (+ UTF-8 payload for TK_CHAR).
 * Returns TK_NONE for modifier-only and untranslatable records. */
static int win_decode_key(tui_term *t, const KEY_EVENT_RECORD *k,
                          char u8[5]) {
  WORD vk = k->wVirtualKeyCode;
  WCHAR wc = k->uChar.UnicodeChar;
  DWORD st = k->dwControlKeyState;
  int alt = (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

  u8[0] = '\0';
  switch (vk) {
  case VK_RETURN:
    /* Alt+Enter and Ctrl+Enter (uChar '\n') insert a newline, like the
     * POSIX Alt+Enter / Ctrl+J bindings. */
    if ((st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) || wc == L'\n')
      return TK_NEWLINE;
    return TK_ENTER;
  case VK_ESCAPE: return TK_ESC;
  case VK_TAB: return TK_TAB; /* Shift+Tab: treat as Tab (POSIX parity) */
  case VK_BACK: return TK_BACKSPACE;
  case VK_DELETE: return TK_DELETE;
  case VK_UP: return alt ? TK_ALT_UP : TK_UP;
  case VK_DOWN: return alt ? TK_ALT_DOWN : TK_DOWN;
  case VK_LEFT: return TK_LEFT;
  case VK_RIGHT: return TK_RIGHT;
  case VK_HOME: return TK_HOME;
  case VK_END: return TK_END;
  case VK_PRIOR: return alt ? TK_ALT_PGUP : TK_PGUP;
  case VK_NEXT: return alt ? TK_ALT_PGDN : TK_PGDN;
  case VK_F1: return TK_F1;
  case VK_F2: return TK_F2;
  case VK_F3: return TK_F3;
  case VK_F4: return TK_F4;
  case VK_F5: return TK_F5;
  case VK_F6: return TK_F6;
  case VK_F7: return TK_F7;
  case VK_F8: return TK_F8;
  default: break;
  }
  if (wc == 0) return TK_NONE; /* modifier press, dead key, ... */
  switch (wc) {
  case 0x03: return TK_CTRL_C;
  case 0x04: return TK_CTRL_D;
  case 0x15: return TK_CTRL_U;
  case 0x0B: return TK_CTRL_K;
  case 0x17: return TK_CTRL_W;
  case 0x19: return TK_CTRL_Y;
  case 0x10: return TK_CTRL_P;
  case 0x0E: return TK_CTRL_N;
  case L'\r': return TK_ENTER;
  case L'\n': return TK_NEWLINE;
  case 0x08: case 0x7F: return TK_BACKSPACE;
  case L'\t': return TK_TAB;
  default: break;
  }
  if (wc < 0x20) return TK_NONE; /* other control chars: ignore */
  if (wc >= 0xD800 && wc <= 0xDBFF) { /* high surrogate: hold */
    t->pending_high = (unsigned short)wc;
    return TK_NONE;
  }
  if (wc >= 0xDC00 && wc <= 0xDFFF) { /* low surrogate: pair or drop */
    unsigned long cp;
    if (t->pending_high == 0) return TK_NONE;
    cp = 0x10000ul + (((unsigned long)t->pending_high - 0xD800ul) << 10) +
         ((unsigned long)wc - 0xDC00ul);
    t->pending_high = 0;
    u32_to_u8(cp, u8);
    return TK_CHAR;
  }
  t->pending_high = 0;
  u32_to_u8((unsigned long)wc, u8);
  return TK_CHAR;
}

int tui_term_read_keys(tui_term *t, tui_key *out, int max) {
  int nk = 0;

  /* One record per read: a repeat count can fan out into several keys,
   * and records must never be consumed past the caller's key budget. */
  while (nk < max) {
    INPUT_RECORD rec;
    DWORD avail = 0, got = 0;
    if (!GetNumberOfConsoleInputEvents((HANDLE)t->h_in, &avail)) {
      t->in_hup = 1; /* console is gone: the caller should quit */
      break;
    }
    if (avail == 0) break;
    if (!ReadConsoleInputW((HANDLE)t->h_in, &rec, 1, &got) || got == 0) {
      t->in_hup = 1;
      break;
    }
    if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
      char u8[5];
      int kind = win_decode_key(t, &rec.Event.KeyEvent, u8);
      int reps = rec.Event.KeyEvent.wRepeatCount > 0
                     ? rec.Event.KeyEvent.wRepeatCount
                     : 1;
      int r;
      if (kind == TK_NONE) continue;
      /* Repeats beyond the budget are auto-repeat spam; dropping them
       * loses nothing a user can notice. */
      for (r = 0; r < reps && nk < max; r++)
        nk = key_push(out, max, nk, kind, kind == TK_CHAR ? u8 : NULL);
    } else if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
      /* Handled like SIGWINCH: flag it and wake the loop so the next
       * drain re-measures and repaints. */
      g_resized = 1;
      tui_term_wake(t);
    }
    /* FOCUS_EVENT / MENU_EVENT / MOUSE_EVENT: consumed and ignored. */
  }
  return nk;
}

#endif /* _WIN32 */

/* ── portable primitives (tui.h) ─────────────────────────────────────── */

#ifdef _WIN32

typedef char tui_assert_srwlock_fits[(sizeof(SRWLOCK) <= sizeof(void *))
                                         ? 1
                                         : -1];

void tui_mutex_init(tui_mutex *m) { InitializeSRWLock((PSRWLOCK)&m->h); }
void tui_mutex_destroy(tui_mutex *m) { (void)m; /* SRW locks are static */ }
void tui_mutex_lock(tui_mutex *m) {
  AcquireSRWLockExclusive((PSRWLOCK)&m->h);
}
void tui_mutex_unlock(tui_mutex *m) {
  ReleaseSRWLockExclusive((PSRWLOCK)&m->h);
}

long long tui_now_ms(void) {
  static LARGE_INTEGER freq; /* zero until first call */
  LARGE_INTEGER now;
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (long long)(now.QuadPart / (freq.QuadPart / 1000));
}

#else /* !_WIN32 */

void tui_mutex_init(tui_mutex *m) { pthread_mutex_init(&m->m, NULL); }
void tui_mutex_destroy(tui_mutex *m) { pthread_mutex_destroy(&m->m); }
void tui_mutex_lock(tui_mutex *m) { pthread_mutex_lock(&m->m); }
void tui_mutex_unlock(tui_mutex *m) { pthread_mutex_unlock(&m->m); }

long long tui_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif /* _WIN32 */

/* ── engine → main-loop queue ────────────────────────────────────────── */

void tui_queue_init(tui_queue *q, tui_term *term) {
  tui_mutex_init(&q->mu);
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
  tui_mutex_destroy(&q->mu);
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
  tui_mutex_lock(&q->mu);
  if (q->tail != NULL) q->tail->next = m;
  else q->head = m;
  q->tail = m;
  tui_mutex_unlock(&q->mu);
  if (q->term != NULL) tui_term_wake(q->term);
}

tui_msg *tui_queue_drain(tui_queue *q) {
  tui_msg *m;
  tui_mutex_lock(&q->mu);
  m = q->head;
  q->head = q->tail = NULL;
  tui_mutex_unlock(&q->mu);
  return m;
}
