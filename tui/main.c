/*
 * main.c — the asngn terminal application: argument parsing, the
 * event loop, slash commands, and the headless --once mode.
 *
 * Threading contract: asngn callbacks (tokens, events, log) fire
 * on engine threads. They only append to the mutex-guarded queue and
 * write one byte down the self-pipe; the main loop polls stdin plus the
 * pipe, drains the queue, updates state, and renders. asngn_* is never
 * called from inside a callback.
 *
 * INTERNAL-HEADER EXCEPTION: this file includes "asngn_internal.h" for
 * exactly one purpose — right after asngn_open it (a) applies the
 * --confirm / --once autoconfirm override to c->cfg.autoconfirm
 * and (b) snapshots the read-only TUI/budget config the public API does
 * not expose (tui.theme, tui.truecolor, tui.sidebar, budgets.session,
 * budgets.warn_at). Everything else goes strictly through asngn.h.
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h" /* first: defines the feature-test macros */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#define asngn_tty_in() _isatty(_fileno(stdin))
#define asngn_tty_out() _isatty(_fileno(stdout))
#else
#include <unistd.h>
#define asngn_tty_in() isatty(STDIN_FILENO)
#define asngn_tty_out() isatty(STDOUT_FILENO)
#endif

#include "asngn_internal.h" /* see the exception note above */

/* ── small utilities ──────────────────────────────────────────────────── */

static long long now_ms(void) { return tui_now_ms(); }

static void refresh_stats(tui_app *a) {
  if (a->ctx == NULL) return;
  a->stats_ok = asngn_get_stats(a->ctx, &a->stats) == ASNGN_OK;
  a->sib_ok = asngn_get_sibling_stats(a->ctx, &a->sib) == ASNGN_OK;
  if (a->ses != NULL)
    a->sstats_ok =
        asngn_session_get_stats(a->ses, &a->sstats) == ASNGN_OK;
  if (asngn_get_models(a->ctx, a->models,
                       sizeof a->models / sizeof a->models[0],
                       &a->models_n) != ASNGN_OK)
    a->models_n = 0;
  if (a->models_n > sizeof a->models / sizeof a->models[0])
    a->models_n = sizeof a->models / sizeof a->models[0];
}

/* Replay the open session's transcript into the chat pane so the
 * conversation is always readable from the top — on startup and after
 * every session switch. */
static void chat_replay(tui_app *a) {
  asngn_transcript_entry *ents = NULL;
  size_t n = 0, i;
  if (asngn_session_transcript(a->ses, &ents, &n) != ASNGN_OK) {
    chat_systemf(&a->chat, "history unavailable: %s",
                 asngn_last_error(a->ctx));
    return;
  }
  for (i = 0; i < n; i++) {
    chat_entry *e = chat_push(&a->chat,
                              strcmp(ents[i].role, "user") == 0
                                  ? CHAT_USER
                                  : CHAT_ASSISTANT);
    if (e == NULL) break;
    chat_append(&a->chat, e, ents[i].text, strlen(ents[i].text));
    e->turn = ents[i].turn;
    if (ents[i].role[0] == 'a') a->last_turn = ents[i].turn;
  }
  if (n > 0)
    chat_systemf(&a->chat, "%zu turns restored from the transcript", n);
  asngn_free(ents);
}

/* ── engine callbacks: queue-only, engine threads ────────────────────── */

static void cb_token(const char *utf8, void *ud) {
  tui_app *a = ud;
  tui_queue_push(&a->q, TMSG_TOKEN, 0, utf8);
}

static void cb_event(const char *event_xcdn, void *ud) {
  tui_app *a = ud;
  tui_queue_push(&a->q, TMSG_EVENT, 0, event_xcdn);
}

static void cb_log(int level, const char *msg, void *ud) {
  tui_app *a = ud;
  if (level <= ASNGN_LOG_WARN) /* WARN + ERROR go to the trace pane */
    tui_queue_push(&a->q, TMSG_LOG, level, msg);
}

/* ── frame rendering ─────────────────────────────────────────────────── */

static void header_budget(tui_app *a, tui_frame *f, int *x) {
  const tui_theme *th = &a->theme;
  long long budget = a->budget_session;
  if (budget <= 0) {
    *x += d_put(f, *x, 0, th->ascii ? "inf" : "\xE2\x88\x9E", TFG_DIM,
                TBG_DEFAULT, TA_DIM);
    return;
  }
  {
    long long spent = a->sstats_ok ? a->sstats.spent_tokens : 0;
    double frac = (double)spent / (double)budget;
    int filled = (int)(frac * 10.0 + 0.5);
    uint8_t fg = TFG_ACCENT;
    int i;
    char pct[16];
    if (filled > 10) filled = 10;
    if (filled < 0) filled = 0;
    if (frac >= 1.0) fg = TFG_RED;
    else if (frac >= a->warn_at) fg = TFG_AMBER;
    for (i = 0; i < 10; i++)
      *x += d_put(f, *x, 0, i < filled ? th->bar_full : th->bar_empty,
                  i < filled ? fg : TFG_DIM, TBG_DEFAULT, 0);
    snprintf(pct, sizeof pct, " %d%%", (int)(frac * 100.0 + 0.5));
    *x += d_put(f, *x, 0, pct, TFG_DIM, TBG_DEFAULT, 0);
  }
}

static void render_header(tui_app *a, tui_frame *f) {
  const tui_theme *th = &a->theme;
  int x = 0;
  char buf[64];
  d_cell(f, 0, 0, th->tl, TFG_DIM, TBG_DEFAULT, 0);
  d_hline(f, 1, 0, f->w - 2, th->hline, TFG_DIM, 0);
  d_cell(f, f->w - 1, 0, th->tr, TFG_DIM, TBG_DEFAULT, 0);
  x = 1;
  x += d_put(f, x, 0, " ", TFG_DIM, TBG_DEFAULT, 0);
  x += d_put(f, x, 0, "asngn", TFG_ACCENT, TBG_DEFAULT, TA_BOLD);
  x += d_put(f, x, 0, " ", TFG_DIM, TBG_DEFAULT, 0);
  x += d_put(f, x, 0, th->star, TFG_ACCENT, TBG_DEFAULT, 0);
  x += d_put(f, x, 0, " ", TFG_DIM, TBG_DEFAULT, 0);
  x += d_putn(f, x, 0, a->slug, 24, TFG_BRIGHT, TBG_DEFAULT, 0);
  snprintf(buf, sizeof buf, " %s %s %s ", th->bullet,
           a->tier_last[0] != '\0' ? a->tier_last : "-", th->bullet);
  x += d_put(f, x, 0, buf, TFG_DIM, TBG_DEFAULT, 0);
  header_budget(a, f, &x);
  snprintf(buf, sizeof buf, " %s tok %zu ", th->bullet,
           a->stats_ok ? a->stats.tokens_prompt + a->stats.tokens_gen : 0);
  x += d_put(f, x, 0, buf, TFG_DIM, TBG_DEFAULT, 0);
}

static void render_status(tui_app *a, tui_frame *f) {
  const tui_theme *th = &a->theme;
  int y = f->h - 1;
  int x;
  char buf[128], cnt[16];
  double tps = 0.0;
  d_cell(f, 0, y, th->bl, TFG_DIM, TBG_DEFAULT, 0);
  d_hline(f, 1, y, f->w - 2, th->hline, TFG_DIM, 0);
  d_cell(f, f->w - 1, y, th->br, TFG_DIM, TBG_DEFAULT, 0);
  x = 1;
  x += d_put(f, x, y, " ", TFG_DIM, TBG_DEFAULT, 0);
  if (a->task != NULL || a->now_what[0] != '\0') {
    x += d_put(f, x, y, th->spinner[a->spin % th->spinner_n], TFG_ACCENT,
               TBG_DEFAULT, 0);
    x += d_put(f, x, y, " ", TFG_DIM, TBG_DEFAULT, 0);
  }

  /* what the engine runs right now, with elapsed time */
  if (a->now_what[0] != '\0') {
    long long el = (now_ms() - a->now_t0_ms) / 1000;
    snprintf(buf, sizeof buf, "%s %llds %s ", a->now_what, el,
             th->bullet);
    x += d_put(f, x, y, buf, TFG_ACCENT, TBG_DEFAULT, 0);
  }

  /* generation speed: live while streaming, last answer call after */
  if (a->task != NULL && a->live_tokens >= 5 && a->live_t0_ms > 0) {
    long long el = now_ms() - a->live_t0_ms;
    if (el >= 500) tps = (double)a->live_tokens * 1000.0 / (double)el;
  } else if (a->tps_last > 0.0) {
    tps = a->tps_last;
  }

  if (a->task != NULL) {
    /* streamed tokens of the running turn */
    tui_fmt_count(cnt, sizeof cnt, (long long)a->live_tokens);
    snprintf(buf, sizeof buf, "%s tok %s ", cnt, th->bullet);
    x += d_put(f, x, y, buf, TFG_BRIGHT, TBG_DEFAULT, 0);
  }
  if (tps > 0.0) {
    snprintf(buf, sizeof buf, "%.1f tok/s %s ", tps, th->bullet);
    x += d_put(f, x, y, buf, TFG_DIM, TBG_DEFAULT, 0);
  }
  if (a->sstats_ok) {
    /* whole-conversation spend (prompt+gen, from the ledger) and the
     * share the Asper memory zone took of it */
    tui_fmt_count(cnt, sizeof cnt,
                  (long long)(a->sstats.tokens_prompt +
                              a->sstats.tokens_gen));
    snprintf(buf, sizeof buf, "conv %s tok %s ", cnt, th->bullet);
    x += d_put(f, x, y, buf, TFG_DIM, TBG_DEFAULT, 0);
    tui_fmt_count(cnt, sizeof cnt, (long long)a->sstats.tokens_memory);
    snprintf(buf, sizeof buf, "mem %s tok %s ", cnt, th->bullet);
    x += d_put(f, x, y, buf, TFG_DIM, TBG_DEFAULT, 0);
  }
  snprintf(buf, sizeof buf, "F1 help %s Esc cancel ", th->bullet);
  x += d_put(f, x, y, buf, TFG_DIM, TBG_DEFAULT, 0);
}

void tui_render_frame(tui_app *a, tui_frame *f, int *cx, int *cy) {
  const tui_theme *th = &a->theme;
  ed_row rows[512];
  int nrows, crow = 0, ccol = 0;
  int ed_w, input_rows, sep_row, y;
  *cx = *cy = -1;

  d_clear(f);
  if (f->w < 24 || f->h < 7) {
    d_put(f, 0, 0, "asngn: window too small", TFG_DIM, TBG_DEFAULT, 0);
    return;
  }

  ed_w = f->w - 6; /* "| > " … " |" */
  nrows = ed_layout(&a->ed, ed_w, rows,
                    (int)(sizeof rows / sizeof rows[0]), &crow, &ccol);
  input_rows = nrows;
  if (input_rows < TUI_ED_VIEW_MIN) input_rows = TUI_ED_VIEW_MIN;
  if (input_rows > TUI_ED_VIEW_MAX) input_rows = TUI_ED_VIEW_MAX;
  sep_row = f->h - 2 - input_rows;
  if (sep_row < 2) {
    sep_row = 2;
    input_rows = f->h - 4;
    if (input_rows < 1) input_rows = 1;
  }

  render_header(a, f);
  for (y = 1; y < f->h - 1; y++) {
    d_cell(f, 0, y, th->vline, TFG_DIM, TBG_DEFAULT, 0);
    d_cell(f, f->w - 1, y, th->vline, TFG_DIM, TBG_DEFAULT, 0);
  }
  d_cell(f, 0, sep_row, th->sep_l, TFG_DIM, TBG_DEFAULT, 0);
  d_hline(f, 1, sep_row, f->w - 2, th->hline, TFG_DIM, 0);
  d_cell(f, f->w - 1, sep_row, th->sep_r, TFG_DIM, TBG_DEFAULT, 0);

  /* chat + sidebar: 27 cols on the right; when visible the chat
   * reflows at the box edge (reference layout) */
  {
    int box_w = 27, box_x = f->w - 30;
    int chat_h = sep_row - 1;
    int chat_w = f->w - 4;
    int show = a->sidebar_on && chat_h >= 4 && f->w >= 44;
    if (show) chat_w = box_x - 3 - 2;
    chat_draw(&a->chat, f, th, 2, 1, chat_w, chat_h);
    if (show) panes_draw(a, f, box_x, 1, box_w, chat_h);
  }

  /* Input editor: always at least three rows, growing to six. Longer
   * input lives in a viewport that follows the cursor; the edge marks
   * make hidden rows explicit. */
  {
    int wstart;
    int max_start = nrows - input_rows;
    int r;
    if (max_start < 0) max_start = 0;
    if (!a->ed.view_manual) {
      a->ed.view_top = crow >= input_rows ? crow - input_rows + 1 : 0;
    }
    if (a->ed.view_top < 0) a->ed.view_top = 0;
    if (a->ed.view_top > max_start) a->ed.view_top = max_start;
    a->ed.view_rows = input_rows;
    a->ed.view_total = nrows;
    wstart = a->ed.view_top;
    for (r = 0; r < input_rows; r++) {
      int ry = sep_row + 1 + r;
      int er = wstart + r;
      int tx = 2;
      if (r == 0) {
        tx += d_put(f, 2, ry, th->prompt, TFG_ACCENT, TBG_DEFAULT,
                    TA_BOLD);
        tx += d_put(f, tx, ry, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
      } else {
        tx = 4;
      }
      if (er < nrows && er < (int)(sizeof rows / sizeof rows[0])) {
        char line[1024];
        size_t n = rows[er].end - rows[er].start;
        if (n > sizeof line - 1) n = sizeof line - 1;
        memcpy(line, a->ed.buf + rows[er].start, n);
        line[n] = '\0';
        d_putn(f, 4, ry, line, ed_w, TFG_DEFAULT, TBG_DEFAULT, 0);
      }
      if (er == crow) {
        *cx = 4 + ccol;
        *cy = ry;
      }
    }
    if (wstart > 0)
      d_put(f, f->w - 2, sep_row + 1, th->ascii ? "^" : "↑",
            TFG_ACCENT, TBG_DEFAULT, TA_DIM);
    if (wstart + input_rows < nrows)
      d_put(f, f->w - 2, sep_row + input_rows, th->newmark,
            TFG_ACCENT, TBG_DEFAULT, TA_DIM);
  }

  render_status(a, f);

  if (a->help_on) {
    modal_draw_help(a, f);
    *cx = *cy = -1;
  }
  if (a->sess.active) {
    modal_draw_sessions(a, f);
    *cx = *cy = -1;
  }
  if (a->perms.active) {
    modal_draw_perms(a, f);
    *cx = *cy = -1;
  }
  if (a->confirm.active) {
    modal_draw_confirm(a, f);
    *cx = *cy = -1;
  }
}

/* ── turn lifecycle ───────────────────────────────────────────────────── */

static void start_turn(tui_app *a, const char *text) {
  chat_entry *u = chat_push(&a->chat, CHAT_USER);
  chat_entry *live;
  asngn_submit_opts opts;
  asngn_err e;
  if (u != NULL) chat_append(&a->chat, u, text, strlen(text));
  live = chat_push(&a->chat, CHAT_ASSISTANT);
  if (live == NULL) return;
  a->live_idx = (int)(a->chat.n - 1);
  a->live_tokens = 0;
  a->live_t0_ms = 0;
  a->turn_guard_stop = 0;
  memset(&opts, 0, sizeof opts);
  opts.detail = a->detail;
  e = asngn_submit(a->ses, text, &opts, cb_token, a, &a->task);
  if (e != ASNGN_OK) {
    a->task = NULL;
    a->live_idx = -1;
    if (e == ASNGN_ERR_BUSY && a->pending_n < TUI_PENDING_MAX) {
      /* drop the echoed user + live assistant entries; the retry
       * re-echoes them when the fold releases the session */
      a->chat.n--;
      free(a->chat.e[a->chat.n].text);
      a->chat.n--;
      free(a->chat.e[a->chat.n].text);
      /* the background fold owns the session for a moment: requeue at
       * the front and retry from the main loop */
      {
        char *dup = malloc(strlen(text) + 1);
        if (dup != NULL) {
          strcpy(dup, text);
          memmove(a->pending + 1, a->pending,
                  a->pending_n * sizeof a->pending[0]);
          a->pending[0] = dup;
          a->pending_n++;
          a->pending_retry_ms = now_ms() + 500;
          if (!a->fold_note) {
            a->fold_note = 1;
            chat_systemf(&a->chat, "engine is folding the session %s "
                         "message queued", a->theme.bullet);
          }
        }
      }
    } else {
      chat_systemf(&a->chat, "submit failed: %s %s %s", asngn_err_name(e),
                   a->theme.bullet, asngn_last_error(a->ctx));
    }
  } else {
    a->fold_note = 0;
  }
  a->dirty = a->structural = 1;
}

static void start_continuation(tui_app *a, int retry) {
  chat_entry *live;
  asngn_err e;
  if (a->task != NULL) return;
  live = chat_push(&a->chat, CHAT_ASSISTANT);
  if (live == NULL) return;
  a->live_idx = (int)(a->chat.n - 1);
  a->live_tokens = 0;
  a->live_t0_ms = 0;
  a->turn_guard_stop = 0;
  e = retry ? asngn_retry(a->ses, cb_token, a, &a->task)
            : asngn_more(a->ses, cb_token, a, &a->task);
  if (e != ASNGN_OK) {
    a->task = NULL;
    a->live_idx = -1;
    a->chat.n--; /* drop the empty assistant entry */
    free(a->chat.e[a->chat.n].text);
    chat_systemf(&a->chat, "%s failed: %s %s %s",
                 retry ? "/retry" : "/more", asngn_err_name(e),
                 a->theme.bullet, asngn_last_error(a->ctx));
  }
  a->dirty = a->structural = 1;
}

static void check_task(tui_app *a) {
  asngn_turn_result r;
  asngn_err e;
  if (a->task == NULL) return;
  /* poll, never park: timeout 0 means wait-until-done (the MCP server
   * relies on that), and the engine only signals the task cv on
   * completion — a 0 here froze the whole UI for every silent stretch
   * of the turn (model loads, prompt evaluation) */
  e = asngn_task_wait(a->task, 1, &r);
  if (e == ASNGN_ERR_BUSY) return;
  asngn_task_free(a->task);
  a->task = NULL;

  if (e == ASNGN_OK) {
    chat_entry *live =
        a->live_idx >= 0 ? &a->chat.e[a->live_idx] : NULL;
    if (live != NULL) {
      if (live->len == 0 && r.answer != NULL) /* cache hit: no stream */
        chat_append(&a->chat, live, r.answer, strlen(r.answer));
      live->turn = r.turn;
      if (r.capped) {
        char b[40];
        snprintf(b, sizeof b, " [capped %s /more]", a->theme.bullet);
        chat_badge(&a->chat, live, b);
      }
      if (r.clarify) chat_badge(&a->chat, live, " [clarify]");
      if (a->turn_guard_stop)
        chat_badge(&a->chat, live, " [step budget]");
    }
    a->last_turn = r.turn;
    snprintf(a->tier_last, sizeof a->tier_last, "%s", r.tier);
    asngn_turn_result_free(&r);
  } else if (e == ASNGN_ERR_CANCELLED) {
    chat_entry *live =
        a->live_idx >= 0 ? &a->chat.e[a->live_idx] : NULL;
    if (live != NULL) chat_badge(&a->chat, live, " [cancelled]");
  } else {
    chat_systemf(&a->chat, "turn failed: %s %s %s", asngn_err_name(e),
                 a->theme.bullet, asngn_last_error(a->ctx));
  }

  a->live_idx = -1;
  refresh_stats(a);
  a->dirty = a->structural = 1;

  /* locally queued messages run next */
  if (a->pending_n > 0 && !a->quit) {
    char *next = a->pending[0];
    memmove(a->pending, a->pending + 1,
            (a->pending_n - 1) * sizeof a->pending[0]);
    a->pending_n--;
    start_turn(a, next);
    free(next);
  }
}

/* ── slash commands ──────────────────────────────────────────────────── */

static int busy_guard(tui_app *a) {
  if (a->task != NULL) {
    chat_systemf(&a->chat, "busy %s Esc cancels the running turn",
                 a->theme.bullet);
    return 1;
  }
  return 0;
}

/* Open (or create: slug NULL asks the engine for a fresh generated
 * slug) a session and rebuild the chat from its transcript. The chat is
 * never lost: switching back replays the full history from disk. */
static void switch_session(tui_app *a, const char *slug) {
  asngn_session *ns = NULL;
  if (busy_guard(a)) return;
  if (asngn_session_open(a->ctx, slug, &ns) != ASNGN_OK) {
    chat_systemf(&a->chat, "session failed: %s",
                 asngn_last_error(a->ctx));
    return;
  }
  asngn_session_close(a->ses);
  a->ses = ns;
  snprintf(a->slug, sizeof a->slug, "%s", asngn_session_slug(ns));
  a->last_turn = 0;
  chat_free(&a->chat);
  chat_init(&a->chat);
  a->live_idx = -1;
  chat_systemf(&a->chat, "session %s", a->slug);
  chat_replay(a);
  refresh_stats(a);
}

/* Fill the session-picker rows (list + per-slug peek), keeping the
 * cursor on the current session. */
static void sessions_build(tui_app *a) {
  tui_sess *p = &a->sess;
  char **slugs = NULL;
  size_t n = 0, i;
  free(p->rows);
  p->rows = NULL;
  p->n = 0;
  p->confirm_del[0] = '\0';
  if (asngn_session_list(a->ctx, &slugs, &n) != ASNGN_OK) {
    snprintf(p->note, sizeof p->note, "list failed: %s",
             asngn_last_error(a->ctx));
    return;
  }
  if (n > 0) {
    p->rows = calloc(n, sizeof *p->rows);
    if (p->rows == NULL) n = 0;
  }
  for (i = 0; i < n; i++) {
    tui_sess_row *r = &p->rows[p->n];
    asngn_session_peek_info pi;
    long long ref;
    memset(&pi, 0, sizeof pi);
    snprintf(r->slug, sizeof r->slug, "%s", slugs[i]);
    r->current = strcmp(slugs[i], a->slug) == 0;
    if (r->current && a->sstats_ok) {
      /* the open session reads its own live stats: a disk peek would
       * race the appenders */
      pi.turns = a->sstats.turns;
      pi.spent_tokens = a->sstats.spent_tokens;
      pi.last_turn_at = a->stats_ok ? a->stats.last_turn_at : 0;
    } else if (asngn_session_peek(a->ctx, slugs[i], &pi) != ASNGN_OK) {
      r->unreadable = 1;
      if (r->current) p->cur = (int)p->n;
      p->n++;
      continue;
    }
    r->turns = pi.turns;
    tui_fmt_count(r->tok, sizeof r->tok, pi.spent_tokens);
    ref = pi.last_turn_at > 0 ? pi.last_turn_at : pi.created_at;
    if (ref > 0) {
      long long mins = ((long long)time(NULL) - ref) / 60;
      if (mins < 1) snprintf(r->age, sizeof r->age, "now");
      else if (mins < 60)
        snprintf(r->age, sizeof r->age, "%lldm ago", mins);
      else if (mins < 60 * 48)
        snprintf(r->age, sizeof r->age, "%lldh ago", mins / 60);
      else
        snprintf(r->age, sizeof r->age, "%lldd ago", mins / (60 * 24));
    } else if (pi.turns == 0) {
      snprintf(r->age, sizeof r->age, "empty");
    } else {
      /* live stats carry no timestamps before the first turn of this
       * process */
      snprintf(r->age, sizeof r->age, "open");
    }
    snprintf(r->project, sizeof r->project, "%s", pi.project);
    if (r->current) p->cur = (int)p->n;
    p->n++;
  }
  asngn_strings_free(slugs, n);
  if (p->cur >= (int)p->n) p->cur = (int)p->n - 1;
  if (p->cur < 0) p->cur = 0;
  if (p->top > p->cur) p->top = p->cur;
}

static void sessions_open(tui_app *a) {
  a->sess.note[0] = '\0';
  refresh_stats(a);
  sessions_build(a);
  a->sess.active = 1;
}

static void cmd_session(tui_app *a, const char *arg) {
  if (arg == NULL || strcmp(arg, "list") == 0) {
    sessions_open(a);
    return;
  }
  if (strncmp(arg, "delete", 6) == 0 &&
      (arg[6] == ' ' || arg[6] == '\0')) {
    const char *victim = arg + 6;
    while (*victim == ' ') victim++;
    if (*victim == '\0') {
      chat_systemf(&a->chat, "usage: /session delete <slug>");
      return;
    }
    if (strcmp(victim, a->slug) == 0) {
      chat_systemf(&a->chat, "cannot delete the current session %s "
                   "switch first (/session new)", a->theme.bullet);
      return;
    }
    if (asngn_session_delete(a->ctx, victim) == ASNGN_OK)
      chat_systemf(&a->chat, "session %s deleted", victim);
    else
      chat_systemf(&a->chat, "delete failed: %s",
                   asngn_last_error(a->ctx));
    return;
  }
  /* "/session new" asks the engine for a fresh generated slug */
  switch_session(a, strcmp(arg, "new") == 0 ? NULL : arg);
}

static void cmd_cache(tui_app *a, const char *arg) {
  if (arg != NULL && strcmp(arg, "clear") == 0) {
    if (asngn_cache_clear(a->ctx, NULL) == ASNGN_OK) {
      refresh_stats(a);
      chat_systemf(&a->chat, "cache cleared (session + global)");
    } else {
      chat_systemf(&a->chat, "cache clear failed: %s",
                   asngn_last_error(a->ctx));
    }
    return;
  }
  a->pane = PANE_CACHE;
  a->sidebar_on = 1;
  refresh_stats(a);
  if (a->stats_ok)
    chat_systemf(&a->chat, "cache: %zu hit %s %zu adapt %s %zu miss",
                 a->stats.cache_hits, a->theme.bullet,
                 a->stats.cache_adapts, a->theme.bullet,
                 a->stats.cache_misses);
}

/* ── tool permissions overlay (/perms) ───────────────────────────────── */

/* Re-read the registry into the overlay, keeping the cursor on the same
 * tool when it survives the refresh. */
static void perms_refresh(tui_app *a) {
  tui_perms *p = &a->perms;
  asngn_tool_info *tools = NULL;
  size_t n = 0, i;
  char keep[64];
  keep[0] = '\0';
  if (p->cur >= 0 && (size_t)p->cur < p->n)
    snprintf(keep, sizeof keep, "%s", p->tools[p->cur].ref);
  if (asngn_tool_list(a->ctx, &tools, &n) != ASNGN_OK) {
    chat_systemf(&a->chat, "tool list failed: %s",
                 asngn_last_error(a->ctx));
    return;
  }
  asngn_free(p->tools);
  p->tools = tools;
  p->n = n;
  if (keep[0] != '\0')
    for (i = 0; i < n; i++)
      if (strcmp(tools[i].ref, keep) == 0) p->cur = (int)i;
  if (p->cur >= (int)n) p->cur = (int)n - 1;
  if (p->cur < 0) p->cur = 0;
  if (p->top > p->cur) p->top = p->cur;
}

static void perms_open(tui_app *a) {
  if (!a->sib_ok || !a->sib.astools_ok) {
    chat_systemf(&a->chat, "astools disabled %s no tools to manage",
                 a->theme.bullet);
    return;
  }
  perms_refresh(a);
  a->perms.active = 1;
}

static void perms_toggle(tui_app *a) {
  tui_perms *p = &a->perms;
  char ref[64];
  int want;
  if (p->cur < 0 || (size_t)p->cur >= p->n) return;
  snprintf(ref, sizeof ref, "%s", p->tools[p->cur].ref);
  want = !p->tools[p->cur].enabled;
  if (asngn_tool_enable(a->ctx, ref, want) != ASNGN_OK) {
    chat_systemf(&a->chat, "tool %s: %s", ref,
                 asngn_last_error(a->ctx));
    return;
  }
  perms_refresh(a);
  if (want && (size_t)p->cur < p->n &&
      strcmp(p->tools[p->cur].ref, ref) == 0 &&
      !p->tools[p->cur].enabled)
    chat_systemf(&a->chat, "tool %s held disabled by pinning %s "
                 "approve its lockfile entry first", ref,
                 a->theme.bullet);
  refresh_stats(a);
}

static void cmd_export(tui_app *a) {
  char *text = NULL;
  if (busy_guard(a)) return;
  if (asngn_session_export(a->ses, &text) == ASNGN_OK) {
    chat_systemf(&a->chat,
                 "report written: sessions/%s/report.txt (+ report.xcdn)",
                 a->slug);
    if (text != NULL) {
      /* show the first few lines */
      const char *p = text;
      int lines = 0;
      const char *end = text + strlen(text);
      while (p < end && lines < 6) {
        const char *nl = strchr(p, '\n');
        size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
        if (len > 0) chat_systemf(&a->chat, "%.*s", (int)len, p);
        lines++;
        if (nl == NULL) break;
        p = nl + 1;
      }
    }
    asngn_free(text);
  } else {
    chat_systemf(&a->chat, "export failed: %s",
                 asngn_last_error(a->ctx));
  }
}

static void handle_slash(tui_app *a, char *line) {
  char *cmd = line;
  char *arg = strchr(line, ' ');
  if (arg != NULL) {
    *arg++ = '\0';
    while (*arg == ' ') arg++;
    if (*arg == '\0') arg = NULL;
  }

  if (strcmp(cmd, "/help") == 0) {
    a->help_on = 1;
  } else if (strcmp(cmd, "/quit") == 0) {
    a->quit = 1;
  } else if (strcmp(cmd, "/session") == 0) {
    cmd_session(a, arg);
  } else if (strcmp(cmd, "/project") == 0) {
    if (arg == NULL) {
      chat_systemf(&a->chat, "usage: /project <slug> | none");
    } else {
      const char *slug = strcmp(arg, "none") == 0 ? NULL : arg;
      if (asngn_session_project(a->ses, slug) == ASNGN_OK) {
        refresh_stats(a);
        chat_systemf(&a->chat, "project %s",
                     slug != NULL ? slug : "none");
      } else {
        chat_systemf(&a->chat, "project failed: %s",
                     asngn_last_error(a->ctx));
      }
    }
  } else if (strcmp(cmd, "/detail") == 0) {
    if (arg == NULL) {
      chat_systemf(&a->chat, "usage: /detail terse|normal|rich|auto");
    } else if (strcmp(arg, "terse") == 0) {
      a->detail = ASNGN_DETAIL_TERSE;
      chat_systemf(&a->chat, "detail terse");
    } else if (strcmp(arg, "normal") == 0) {
      a->detail = ASNGN_DETAIL_NORMAL;
      chat_systemf(&a->chat, "detail normal");
    } else if (strcmp(arg, "rich") == 0) {
      a->detail = ASNGN_DETAIL_RICH;
      chat_systemf(&a->chat, "detail rich");
    } else if (strcmp(arg, "auto") == 0) {
      a->detail = ASNGN_DETAIL_AUTO;
      chat_systemf(&a->chat, "detail auto");
    } else {
      chat_systemf(&a->chat, "unknown detail: %s", arg);
    }
  } else if (strcmp(cmd, "/more") == 0) {
    if (!busy_guard(a)) start_continuation(a, 0);
  } else if (strcmp(cmd, "/retry") == 0) {
    if (!busy_guard(a)) start_continuation(a, 1);
  } else if (strcmp(cmd, "/pin") == 0) {
    size_t n = arg != NULL ? (size_t)strtoul(arg, NULL, 10)
                           : a->last_turn;
    if (n == 0) {
      chat_systemf(&a->chat, "nothing to pin yet");
    } else if (asngn_session_pin(a->ses, n, 1) == ASNGN_OK) {
      chat_systemf(&a->chat, "pinned turn %zu", n);
    } else {
      chat_systemf(&a->chat, "pin failed: %s",
                   asngn_last_error(a->ctx));
    }
  } else if (strcmp(cmd, "/compact") == 0) {
    if (!busy_guard(a)) {
      if (asngn_session_compact(a->ses) == ASNGN_OK) {
        refresh_stats(a);
        chat_systemf(&a->chat, "session folded and compacted");
      } else {
        chat_systemf(&a->chat, "compact failed: %s",
                     asngn_last_error(a->ctx));
      }
    }
  } else if (strcmp(cmd, "/cache") == 0) {
    cmd_cache(a, arg);
  } else if (strcmp(cmd, "/memory") == 0) {
    if (arg == NULL) {
      chat_systemf(&a->chat, "usage: /memory <question>");
    } else {
      char *out = NULL;
      if (asngn_recall(a->ctx, arg, &out) == ASNGN_OK) {
        chat_entry *e = chat_push(&a->chat, CHAT_SYSTEM);
        if (e != NULL && out != NULL)
          chat_append(&a->chat, e, out, strlen(out));
        asngn_free(out);
      } else {
        chat_systemf(&a->chat, "recall failed: %s",
                     asngn_last_error(a->ctx));
      }
    }
  } else if (strcmp(cmd, "/tools") == 0) {
    a->pane = PANE_TOOLS;
    a->sidebar_on = 1;
  } else if (strcmp(cmd, "/perms") == 0) {
    perms_open(a);
  } else if (strcmp(cmd, "/stats") == 0) {
    a->pane = PANE_STATS;
    a->sidebar_on = 1;
  } else if (strcmp(cmd, "/export") == 0) {
    cmd_export(a);
  } else if (strcmp(cmd, "/redact") == 0) {
    int on = arg != NULL && strcmp(arg, "on") == 0;
    if (arg == NULL ||
        (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0)) {
      chat_systemf(&a->chat, "usage: /redact on|off");
    } else if (asngn_session_redact(a->ses, on) == ASNGN_OK) {
      chat_systemf(&a->chat, "redaction %s", on ? "on" : "off");
    } else {
      chat_systemf(&a->chat, "redact failed: %s",
                   asngn_last_error(a->ctx));
    }
  } else {
    chat_systemf(&a->chat, "unknown command: %s %s /help", cmd,
                 a->theme.bullet);
  }
  a->dirty = a->structural = 1;
}

/* ── Tab completion (commands, session and project slugs) ─────────────── */

static const char *const SLASH_CMDS[] = {
    "/cache", "/compact", "/detail", "/export", "/help", "/memory",
    "/more", "/perms", "/pin", "/project", "/quit", "/redact",
    "/retry", "/session", "/stats", "/tools",
};

static void complete_from(tui_app *a, const char *partial,
                          const char *const *cands, size_t nc,
                          int add_space) {
  size_t plen = strlen(partial);
  const char *match[64];
  size_t nm = 0, i;
  for (i = 0; i < nc && nm < 64; i++)
    if (strncmp(cands[i], partial, plen) == 0) match[nm++] = cands[i];
  if (nm == 0) return;
  {
    /* longest common prefix of the matches, beyond the partial */
    size_t lcp = strlen(match[0]);
    for (i = 1; i < nm; i++) {
      size_t j = 0;
      while (j < lcp && match[i][j] != '\0' &&
             match[i][j] == match[0][j])
        j++;
      lcp = j;
    }
    if (lcp > plen)
      ed_insert(&a->ed, match[0] + plen, lcp - plen);
    if (nm == 1 && add_space) ed_insert(&a->ed, " ", 1);
  }
}

static void complete(tui_app *a) {
  char buf[TUI_ED_MAX];
  char *sp;
  if (a->ed.len == 0 || a->ed.buf[0] != '/' ||
      a->ed.cur != a->ed.len)
    return;
  memcpy(buf, a->ed.buf, a->ed.len);
  buf[a->ed.len] = '\0';
  sp = strchr(buf, ' ');
  if (sp == NULL) {
    complete_from(a, buf, SLASH_CMDS,
                  sizeof SLASH_CMDS / sizeof SLASH_CMDS[0], 1);
    return;
  }
  *sp = '\0';
  {
    const char *arg = sp + 1;
    const char *last = strrchr(arg, ' ');
    if (last != NULL) arg = last + 1;
    if (strcmp(buf, "/detail") == 0) {
      static const char *const D[] = {"auto", "normal", "rich", "terse"};
      complete_from(a, arg, D, 4, 0);
    } else if (strcmp(buf, "/cache") == 0) {
      static const char *const C[] = {"clear", "stats"};
      complete_from(a, arg, C, 2, 0);
    } else if (strcmp(buf, "/redact") == 0) {
      static const char *const R[] = {"off", "on"};
      complete_from(a, arg, R, 2, 0);
    } else if (strcmp(buf, "/session") == 0 ||
               strcmp(buf, "/project") == 0) {
      int project = buf[1] == 'p';
      char **slugs = NULL;
      size_t n = 0;
      const char *cands[64];
      size_t nc = 0, i;
      asngn_err e = project
                        ? asngn_project_list(a->ctx, &slugs, &n)
                        : asngn_session_list(a->ctx, &slugs, &n);
      cands[nc++] = project ? "none" : "list";
      if (e == ASNGN_OK) {
        for (i = 0; i < n && nc < 64; i++) cands[nc++] = slugs[i];
      }
      complete_from(a, arg, cands, nc, 0);
      if (e == ASNGN_OK) asngn_strings_free(slugs, n);
    }
  }
}

/* ── key handling ─────────────────────────────────────────────────────── */

static void resolve_confirm(tui_app *a, int allow, int session_wide) {
  if (!a->confirm.active) return;
  asngn_confirm(a->ctx, a->confirm.id, allow, session_wide);
  a->confirm.active = 0;
  a->dirty = a->structural = 1;
}

static void submit_editor(tui_app *a) {
  char text[TUI_ED_MAX];
  int cx, cy;
  if (a->ed.len == 0) return;
  memcpy(text, a->ed.buf, a->ed.len);
  text[a->ed.len] = '\0';
  ed_hist_add(&a->ed, text);
  ed_clear(&a->ed);
  a->chat.scroll = 0; /* sending returns to the tail */
  a->chat.unseen = 0;

  /* Paint the cleared editor before command handling or asngn_submit:
   * submission may wait for a session fold, and the old prompt must not
   * remain visible during that wait. Cached stats are sufficient for
   * this immediate input-only frame; the regular structural repaint
   * refreshes them immediately afterwards. */
  a->dirty = a->structural = 1;
  tui_render_frame(a, &a->term.cur, &cx, &cy);
  tui_term_render(&a->term, cx, cy);
  a->last_render_ms = now_ms();
  a->dirty = a->structural = 0;

  if (text[0] == '/') {
    handle_slash(a, text);
    return;
  }
  if (a->task != NULL) {
    if (a->pending_n < TUI_PENDING_MAX) {
      char *dup = malloc(strlen(text) + 1);
      if (dup != NULL) {
        strcpy(dup, text);
        a->pending[a->pending_n++] = dup;
        chat_systemf(&a->chat, "queued (%zu waiting)", a->pending_n);
      }
    } else {
      chat_systemf(&a->chat, "queue full %s wait for the turn",
                   a->theme.bullet);
    }
    return;
  }
  start_turn(a, text);
}

static void rate_last(tui_app *a, int signal) {
  if (a->last_turn == 0) {
    chat_systemf(&a->chat, "no answer to rate yet");
    return;
  }
  if (asngn_feedback(a->ses, a->last_turn, signal) == ASNGN_OK)
    chat_systemf(&a->chat, "rated %s (turn %zu)",
                 signal > 0 ? a->theme.check : a->theme.cross,
                 a->last_turn);
  else
    chat_systemf(&a->chat, "feedback failed: %s",
                 asngn_last_error(a->ctx));
}

static void handle_key(tui_app *a, const tui_key *k) {
  a->dirty = 1;

  if (a->confirm.active) { /* y / n / a / Esc */
    if (k->kind == TK_CHAR) {
      char c = k->utf8[0];
      if (c == 'y' || c == 'Y') resolve_confirm(a, 1, 0);
      else if (c == 'n' || c == 'N') resolve_confirm(a, 0, 0);
      else if (c == 'a' || c == 'A') resolve_confirm(a, 1, 1);
    } else if (k->kind == TK_ESC || k->kind == TK_CTRL_C ||
               k->kind == TK_CTRL_D) {
      resolve_confirm(a, 0, 0); /* escape hatches deny */
    }
    return;
  }
  if (a->sess.active) { /* arrows move, Enter switches, n new, d d deletes */
    tui_sess *p = &a->sess;
    switch (k->kind) {
    case TK_UP:
      if (p->cur > 0) p->cur--;
      p->confirm_del[0] = p->note[0] = '\0';
      break;
    case TK_DOWN:
      if (p->cur + 1 < (int)p->n) p->cur++;
      p->confirm_del[0] = p->note[0] = '\0';
      break;
    case TK_HOME:
      p->cur = 0;
      p->confirm_del[0] = p->note[0] = '\0';
      break;
    case TK_END:
      if (p->n > 0) p->cur = (int)p->n - 1;
      p->confirm_del[0] = p->note[0] = '\0';
      break;
    case TK_ENTER:
      if (p->cur >= 0 && (size_t)p->cur < p->n &&
          !p->rows[p->cur].current) {
        char slug[72];
        snprintf(slug, sizeof slug, "%s", p->rows[p->cur].slug);
        p->active = 0;
        switch_session(a, slug);
      } else {
        p->active = 0;
      }
      break;
    case TK_CHAR:
      if (k->utf8[0] == 'n') {
        p->active = 0;
        switch_session(a, NULL);
      } else if (k->utf8[0] == 'd') {
        if (p->cur >= 0 && (size_t)p->cur < p->n) {
          tui_sess_row *r = &p->rows[p->cur];
          if (r->current) {
            snprintf(p->note, sizeof p->note,
                     "cannot delete the current session %s switch first",
                     a->theme.bullet);
          } else if (strcmp(p->confirm_del, r->slug) == 0) {
            char victim[72];
            snprintf(victim, sizeof victim, "%s", r->slug);
            if (asngn_session_delete(a->ctx, victim) == ASNGN_OK) {
              sessions_build(a);
              snprintf(p->note, sizeof p->note, "session %s deleted",
                       victim);
            } else {
              p->confirm_del[0] = '\0';
              snprintf(p->note, sizeof p->note, "delete failed: %s",
                       asngn_last_error(a->ctx));
            }
          } else {
            snprintf(p->confirm_del, sizeof p->confirm_del, "%s",
                     r->slug);
            snprintf(p->note, sizeof p->note,
                     "d again deletes %.64s (irreversible)", r->slug);
          }
        }
      } else if (k->utf8[0] == 'q') {
        p->active = 0;
      }
      break;
    case TK_ESC:
    case TK_CTRL_C:
    case TK_CTRL_D:
      p->active = 0;
      break;
    default:
      break;
    }
    a->structural = 1;
    return;
  }
  if (a->perms.active) { /* arrows move, Space/Enter toggles, Esc closes */
    switch (k->kind) {
    case TK_UP:
      if (a->perms.cur > 0) a->perms.cur--;
      break;
    case TK_DOWN:
      if (a->perms.cur + 1 < (int)a->perms.n) a->perms.cur++;
      break;
    case TK_HOME:
      a->perms.cur = 0;
      break;
    case TK_END:
      if (a->perms.n > 0) a->perms.cur = (int)a->perms.n - 1;
      break;
    case TK_ENTER:
      perms_toggle(a);
      break;
    case TK_CHAR:
      if (k->utf8[0] == ' ') perms_toggle(a);
      else if (k->utf8[0] == 'q') a->perms.active = 0;
      break;
    case TK_ESC:
    case TK_CTRL_C:
    case TK_CTRL_D:
      a->perms.active = 0;
      break;
    default:
      break;
    }
    a->structural = 1;
    return;
  }
  if (a->help_on) {
    if (k->kind == TK_ESC || k->kind == TK_F1 || k->kind == TK_CTRL_C ||
        k->kind == TK_CTRL_D ||
        (k->kind == TK_CHAR && k->utf8[0] == 'q'))
      a->help_on = 0;
    return;
  }

  switch (k->kind) {
  case TK_CHAR:
    ed_insert(&a->ed, k->utf8, strlen(k->utf8));
    break;
  case TK_ENTER:
    submit_editor(a);
    a->structural = 1;
    break;
  case TK_NEWLINE: /* Alt+Enter / Ctrl+J */
    ed_insert(&a->ed, "\n", 1);
    a->structural = 1;
    break;
  case TK_ESC:
    if (a->task != NULL) asngn_task_cancel(a->task);
    break;
  case TK_CTRL_C:
    if (a->task != NULL) asngn_task_cancel(a->task);
    else a->quit = 1;
    break;
  case TK_CTRL_D:
    if (a->ed.len == 0) a->quit = 1;
    else ed_delete(&a->ed);
    break;
  case TK_TAB:
    if (a->ed.len > 0 && a->ed.buf[0] == '/') {
      complete(a);
    } else if (!a->sidebar_on) {
      a->sidebar_on = 1; /* narrow screens: Tab overlays the sidebar */
      a->structural = 1;
    } else if (a->pane + 1 < PANE_COUNT) {
      a->pane++;
      a->structural = 1;
    } else {
      a->pane = PANE_TRACE;
      a->sidebar_on = 0; /* cycled past cache: hide */
      a->structural = 1;
    }
    break;
  case TK_F1: a->help_on = 1; break;
  case TK_F2: case TK_F3: case TK_F4: case TK_F5: case TK_F6:
    a->pane = k->kind - TK_F2;
    a->sidebar_on = 1;
    a->structural = 1;
    break;
  case TK_F7: rate_last(a, 1); break;
  case TK_F8: rate_last(a, -1); break;
  case TK_PGUP:
    chat_scroll_by(&a->chat,
                   a->chat.last_h > 1 ? a->chat.last_h - 1 : 10);
    a->structural = 1;
    break;
  case TK_PGDN:
    chat_scroll_by(&a->chat,
                   -(a->chat.last_h > 1 ? a->chat.last_h - 1 : 10));
    a->structural = 1;
    break;
  case TK_ALT_UP:
    ed_view_scroll(&a->ed, -1);
    a->structural = 1;
    break;
  case TK_ALT_DOWN:
    ed_view_scroll(&a->ed, 1);
    a->structural = 1;
    break;
  case TK_ALT_PGUP:
    ed_view_scroll(&a->ed,
                   -(a->ed.view_rows > 1 ? a->ed.view_rows - 1 : 1));
    a->structural = 1;
    break;
  case TK_ALT_PGDN:
    ed_view_scroll(&a->ed,
                   a->ed.view_rows > 1 ? a->ed.view_rows - 1 : 1);
    a->structural = 1;
    break;
  case TK_UP:
    if (a->ed.hist_pos >= 0) {
      ed_hist_up(&a->ed);
    } else {
      chat_scroll_by(&a->chat, 1);
      a->structural = 1;
    }
    break;
  case TK_DOWN:
    if (a->ed.hist_pos >= 0) {
      ed_hist_down(&a->ed);
    } else {
      chat_scroll_by(&a->chat, -1);
      a->structural = 1;
    }
    break;
  case TK_LEFT: ed_left(&a->ed); break;
  case TK_RIGHT: ed_right(&a->ed); break;
  case TK_HOME: ed_home(&a->ed); break;
  case TK_END: ed_end(&a->ed); break;
  case TK_BACKSPACE: ed_backspace(&a->ed); break;
  case TK_DELETE: ed_delete(&a->ed); break;
  case TK_CTRL_U: ed_kill_to_start(&a->ed); break;
  case TK_CTRL_K: ed_kill_to_end(&a->ed); break;
  case TK_CTRL_W: ed_kill_word(&a->ed); break;
  case TK_CTRL_Y: ed_yank(&a->ed); break;
  case TK_CTRL_P: ed_hist_up(&a->ed); break;
  case TK_CTRL_N: ed_hist_down(&a->ed); break;
  default: break;
  }
}

/* ── message drain ────────────────────────────────────────────────────── */

static void drain_queue(tui_app *a) {
  tui_msg *m = tui_queue_drain(&a->q);
  while (m != NULL) {
    tui_msg *next = m->next;
    switch (m->kind) {
    case TMSG_TOKEN:
      if (a->live_idx >= 0)
        chat_append(&a->chat, &a->chat.e[a->live_idx], m->text,
                    strlen(m->text));
      if (a->live_tokens == 0) a->live_t0_ms = now_ms();
      a->live_tokens++;
      a->dirty = 1;
      break;
    case TMSG_EVENT:
      tui_events_ingest(a, m->text);
      break;
    default:
      tui_events_log(a, m->level, m->text);
      break;
    }
    free(m);
    m = next;
  }
}

/* ── welcome line ─────────────────────────────────────────────────────── */

static void welcome(tui_app *a) {
  chat_systemf(&a->chat,
               "asngn %s %s per aspera ad astra %s /help for commands",
               asngn_version(), a->theme.star,
               a->theme.ascii ? "-" : "\xE2\x80\x94");
}

/* ── --frame-dump: golden-frame hook ─────────────────────────────────── */

static int run_frame_dump(void) {
  tui_app a;
  tui_frame f;
  int cx, cy, x, y;
  memset(&a, 0, sizeof a);
  a.live_idx = -1;
  a.detail = ASNGN_DETAIL_AUTO;
  a.sidebar_on = 1;
  a.pane = PANE_TRACE;
  tui_theme_init(&a.theme, 0); /* golden frames are stable: asterism */
  snprintf(a.slug, sizeof a.slug, "main");
  chat_init(&a.chat);
  ed_init(&a.ed);
  tui_queue_init(&a.q, NULL);
  welcome(&a);

  if (tui_frame_alloc(&f, 80, 24) != 0) return 1;
  tui_render_frame(&a, &f, &cx, &cy);

  for (y = 0; y < f.h; y++) {
    char line[80 * 4 + 1];
    size_t n = 0;
    for (x = 0; x < f.w; x++) {
      const char *g = f.cells[y * f.w + x].utf8;
      size_t l = strlen(g[0] != '\0' ? g : " ");
      if (n + l < sizeof line) {
        memcpy(line + n, g[0] != '\0' ? g : " ", l);
        n += l;
      }
    }
    while (n > 0 && line[n - 1] == ' ') n--; /* trim */
    line[n] = '\0';
    puts(line);
  }
  tui_frame_free(&f);
  chat_free(&a.chat);
  ed_free(&a.ed);
  tui_queue_free(&a.q);
  return 0;
}

/* ── headless --once mode ────────────────────────────────────────────── */

typedef struct {
  size_t bytes;
} once_stream;

static void cb_token_stdout(const char *utf8, void *ud) {
  once_stream *s = ud;
  size_t n = strlen(utf8);
  fwrite(utf8, 1, n, stdout);
  fflush(stdout);
  s->bytes += n;
}

static char *read_stdin_all(void) {
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (buf == NULL) return NULL;
  for (;;) {
    size_t got;
    if (len + 1024 + 1 > cap) {
      char *p = realloc(buf, cap *= 2);
      if (p == NULL) {
        free(buf);
        return NULL;
      }
      buf = p;
    }
    got = fread(buf + len, 1, 1024, stdin);
    len += got;
    if (got < 1024) break;
  }
  buf[len] = '\0';
  return buf;
}

static int run_once(const char *root, const char *config,
                    const char *workspace, int allow_degraded,
                    const char *session, asngn_detail detail,
                    int confirm_override, const char *message) {
  asngn_open_params p;
  asngn_ctx *c = NULL;
  asngn_session *s = NULL;
  asngn_task *t = NULL;
  asngn_turn_result r;
  asngn_submit_opts opts;
  once_stream stream;
  asngn_err e;
  char *stdin_msg = NULL;

  if (strcmp(message, "-") == 0) {
    stdin_msg = read_stdin_all();
    if (stdin_msg == NULL || stdin_msg[0] == '\0') {
      fprintf(stderr, "asngn: empty message on stdin\n");
      free(stdin_msg);
      return 1;
    }
    message = stdin_msg;
  }

  memset(&p, 0, sizeof p);
  p.engine_root = root;
  p.config_path = config;
  p.workspace_root = workspace;
  p.allow_degraded = allow_degraded;
  e = asngn_open(&p, &c);
  if (e != ASNGN_OK) {
    fprintf(stderr, "asngn: open failed: %s\n", asngn_err_name(e));
    free(stdin_msg);
    return 1;
  }
  /* INTERNAL-HEADER EXCEPTION (see the file comment): headless
   * autoconfirm defaults to "deny"; --confirm allow|deny is
   * honored, --confirm prompt cannot prompt here and stays deny. */
  c->cfg.autoconfirm = confirm_override == (int)ASNGN_CONFIRM_ALLOW
                           ? ASNGN_CONFIRM_ALLOW
                           : ASNGN_CONFIRM_DENY;

  e = asngn_session_open(c, session != NULL ? session : "main", &s);
  if (e != ASNGN_OK) {
    fprintf(stderr, "asngn: %s: %s\n", asngn_err_name(e),
            asngn_last_error(c));
    asngn_close(c);
    free(stdin_msg);
    return 1;
  }

  memset(&opts, 0, sizeof opts);
  memset(&r, 0, sizeof r); /* only read under e == ASNGN_OK, but flow
                            * analysis cannot see that (C4701) */
  opts.detail = detail;
  stream.bytes = 0;
  e = asngn_submit(s, message, &opts, cb_token_stdout, &stream, &t);
  if (e == ASNGN_OK)
    while ((e = asngn_task_wait(t, 250, &r)) == ASNGN_ERR_BUSY) {
      /* stream in flight */
    }
  if (e == ASNGN_OK) {
    if (stream.bytes == 0 && r.answer != NULL) /* e.g. cache hit */
      fwrite(r.answer, 1, strlen(r.answer), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    asngn_turn_result_free(&r);
  } else {
    fprintf(stderr, "asngn: %s: %s\n", asngn_err_name(e),
            asngn_last_error(c));
  }
  asngn_task_free(t);
  asngn_session_close(s);
  asngn_close(c);
  free(stdin_msg);
  return e == ASNGN_OK ? 0 : 1;
}

/* ── interactive mode ─────────────────────────────────────────────────── */

static int run_interactive(const char *root, const char *config,
                           const char *workspace, int allow_degraded,
                           const char *session, asngn_detail detail,
                           int confirm_override) {
  asngn_open_params p;
  tui_app a;
  asngn_err e;
  int theme_plain, truecolor_hint = -1;

  /* refuse pipes before the engine spins up */
  {
    const char *tm = getenv("TERM");
    if (!asngn_tty_in() || !asngn_tty_out() ||
        (tm != NULL && strcmp(tm, "dumb") == 0)) {
      fprintf(stderr,
              "asngn: the interactive TUI needs a terminal "
              "(TERM=dumb or output is not a tty).\n"
              "       try: asngn --once \"your message\"\n");
      return 1;
    }
  }

  memset(&a, 0, sizeof a);
  a.live_idx = -1;
  a.detail = detail;
  a.sidebar_on = 1;
  a.pane = PANE_TRACE;
  chat_init(&a.chat);
  ed_init(&a.ed);

  memset(&p, 0, sizeof p);
  p.engine_root = root;
  p.config_path = config;
  p.workspace_root = workspace;
  p.allow_degraded = allow_degraded;
  e = asngn_open(&p, &a.ctx);
  if (e != ASNGN_OK) {
    fprintf(stderr, "asngn: open failed: %s\n", asngn_err_name(e));
    return 1;
  }

  /* INTERNAL-HEADER EXCEPTION (see the file comment): the --confirm
   * override plus a read-only snapshot of TUI/budget config. */
  if (confirm_override >= 0)
    a.ctx->cfg.autoconfirm = (asngn_confirm_mode)confirm_override;
  theme_plain = a.ctx->cfg.theme == ASNGN_THEME_PLAIN;
  if (a.ctx->cfg.truecolor == ASNGN_TRUECOLOR_ON) truecolor_hint = 1;
  if (a.ctx->cfg.truecolor == ASNGN_TRUECOLOR_OFF) truecolor_hint = 0;
  a.budget_session = a.ctx->cfg.session_tokens;
  a.warn_at = a.ctx->cfg.warn_at;
  {
    static const char *const NAMES[PANE_COUNT] = {
        "trace", "stats", "memory", "tools", "cache"};
    int i;
    for (i = 0; i < PANE_COUNT; i++)
      if (strcmp(a.ctx->cfg.sidebar, NAMES[i]) == 0) a.pane = i;
  }
  if (!tui_locale_utf8()) theme_plain = 1; /* ASCII fallback */
  tui_theme_init(&a.theme, theme_plain);

  e = asngn_session_open(a.ctx, session != NULL ? session : "main",
                         &a.ses);
  if (e != ASNGN_OK) {
    fprintf(stderr, "asngn: %s: %s\n", asngn_err_name(e),
            asngn_last_error(a.ctx));
    asngn_close(a.ctx);
    return 1;
  }
  snprintf(a.slug, sizeof a.slug, "%s", asngn_session_slug(a.ses));

  if (tui_term_open(&a.term, truecolor_hint) != 0) {
    asngn_session_close(a.ses);
    asngn_close(a.ctx);
    return 1;
  }
  tui_queue_init(&a.q, &a.term);
  asngn_set_event_sink(a.ctx, cb_event, &a);
  asngn_set_logger(a.ctx, cb_log, &a); /* keep stderr clean in the TUI */

  refresh_stats(&a);
  welcome(&a);
  chat_replay(&a); /* resumed sessions show their full history */
  a.dirty = a.structural = 1;

  while (!a.quit) {
    long long now = now_ms();
    int sr = 0, wr = 0;

    if (tui_term_quit_requested()) break;

    if (a.task != NULL || a.now_what[0] != '\0') {
      int sp = (int)((now / 90) % a.theme.spinner_n);
      if (sp != a.spin) {
        a.spin = sp;
        a.dirty = 1;
      }
      a.dirty = 1; /* the live-phase elapsed counter ticks */
    }

    /* ~60 fps cap: coalesce token floods into one repaint */
    if (a.dirty &&
        (a.structural || now - a.last_render_ms >= 16)) {
      int cx, cy;
      if (a.structural) refresh_stats(&a);
      tui_render_frame(&a, &a.term.cur, &cx, &cy);
      tui_term_render(&a.term, cx, cy);
      a.last_render_ms = now;
      a.dirty = a.structural = 0;
    }

    {
      int timeout = a.task != NULL || a.confirm.active ||
                            a.now_what[0] != '\0'
                        ? 60
                        : 300;
      if (a.dirty) {
        /* a repaint was deferred by the fps cap above: wake as soon
         * as the 16 ms frame budget elapses, not a full idle sleep */
        long long left = 16 - (now - a.last_render_ms);
        if (left < 1) left = 1;
        if ((long long)timeout > left) timeout = (int)left;
      }
      if (tui_term_poll(&a.term, timeout, &sr, &wr) < 0) break;
    }
    if (wr && tui_term_drain_wake(&a.term)) {
      tui_term_invalidate(&a.term);
      a.dirty = a.structural = 1;
    }
    drain_queue(&a);
    if (sr) {
      tui_key keys[64];
      int nk, i;
      /* drain: bursts (pastes) can exceed 64 keys per call, and the
       * carry buffer may hold a reassembled sequence — loop until the
       * decoder runs dry so no input is dropped */
      do {
        nk = tui_term_read_keys(&a.term, keys, 64);
        for (i = 0; i < nk && !a.quit; i++) handle_key(&a, &keys[i]);
      } while (nk > 0 && !a.quit);
    }
    if (a.term.in_hup) a.quit = 1; /* stdin EOF/hangup: shut down */
    check_task(&a);
    /* messages queued while the engine folded: retry once it settles */
    if (a.task == NULL && a.pending_n > 0 && !a.quit &&
        now_ms() >= a.pending_retry_ms) {
      char *next = a.pending[0];
      memmove(a.pending, a.pending + 1,
              (a.pending_n - 1) * sizeof a.pending[0]);
      a.pending_n--;
      start_turn(&a, next);
      free(next);
    }
  }

  /* clean shutdown: cancel, close, restore (task list) */
  if (a.task != NULL) {
    asngn_turn_result r;
    asngn_task_cancel(a.task);
    if (asngn_task_wait(a.task, 3000, &r) == ASNGN_OK)
      asngn_turn_result_free(&r);
    asngn_task_free(a.task);
    a.task = NULL;
  }
  asngn_set_event_sink(a.ctx, NULL, NULL);
  asngn_set_logger(a.ctx, NULL, NULL);
  asngn_session_close(a.ses);
  asngn_close(a.ctx);
  tui_term_close(&a.term);
  tui_queue_free(&a.q);
  {
    size_t i;
    for (i = 0; i < a.pending_n; i++) free(a.pending[i]);
  }
  asngn_free(a.perms.tools);
  free(a.sess.rows);
  chat_free(&a.chat);
  ed_free(&a.ed);
  return 0;
}

/* ── argument parsing ─────────────────────────────────────────────────── */

static void usage(FILE *out) {
  fprintf(out,
          "usage: asngn [--root <dir>] [--config <file>] "
          "[--workspace <dir>] [--allow-degraded] "
          "[--session <slug>]\n"
          "             [--detail terse|normal|rich|auto] "
          "[--confirm prompt|deny|allow]\n"
          "             [--once \"<message>\"] [--version] [--help]\n"
          "\n"
          "  --once <message>   run one turn headless and print the "
          "answer (\"-\" reads stdin)\n"
          "  --root <dir>       override engine root (default: ~/asngn)\n"
          "  --workspace <dir>  use an external workspace instead of the "
          "session workspace\n"
          "  --detail <level>   force the answer detail level\n"
          "  --confirm <mode>   destructive-tool confirmation policy "
          "(TUI default: prompt;\n"
          "                     --once default: deny)\n");
}

static const char *arg_value(int argc, char **argv, int *i,
                             const char *name) {
  const char *s = argv[*i];
  size_t n = strlen(name);
  if (strncmp(s, name, n) == 0 && s[n] == '=') return s + n + 1;
  if (strcmp(s, name) == 0 && *i + 1 < argc) {
    (*i)++;
    return argv[*i];
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *root = NULL;
  const char *config = NULL;
  const char *workspace = NULL;
  const char *session = NULL;
  const char *once = NULL;
  asngn_detail detail = ASNGN_DETAIL_AUTO;
  int confirm_override = -1;
  int frame_dump = 0;
  int allow_degraded = 0;
  int i;

  for (i = 1; i < argc; i++) {
    const char *v;
    if ((v = arg_value(argc, argv, &i, "--root")) != NULL) {
      root = v;
    } else if ((v = arg_value(argc, argv, &i, "--config")) != NULL) {
      config = v;
    } else if ((v = arg_value(argc, argv, &i, "--workspace")) != NULL) {
      workspace = v;
    } else if ((v = arg_value(argc, argv, &i, "--session")) != NULL) {
      session = v;
    } else if ((v = arg_value(argc, argv, &i, "--detail")) != NULL) {
      if (strcmp(v, "terse") == 0) detail = ASNGN_DETAIL_TERSE;
      else if (strcmp(v, "normal") == 0) detail = ASNGN_DETAIL_NORMAL;
      else if (strcmp(v, "rich") == 0) detail = ASNGN_DETAIL_RICH;
      else if (strcmp(v, "auto") == 0) detail = ASNGN_DETAIL_AUTO;
      else {
        fprintf(stderr, "asngn: unknown detail: %s\n", v);
        return 2;
      }
    } else if ((v = arg_value(argc, argv, &i, "--confirm")) != NULL) {
      if (strcmp(v, "prompt") == 0)
        confirm_override = (int)ASNGN_CONFIRM_PROMPT;
      else if (strcmp(v, "deny") == 0)
        confirm_override = (int)ASNGN_CONFIRM_DENY;
      else if (strcmp(v, "allow") == 0)
        confirm_override = (int)ASNGN_CONFIRM_ALLOW;
      else {
        fprintf(stderr, "asngn: unknown confirm mode: %s\n", v);
        return 2;
      }
    } else if ((v = arg_value(argc, argv, &i, "--once")) != NULL) {
      once = v;
    } else if (strcmp(argv[i], "--allow-degraded") == 0) {
      allow_degraded = 1;
    } else if (strcmp(argv[i], "--frame-dump") == 0) {
      frame_dump = 1; /* hidden: golden-frame hook */
    } else if (strcmp(argv[i], "--version") == 0) {
      printf("asngn %s\n", asngn_version());
      return 0;
    } else if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "asngn: unknown argument: %s\n", argv[i]);
      usage(stderr);
      return 2;
    }
  }

  if (frame_dump) return run_frame_dump();
  if (once != NULL)
    return run_once(root, config, workspace, allow_degraded,
                    session, detail, confirm_override,
                    once);
  return run_interactive(root, config, workspace, allow_degraded,
                         session, detail,
                         confirm_override);
}
