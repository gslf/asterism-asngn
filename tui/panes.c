/*
 * panes.c — the five sidebar panes of the asngn TUI and the
 * telemetry ingest that feeds them.
 *
 * Each #asngn_event xCDN line is parsed with xcdn on the main
 * thread (the engine callback only queued the text) and folded
 * into a rolling ring of 256 events plus a handful of app counters.
 * The trace pane shows the current turn (reset at turn_start); tools
 * and cache read the whole ring. Unknown kinds are tolerated.
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* monotonic ms for the live-phase clock (mirrors main.c's now_ms) */
static long long ev_now_ms(void) { return tui_now_ms(); }

#include "xcdn.h"

/* ── xcdn field helpers ───────────────────────────────────────────────── */

static const xcdn_value_t *field(const xcdn_value_t *obj, const char *k) {
  xcdn_node_t *n;
  if (obj == NULL || obj->type != XCDN_VAL_OBJECT) return NULL;
  n = xcdn_object_get(obj, k);
  return n != NULL ? n->value : NULL;
}

static const char *js(const xcdn_value_t *obj, const char *k,
                      const char *dflt) {
  const xcdn_value_t *v = field(obj, k);
  const char *s = v != NULL ? xcdn_value_as_string(v) : NULL;
  return s != NULL ? s : dflt;
}

static long long ji(const xcdn_value_t *obj, const char *k,
                    long long dflt) {
  const xcdn_value_t *v = field(obj, k);
  if (v == NULL) return dflt;
  if (v->type == XCDN_VAL_INT) return (long long)xcdn_value_as_int(v);
  if (v->type == XCDN_VAL_FLOAT) return (long long)xcdn_value_as_float(v);
  return dflt;
}

static double jf(const xcdn_value_t *obj, const char *k, double dflt) {
  const xcdn_value_t *v = field(obj, k);
  if (v == NULL) return dflt;
  if (v->type == XCDN_VAL_FLOAT) return xcdn_value_as_float(v);
  if (v->type == XCDN_VAL_INT) return (double)xcdn_value_as_int(v);
  return dflt;
}

static int jb(const xcdn_value_t *obj, const char *k, int dflt) {
  const xcdn_value_t *v = field(obj, k);
  if (v == NULL || v->type != XCDN_VAL_BOOL) return dflt;
  return xcdn_value_as_bool(v) ? 1 : 0;
}

/* ── event ring ───────────────────────────────────────────────────────── */

static tui_ev *ev_push(tui_evring *r) {
  tui_ev *e = &r->ev[r->seq % TUI_EV_CAP];
  memset(e, 0, sizeof *e);
  e->ok = -1;
  e->cos = -1.0;
  r->seq++;
  return e;
}

static const tui_ev *ev_at(const tui_evring *r, uint64_t seq) {
  if (seq >= r->seq || r->seq - seq > TUI_EV_CAP) return NULL;
  return &r->ev[seq % TUI_EV_CAP];
}

static void spark_push(tui_app *a, int v) {
  if (a->spark_n == TUI_SPARK_N) {
    memmove(a->spark, a->spark + 1,
            (TUI_SPARK_N - 1) * sizeof a->spark[0]);
    a->spark_n--;
  }
  a->spark[a->spark_n++] = v;
}

/* ── ingest ───────────────────────────────────────────────────────────── */

void tui_events_ingest(tui_app *a, const char *line_xcdn) {
  xcdn_error_t err;
  xcdn_document_t *doc;
  const xcdn_node_t *node;
  const xcdn_value_t *obj = NULL, *data;
  const char *kind;

  doc = xcdn_parse(line_xcdn, &err);
  if (doc == NULL) {
    /* a dropped event can be one the engine is blocked on (confirm),
     * so a malformed line gets a visible row instead of silence */
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "error");
    snprintf(e->name, sizeof e->name, "bad event");
    snprintf(e->extra, sizeof e->extra, "%.31s",
             xcdn_error_kind_str(err.kind));
    e->red = 1;
    a->dirty = 1;
    a->structural = 1;
    return;
  }
  node = xcdn_document_get(doc, 0);
  if (node != NULL && node->value != NULL &&
      node->value->type == XCDN_VAL_OBJECT)
    obj = node->value;
  kind = js(obj, "kind", NULL);
  data = field(obj, "data");
  if (kind == NULL) {
    xcdn_document_free(doc);
    return;
  }

  a->dirty = 1;
  a->structural = 1;

  if (strcmp(kind, "turn_start") == 0) {
    a->evs.turn_seq = a->evs.seq;
    a->turn_tokens = 0;
    a->turn_guard_stop = 0;
  } else if (strcmp(kind, "classify") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "classify");
    snprintf(e->tier, sizeof e->tier, "%.11s", js(data, "class", ""));
    snprintf(e->extra, sizeof e->extra, "%.15s", js(data, "source", ""));
  } else if (strcmp(kind, "route") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    if (jb(data, "escalated", 0)) {
      snprintf(e->name, sizeof e->name, "route");
      snprintf(e->extra, sizeof e->extra, "escalated");
      e->amber = 1;
    } else {
      snprintf(e->name, sizeof e->name, "route");
    }
  } else if (strcmp(kind, "cache_probe") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "cache");
    snprintf(e->tier, sizeof e->tier, "%.11s", js(data, "outcome", "?"));
    e->cos = jf(data, "cos", -1.0);
    if (e->cos >= 0.0)
      snprintf(e->extra, sizeof e->extra, "cos %.2f", e->cos);
  } else if (strcmp(kind, "model_call") == 0) {
    long long ti = ji(data, "tokens_in", 0);
    long long to = ji(data, "tokens_out", 0);
    long long ms = ji(data, "ms", 0);
    const char *task = js(data, "task", "call");
    tui_ev *e = ev_push(&a->evs);
    char msb[12];
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "%.16s", task);
    snprintf(e->tier, sizeof e->tier, "%.11s", js(data, "model", "?"));
    tui_fmt_ms(msb, sizeof msb, ms);
    if (strcmp(task, "answer") == 0) {
      char tb[8];
      double tps = jf(data, "tps", -1.0);
      tui_fmt_count(tb, sizeof tb, to);
      snprintf(e->extra, sizeof e->extra, "%s %s %s tok", msb,
               a->theme.bullet, tb);
      if (tps > 0.0) a->tps_last = tps; /* status-line speed */
    } else {
      snprintf(e->extra, sizeof e->extra, "%s", msb);
    }
    a->turn_tokens += ti + to;
    if (strcmp(task, "answer") != 0) a->aux_tokens += ti + to;
    a->now_what[0] = '\0'; /* phase done */
  } else if (strcmp(kind, "tool_call") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    if (jb(data, "cached", 0)) {
      snprintf(e->name, sizeof e->name, "tool cache");
      snprintf(e->extra, sizeof e->extra, "hit");
      e->ok = 1;
    } else {
      char msb[12];
      snprintf(e->name, sizeof e->name, "%.16s.%.16s",
               js(data, "tool", "tool"), js(data, "command", "?"));
      tui_fmt_ms(msb, sizeof msb, ji(data, "ms", 0));
      snprintf(e->extra, sizeof e->extra, "%s", msb);
      e->ok = jb(data, "ok", 0);
      if (!e->ok) e->red = 1;
    }
    snprintf(e->tier, sizeof e->tier, "tool");
    a->now_what[0] = '\0'; /* phase done */
  } else if (strcmp(kind, "recall") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "recall");
    snprintf(e->tier, sizeof e->tier, "asper");
  } else if (strcmp(kind, "fold") == 0 || strcmp(kind, "digest") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "%s", kind);
  } else if (strcmp(kind, "judge") == 0) {
    long long score = ji(data, "score", -1);
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    if (score >= 0)
      snprintf(e->name, sizeof e->name, "judge %lld/10", score);
    else
      snprintf(e->name, sizeof e->name, "judge");
    snprintf(e->tier, sizeof e->tier, "judge");
  } else if (strcmp(kind, "guard") == 0) {
    const char *g = js(data, "guard", "?");
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "guard: %.24s", g);
    e->amber = 1;
    if (strcmp(g, "step_budget") == 0 || strcmp(g, "tool_cap") == 0 ||
        strcmp(g, "oscillation") == 0)
      a->turn_guard_stop = 1;
  } else if (strcmp(kind, "confirm") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "confirm");
    snprintf(e->tier, sizeof e->tier, "%.11s", js(data, "tool", ""));
    /* raise the modal; keys resolve it via asngn_confirm */
    snprintf(a->confirm.id, sizeof a->confirm.id, "%s",
             js(data, "confirm_id", ""));
    snprintf(a->confirm.tool, sizeof a->confirm.tool, "%s",
             js(data, "tool", "?"));
    snprintf(a->confirm.cmd, sizeof a->confirm.cmd, "%s",
             js(data, "command", "?"));
    snprintf(a->confirm.args, sizeof a->confirm.args, "%s",
             js(data, "args", "{}"));
    a->confirm.destructive = jb(data, "destructive", 0);
    a->confirm.active = a->confirm.id[0] != '\0';
  } else if (strcmp(kind, "phase") == 0) {
    /* live "now" marker — no trace row of its own; the matching
     * completion event (model_call / tool_call / fold) makes the row */
    const char *what = js(data, "what", "");
    if (strcmp(what, "generate") == 0)
      snprintf(a->now_what, sizeof a->now_what, "%.16s @ %.16s",
               js(data, "task", "?"), js(data, "model", "?"));
    else if (strcmp(what, "load") == 0)
      snprintf(a->now_what, sizeof a->now_what, "load %.16s",
               js(data, "model", "?"));
    else if (strcmp(what, "tool") == 0)
      snprintf(a->now_what, sizeof a->now_what, "%.16s.%.16s",
               js(data, "tool", "?"), js(data, "command", "?"));
    else
      a->now_what[0] = '\0'; /* "idle" and anything unknown */
    a->now_t0_ms = ev_now_ms();
  } else if (strcmp(kind, "answer") == 0) {
    /* the answering model_call already traced; nothing extra */
  } else if (strcmp(kind, "turn_end") == 0) {
    a->now_what[0] = '\0';
    if (a->turn_tokens > 0)
      spark_push(a, (int)(a->turn_tokens > 1000000 ? 1000000
                                                   : a->turn_tokens));
  } else if (strcmp(kind, "error") == 0) {
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->name, sizeof e->name, "error");
    snprintf(e->extra, sizeof e->extra, "%.31s", js(data, "code", ""));
    e->red = 1;
    a->now_what[0] = '\0';
  } else {
    /* forward compatibility: show the kind, dimly */
    tui_ev *e = ev_push(&a->evs);
    snprintf(e->kind, sizeof e->kind, "%.15s", kind);
    snprintf(e->name, sizeof e->name, "%.39s", kind);
  }

  xcdn_document_free(doc);
}

/* asngn_log hands the callback the full formatted record
 * ("<RFC3339> LEVEL SUBSYS  message"). The trace pane has ~23 columns:
 * showing the record verbatim renders nothing but the date. Split off
 * the head — keep the subsystem for context, keep the message as the
 * row text, and drop the timestamp/level (the row color already says
 * WARN vs ERROR). Tolerates a bare message with no head. */
static const char *log_split_head(const char *msg, char *subsys,
                                  size_t subsys_sz) {
  const char *p = msg, *tok;
  size_t i, n;
  subsys[0] = '\0';
  for (i = 0; i < 4; i++)
    if (p[i] < '0' || p[i] > '9') return msg; /* no timestamp head */
  if (p[4] != '-') return msg;
  while (*p != '\0' && *p != ' ') p++; /* timestamp */
  while (*p == ' ') p++;
  while (*p != '\0' && *p != ' ') p++; /* level */
  while (*p == ' ') p++;
  tok = p; /* subsystem */
  while (*p != '\0' && *p != ' ') p++;
  n = (size_t)(p - tok);
  if (n >= subsys_sz) n = subsys_sz - 1;
  memcpy(subsys, tok, n);
  subsys[n] = '\0';
  while (*p == ' ') p++;
  return p;
}

void tui_events_log(tui_app *a, int level, const char *msg) {
  tui_ev *e;
  char name[40];
  char subsys[16];
  uint64_t back, lo;

  msg = log_split_head(msg != NULL ? msg : "", subsys, sizeof subsys);
  if (msg[0] == '\0') return; /* no message body: nothing worth a row */
  snprintf(name, sizeof name, "%.39s", msg);

  /* The astools registry re-scan re-emits the same warning set every
   * few seconds; a repeated line bumps an "xN" counter on its earlier
   * row instead of flushing the turn trace out of the ring. */
  lo = a->evs.seq > 32 ? a->evs.seq - 32 : 0;
  if (lo < a->evs.turn_seq) lo = a->evs.turn_seq;
  for (back = a->evs.seq; back > lo; back--) {
    tui_ev *prev = &a->evs.ev[(back - 1) % TUI_EV_CAP];
    if (strcmp(prev->kind, "log") == 0 &&
        strcmp(prev->name, name) == 0 &&
        strcmp(prev->tier, subsys) == 0) {
      prev->repeats++;
      snprintf(prev->extra, sizeof prev->extra, "x%d",
               prev->repeats + 1);
      a->dirty = 1;
      return;
    }
  }

  e = ev_push(&a->evs);
  snprintf(e->kind, sizeof e->kind, "log");
  memcpy(e->name, name, sizeof name);
  snprintf(e->tier, sizeof e->tier, "%s", subsys);
  if (level <= ASNGN_LOG_ERROR) e->red = 1;
  else e->amber = 1;
  a->dirty = 1;
  a->structural = 1;
}

/* ── pane rendering ───────────────────────────────────────────────────── */

static const char *const PANE_NAME[PANE_COUNT] = {"trace", "stats",
                                                  "memory", "tools",
                                                  "cache"};

static void draw_ev_line(tui_app *a, tui_frame *f, int x, int y, int w,
                         const tui_ev *e) {
  const tui_theme *th = &a->theme;
  uint8_t name_fg = TFG_DEFAULT;
  int cx = x;
  if (e->amber) name_fg = TFG_AMBER;
  if (e->red) name_fg = TFG_RED;
  cx += d_put(f, cx, y, th->marker, TFG_ACCENT, TBG_DEFAULT, 0);
  cx += d_put(f, cx, y, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
  if (e->kind[0] == 'l') { /* log line: subsys, then full-width message */
    int avail;
    if (e->tier[0] != '\0') {
      cx += d_putn(f, cx, y, e->tier, 6, TFG_DIM, TBG_DEFAULT, 0);
      cx += d_put(f, cx, y, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
    }
    avail = x + w - cx;
    if (e->repeats > 0) avail -= (int)strlen(e->extra) + 1;
    if (avail > 0)
      cx += d_putn(f, cx, y, e->name, avail, name_fg, TBG_DEFAULT, 0);
    if (e->repeats > 0 && cx < x + w) {
      cx += d_put(f, cx, y, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
      d_putn(f, cx, y, e->extra, x + w - cx, TFG_DIM, TBG_DEFAULT, 0);
    }
    return;
  }
  cx += d_putn(f, cx, y, e->name, w > 14 ? 12 : w - 4, name_fg,
               TBG_DEFAULT, 0);
  { /* event row: fixed columns */
    int col = x + 15;
    if (col > cx + 1) cx = col;
    else cx += 1;
    cx += d_putn(f, cx, y, e->tier, 6, TFG_DIM, TBG_DEFAULT, 0);
    cx = x + 22;
    d_putn(f, cx, y, e->extra, w - 22 > 0 ? w - 22 : 0, TFG_DIM,
           TBG_DEFAULT, 0);
  }
}

static void pane_trace(tui_app *a, tui_frame *f, int x, int y, int w,
                       int h) {
  uint64_t from = a->evs.turn_seq, to = a->evs.seq;
  uint64_t n = to - from;
  uint64_t first;
  int row = 0;
  if (n == 0) {
    d_put(f, x, y, "no turn yet", TFG_DIM, TBG_DEFAULT, TA_DIM);
    return;
  }
  first = n > (uint64_t)h ? to - (uint64_t)h : from;
  for (; first < to && row < h; first++) {
    const tui_ev *e = ev_at(&a->evs, first);
    if (e == NULL) continue;
    draw_ev_line(a, f, x, y + row, w, e);
    row++;
  }
}

static void pane_stats(tui_app *a, tui_frame *f, int x, int y, int w,
                       int h) {
  const tui_theme *th = &a->theme;
  char b1[16], b2[16];
  int row = 0;
  size_t mi;
  (void)w;
  /* the pool: which actual weights each id maps to, dot = resident */
  if (row < h && a->models_n > 0)
    d_put(f, x, y + row++, "models", TFG_DIM, TBG_DEFAULT, 0);
  for (mi = 0; mi < a->models_n && row < h; mi++) {
    const asngn_model_info *m = &a->models[mi];
    int cx = x;
    cx += d_put(f, cx, y + row, m->resident ? th->dot_on : th->dot_off,
                m->resident ? TFG_ACCENT : TFG_DIM, TBG_DEFAULT, 0);
    cx += d_put(f, cx, y + row, " ", TFG_DIM, TBG_DEFAULT, 0);
    cx += d_putn(f, cx, y + row, m->id, 6, TFG_BRIGHT, TBG_DEFAULT, 0);
    cx += d_put(f, cx, y + row, " ", TFG_DIM, TBG_DEFAULT, 0);
    d_putn(f, cx, y + row, m->file, x + w - cx, TFG_DEFAULT, TBG_DEFAULT,
           0);
    row++;
  }
  if (row < h && a->models_n > 0) row++; /* gap */
  if (row < h)
    d_put(f, x, y + row++, "tokens/turn", TFG_DIM, TBG_DEFAULT, 0);
  if (row < h) {
    int i, max = 1, cx = x;
    for (i = 0; i < a->spark_n; i++)
      if (a->spark[i] > max) max = a->spark[i];
    for (i = 0; i < a->spark_n && cx < x + w; i++) {
      int lvl = (a->spark[i] * 4) / (max + 1);
      if (lvl > 3) lvl = 3;
      cx += d_put(f, cx, y + row, th->spark[lvl], TFG_ACCENT,
                  TBG_DEFAULT, 0);
    }
    if (a->spark_n == 0)
      d_put(f, x, y + row, "(no turns)", TFG_DIM, TBG_DEFAULT, TA_DIM);
    row++;
  }
  row++; /* gap */
  if (row < h && a->stats_ok) {
    char line[64];
    snprintf(line, sizeof line, "cache %zu%s %zu~ %zu%s",
             a->stats.cache_hits, th->check, a->stats.cache_adapts,
             a->stats.cache_misses, th->cross);
    d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  if (row < h && a->stats_ok) {
    int cx = x;
    tui_fmt_count(b1, sizeof b1, (long long)a->stats.tokens_saved);
    cx += d_put(f, cx, y + row, "saved ", TFG_DIM, TBG_DEFAULT, 0);
    cx += d_put(f, cx, y + row, b1, TFG_ACCENT, TBG_DEFAULT, 0);
    d_put(f, cx, y + row, " tok", TFG_DIM, TBG_DEFAULT, 0);
    row++;
  }
  if (row < h) {
    int cx = x;
    tui_fmt_count(b1, sizeof b1, a->aux_tokens);
    cx += d_put(f, cx, y + row, "safety cost ", TFG_DIM, TBG_DEFAULT, 0);
    cx += d_put(f, cx, y + row, b1, TFG_DEFAULT, TBG_DEFAULT, 0);
    d_put(f, cx, y + row, " aux", TFG_DIM, TBG_DEFAULT, 0);
    row++;
  }
  if (row < h && a->stats_ok) {
    char line[64];
    snprintf(line, sizeof line, "guards %zu %s esc %zu",
             a->stats.guard_trips, th->bullet, a->stats.escalations);
    d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  if (row < h && a->sstats_ok) {
    char line[64];
    snprintf(line, sizeof line, "capped %zu %s clarify %zu",
             a->sstats.capped, th->bullet, a->sstats.clarifies);
    d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  if (row < h && a->sstats_ok) {
    tui_fmt_count(b2, sizeof b2, a->sstats.spent_tokens);
    {
      char line[48];
      snprintf(line, sizeof line, "spent %s tok", b2);
      d_put(f, x, y + row++, line, TFG_DIM, TBG_DEFAULT, 0);
    }
  }
}

static void pane_memory(tui_app *a, tui_frame *f, int x, int y, int w,
                        int h) {
  int row = 0;
  char line[64];
  (void)w;
  if (!a->sib_ok || !a->sib.asper_ok) {
    d_put(f, x, y, "asper disabled", TFG_DIM, TBG_DEFAULT, TA_DIM);
    return;
  }
  snprintf(line, sizeof line, "identity   %zu", a->sib.mem_identity);
  if (row < h) d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  snprintf(line, sizeof line, "context    %zu", a->sib.mem_context);
  if (row < h) d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  snprintf(line, sizeof line, "project    %zu", a->sib.mem_project);
  if (row < h) d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  snprintf(line, sizeof line, "deprecated %zu", a->sib.mem_deprecated);
  if (row < h) d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  row++;
  if (row < h) {
    int cx = x;
    cx += d_put(f, cx, y + row, "project ", TFG_DIM, TBG_DEFAULT, 0);
    if (a->sib.project[0] != '\0')
      d_put(f, cx, y + row, a->sib.project, TFG_ACCENT, TBG_DEFAULT, 0);
    else
      d_put(f, cx, y + row, "none", TFG_DIM, TBG_DEFAULT, TA_DIM);
    row++;
  }
  if (row < h) {
    char ago[16];
    int cx = x;
    tui_fmt_ago(ago, sizeof ago, a->sib.mem_last_cycle_at);
    cx += d_put(f, cx, y + row, "cycle ", TFG_DIM, TBG_DEFAULT, 0);
    d_put(f, cx, y + row, ago, TFG_DEFAULT, TBG_DEFAULT, 0);
    row++;
  }
}

static void pane_tools(tui_app *a, tui_frame *f, int x, int y, int w,
                       int h) {
  const tui_theme *th = &a->theme;
  int row = 0;
  char line[80];
  if (!a->sib_ok || !a->sib.astools_ok) {
    d_put(f, x, y, "astools disabled", TFG_DIM, TBG_DEFAULT, TA_DIM);
    return;
  }
  snprintf(line, sizeof line, "tools  %zu/%zu enabled",
           a->sib.tools_enabled, a->sib.tools_total);
  if (row < h) d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  snprintf(line, sizeof line, "calls  %zu %s%zu %s%zu",
           a->sib.tool_invocations, th->check, a->sib.tool_ok, th->cross,
           a->sib.tool_failed);
  if (row < h) d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  snprintf(line, sizeof line, "denied %zu", a->sib.tool_denied);
  if (row < h) d_put(f, x, y + row++, line, TFG_DIM, TBG_DEFAULT, 0);
  row++;
  /* the last few tool_call events, newest last */
  {
    uint64_t seq = a->evs.seq;
    uint64_t i = seq > TUI_EV_CAP ? seq - TUI_EV_CAP : 0;
    const tui_ev *picks[16];
    int np = 0, k;
    for (; i < seq; i++) {
      const tui_ev *e = ev_at(&a->evs, i);
      if (e == NULL || strcmp(e->kind, "tool_call") != 0) continue;
      if (np == 16) {
        memmove((void *)picks, picks + 1, 15 * sizeof picks[0]);
        np--;
      }
      picks[np++] = e;
    }
    if (row >= h) {
      np = 0;
    } else if (np > h - row) {
      k = np - (h - row);
      memmove((void *)picks, (const void *)(picks + k),
              (size_t)(np - k) * sizeof picks[0]);
      np -= k;
    }
    for (k = 0; k < np && row < h; k++, row++) {
      int cx = x;
      const tui_ev *e = picks[k];
      cx += d_put(f, cx, y + row, e->ok == 1 ? th->check : th->cross,
                  e->ok == 1 ? TFG_ACCENT : TFG_RED, TBG_DEFAULT, 0);
      cx += d_put(f, cx, y + row, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
      cx += d_putn(f, cx, y + row, e->name, 14, TFG_DEFAULT,
                   TBG_DEFAULT, 0);
      cx = x + 17;
      d_putn(f, cx, y + row, e->extra, w - 17 > 0 ? w - 17 : 0, TFG_DIM,
             TBG_DEFAULT, 0);
    }
  }
}

static void pane_cache(tui_app *a, tui_frame *f, int x, int y, int w,
                       int h) {
  const tui_theme *th = &a->theme;
  int row = 0;
  char line[64];
  if (a->stats_ok) {
    snprintf(line, sizeof line, "hit %zu %s adapt %zu", a->stats.cache_hits,
             th->bullet, a->stats.cache_adapts);
    if (row < h)
      d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
    snprintf(line, sizeof line, "miss %zu %s tool %zu",
             a->stats.cache_misses, th->bullet, a->stats.tool_cache_hits);
    if (row < h)
      d_put(f, x, y + row++, line, TFG_DEFAULT, TBG_DEFAULT, 0);
  }
  row++;
  if (row < h) d_put(f, x, y + row++, "probes", TFG_DIM, TBG_DEFAULT, 0);
  {
    uint64_t seq = a->evs.seq;
    uint64_t i = seq > TUI_EV_CAP ? seq - TUI_EV_CAP : 0;
    const tui_ev *picks[8];
    int np = 0, k;
    for (; i < seq; i++) {
      const tui_ev *e = ev_at(&a->evs, i);
      if (e == NULL || strcmp(e->kind, "cache_probe") != 0) continue;
      if (np == 8) {
        memmove((void *)picks, (const void *)(picks + 1),
                7 * sizeof picks[0]);
        np--;
      }
      picks[np++] = e;
    }
    for (k = np > h - row - 1 ? np - (h - row - 1) : 0;
         k < np && row < h - 1; k++, row++) {
      int cx = x;
      const tui_ev *e = picks[k];
      cx += d_putn(f, cx, y + row, e->tier, 6, TFG_DEFAULT, TBG_DEFAULT,
                   0);
      cx = x + 7;
      d_putn(f, cx, y + row, e->extra, w - 7 > 0 ? w - 7 : 0, TFG_DIM,
             TBG_DEFAULT, 0);
    }
  }
  if (h > 0)
    d_put(f, x, y + h - 1, "/cache clear", TFG_DIM, TBG_DEFAULT, TA_DIM);
}

void panes_draw(tui_app *a, tui_frame *f, int x, int y, int w, int h) {
  const tui_theme *th = &a->theme;
  int ix, iy, iw, ih;
  if (w < 12 || h < 4) return;
  d_box(f, x, y, w, h, th, 0, TFG_DIM, 0);
  {
    int tx = x + 1;
    tx += d_put(f, tx, y, " ", TFG_DIM, TBG_DEFAULT, 0);
    tx += d_put(f, tx, y, PANE_NAME[a->pane], TFG_ACCENT, TBG_DEFAULT, 0);
    d_put(f, tx, y, " ", TFG_DIM, TBG_DEFAULT, 0);
  }
  ix = x + 2;
  iy = y + 1;
  iw = w - 4;
  ih = h - 2;
  switch (a->pane) {
  case PANE_TRACE: pane_trace(a, f, ix, iy, iw, ih); break;
  case PANE_STATS: pane_stats(a, f, ix, iy, iw, ih); break;
  case PANE_MEMORY: pane_memory(a, f, ix, iy, iw, ih); break;
  case PANE_TOOLS: pane_tools(a, f, ix, iy, iw, ih); break;
  default: pane_cache(a, f, ix, iy, iw, ih); break;
  }
}
