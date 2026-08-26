/*
 * ledger.c — the per-turn token ledger.
 *
 * sessions/<slug>/ledger.xcdn is an append stream of compact #ledger_entry
 * values plus #ledger_feedback { turn, user, at } amendments, so the file
 * stays append-only while F7/F8 feedback can land after commit. Entries
 * stay resident on the session for stats, QpT, and export.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

/* ── QpT ─────────────────────────────────────────────────────────────── */

size_t asngn_ledger_total_tokens(const asngn_ledger_entry *e) {
  return e->pt_system + e->pt_memory + e->pt_catalog + e->pt_summary +
         e->pt_verbatim + e->pt_working + e->gt_decision + e->gt_answer +
         e->gt_aux;
}

double asngn_qpt(double q, size_t total_tokens) {
  if (total_tokens == 0) total_tokens = 1;
  if (q < 0.0) q = 0.0;
  if (q > 1.0) q = 1.0;
  return q / ((double)total_tokens / 1000.0);
}

static double led_quality(const asngn_ledger_entry *e) {
  /* Unknown quality is never invented as a neutral 0.5. QpT is a
   * backwards-compatible diagnostic; external outcome gates own coding
   * task success. Explicit feedback can provide a binary result when no
   * judge ran. */
  double q = e->has_judge ? e->judge
                          : (e->has_user_fb && e->user_fb > 0 ? 1.0 : 0.0);
  if (e->has_judge && e->has_user_fb)
    q += 0.3 * (double)e->user_fb;
  if (q < 0.0) q = 0.0;
  if (q > 1.0) q = 1.0;
  return q;
}

/* Rolling QpT over the last 20 committed entries of a session. */
static double led_rolling_qpt(const asngn_session *s) {
  size_t n = s->led_n < 20 ? s->led_n : 20;
  size_t i;
  double sum = 0.0;
  if (n == 0) return 0.0;
  for (i = s->led_n - n; i < s->led_n; i++)
    sum += asngn_qpt(led_quality(&s->led[i]),
                     asngn_ledger_total_tokens(&s->led[i]));
  return sum / (double)n;
}

double asngn_session_qpt(const asngn_session *s) {
  return led_rolling_qpt(s);
}

/* ── serialization ────────────────────────────────────────────────────── */

static xcdn_value_t *led_str(const char *s) {
  return xcdn_value_string(s != NULL ? s : "");
}

static xcdn_node_t *led_build_node(const asngn_ledger_entry *e) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_value_t *route = xcdn_value_object();
  xcdn_value_t *pt = xcdn_value_object();
  xcdn_value_t *gt = xcdn_value_object();
  xcdn_value_t *sv = xcdn_value_object();
  xcdn_value_t *q = xcdn_value_object();
  xcdn_node_t *node = NULL;
  char at[21];
  bool ok = obj != NULL && route != NULL && pt != NULL && gt != NULL &&
            sv != NULL && q != NULL;

  asngn_time_format_rfc3339(e->at, at);
  ok = ok && asngn_xobj_put(obj, "turn", xcdn_value_int((int64_t)e->turn));
  ok = ok && asngn_xobj_put(obj, "at", xcdn_value_datetime(at));

  ok = ok && asngn_xobj_put(route, "class", led_str(e->klass));
  ok = ok && asngn_xobj_put(route, "detail", led_str(e->detail));
  ok = ok && asngn_xobj_put(route, "mode", led_str(e->mode));
  ok = ok && asngn_xobj_put(route, "tier", led_str(e->tier));
  ok = ok && asngn_xobj_put(route, "escalations",
                            xcdn_value_int(e->escalations));
  ok = ok && asngn_xobj_put(route, "cache", led_str(e->cache));
  if (ok) { ok = asngn_xobj_put(obj, "route", route); route = NULL; }

  ok = ok && asngn_xobj_put(pt, "system",
                            xcdn_value_int((int64_t)e->pt_system));
  ok = ok && asngn_xobj_put(pt, "memory",
                            xcdn_value_int((int64_t)e->pt_memory));
  ok = ok && asngn_xobj_put(pt, "catalog",
                            xcdn_value_int((int64_t)e->pt_catalog));
  ok = ok && asngn_xobj_put(pt, "summary",
                            xcdn_value_int((int64_t)e->pt_summary));
  ok = ok && asngn_xobj_put(pt, "verbatim",
                            xcdn_value_int((int64_t)e->pt_verbatim));
  ok = ok && asngn_xobj_put(pt, "working",
                            xcdn_value_int((int64_t)e->pt_working));
  if (ok) { ok = asngn_xobj_put(obj, "prompt_tokens", pt); pt = NULL; }

  ok = ok && asngn_xobj_put(gt, "decision",
                            xcdn_value_int((int64_t)e->gt_decision));
  ok = ok && asngn_xobj_put(gt, "answer",
                            xcdn_value_int((int64_t)e->gt_answer));
  ok = ok && asngn_xobj_put(gt, "aux", xcdn_value_int((int64_t)e->gt_aux));
  if (ok) { ok = asngn_xobj_put(obj, "gen_tokens", gt); gt = NULL; }

  ok = ok && asngn_xobj_put(sv, "cache",
                            xcdn_value_int((int64_t)e->sv_cache));
  ok = ok && asngn_xobj_put(sv, "digest",
                            xcdn_value_int((int64_t)e->sv_digest));
  if (ok) { ok = asngn_xobj_put(obj, "saved_tokens", sv); sv = NULL; }

  ok = ok && asngn_xobj_put(q, "judge",
                            e->has_judge ? xcdn_value_float(e->judge)
                                         : xcdn_value_null());
  ok = ok && asngn_xobj_put(q, "user",
                            e->has_user_fb ? xcdn_value_int(e->user_fb)
                                           : xcdn_value_null());
  if (ok) { ok = asngn_xobj_put(obj, "quality", q); q = NULL; }

  ok = ok && asngn_xobj_put(obj, "capped", xcdn_value_bool(e->capped));
  ok = ok && asngn_xobj_put(obj, "duration_ms",
                            xcdn_value_int((int64_t)e->duration_ms));
  if (!ok) goto fail;
  node = xcdn_node_new(obj);
  if (node == NULL) goto fail;
  obj = NULL;
  if (!asngn_xnode_tag(node, "ledger_entry")) {
    /* tag failure leaves the node owned by us: release the whole tree */
    xcdn_node_free(node);
    goto fail;
  }
  return node;

fail:
  if (obj != NULL) xcdn_value_free(obj);
  if (route != NULL) xcdn_value_free(route);
  if (pt != NULL) xcdn_value_free(pt);
  if (gt != NULL) xcdn_value_free(gt);
  if (sv != NULL) xcdn_value_free(sv);
  if (q != NULL) xcdn_value_free(q);
  return NULL;
}

static void led_field(char *dst, size_t dstsz, const char *src) {
  snprintf(dst, dstsz, "%s", src != NULL ? src : "");
}

static bool led_from_node(const xcdn_node_t *node, asngn_ledger_entry *out) {
  const xcdn_value_t *obj, *sub, *v;
  int64_t n;
  double d;
  bool b;
  memset(out, 0, sizeof *out);
  if (node == NULL || node->value == NULL ||
      node->value->type != XCDN_VAL_OBJECT)
    return false;
  obj = node->value;
  if (!asngn_xint(asngn_xfield(obj, "turn"), &n) || n < 1) return false;
  out->turn = (size_t)n;
  if (!asngn_xtime(asngn_xfield(obj, "at"), &out->at)) return false;
  sub = asngn_xfield(obj, "route");
  if (sub != NULL && sub->type == XCDN_VAL_OBJECT) {
    led_field(out->klass, sizeof out->klass,
              asngn_xstr(asngn_xfield(sub, "class")));
    led_field(out->detail, sizeof out->detail,
              asngn_xstr(asngn_xfield(sub, "detail")));
    led_field(out->mode, sizeof out->mode,
              asngn_xstr(asngn_xfield(sub, "mode")));
    led_field(out->tier, sizeof out->tier,
              asngn_xstr(asngn_xfield(sub, "tier")));
    led_field(out->cache, sizeof out->cache,
              asngn_xstr(asngn_xfield(sub, "cache")));
    if (asngn_xint(asngn_xfield(sub, "escalations"), &n) && n >= 0)
      out->escalations = (int)n;
  }
  sub = asngn_xfield(obj, "prompt_tokens");
  if (sub != NULL && sub->type == XCDN_VAL_OBJECT) {
    if (asngn_xint(asngn_xfield(sub, "system"), &n) && n >= 0)
      out->pt_system = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "memory"), &n) && n >= 0)
      out->pt_memory = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "catalog"), &n) && n >= 0)
      out->pt_catalog = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "summary"), &n) && n >= 0)
      out->pt_summary = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "verbatim"), &n) && n >= 0)
      out->pt_verbatim = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "working"), &n) && n >= 0)
      out->pt_working = (size_t)n;
  }
  sub = asngn_xfield(obj, "gen_tokens");
  if (sub != NULL && sub->type == XCDN_VAL_OBJECT) {
    if (asngn_xint(asngn_xfield(sub, "decision"), &n) && n >= 0)
      out->gt_decision = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "answer"), &n) && n >= 0)
      out->gt_answer = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "aux"), &n) && n >= 0)
      out->gt_aux = (size_t)n;
  }
  sub = asngn_xfield(obj, "saved_tokens");
  if (sub != NULL && sub->type == XCDN_VAL_OBJECT) {
    if (asngn_xint(asngn_xfield(sub, "cache"), &n) && n >= 0)
      out->sv_cache = (size_t)n;
    if (asngn_xint(asngn_xfield(sub, "digest"), &n) && n >= 0)
      out->sv_digest = (size_t)n;
  }
  sub = asngn_xfield(obj, "quality");
  if (sub != NULL && sub->type == XCDN_VAL_OBJECT) {
    v = asngn_xfield(sub, "judge");
    if (v != NULL && asngn_xnum(v, &d)) {
      out->judge = d;
      out->has_judge = true;
    }
    v = asngn_xfield(sub, "user");
    if (v != NULL && asngn_xint(v, &n) && (n == 1 || n == -1)) {
      out->user_fb = (int)n;
      out->has_user_fb = true;
    }
  }
  v = asngn_xfield(obj, "capped");
  if (v != NULL && asngn_xbool(v, &b)) out->capped = b;
  if (asngn_xint(asngn_xfield(obj, "duration_ms"), &n) && n >= 0)
    out->duration_ms = (uint64_t)n;
  return true;
}

/* ── resident array ───────────────────────────────────────────────────── */

static asngn_err led_reserve(asngn_session *s) {
  if (s->led_n < s->led_cap) return ASNGN_OK;
  {
    size_t cap = s->led_cap != 0 ? s->led_cap * 2 : 16;
    asngn_ledger_entry *nl = realloc(s->led, cap * sizeof *nl);
    if (nl == NULL) return ASNGN_ERR_NOMEM;
    s->led = nl;
    s->led_cap = cap;
  }
  return ASNGN_OK;
}

/* live: a freshly committed turn. Replayed history feeds the session
 * totals only — adding it to the context-wide daily counter would double
 * count every time a session reopens within one process, so the daily
 * soft ceiling tracks spend observed live in this process. */
static void led_account(asngn_session *s, const asngn_ledger_entry *e,
                        bool live) {
  asngn_ctx *c = s->ctx;
  size_t total = asngn_ledger_total_tokens(e);
  asngn_time day = e->at / 86400;
  s->spent_tokens += (int64_t)total;
  if (!live) return;
  if (c->daily_day != day) {
    /* first spend seen for this unix day resets the counter */
    if (day > c->daily_day) {
      c->daily_day = day;
      c->daily_spent = 0;
    }
  }
  if (e->at / 86400 == c->daily_day) c->daily_spent += (int64_t)total;
}

/* ── public (internal) API ────────────────────────────────────────────── */

asngn_err asngn_ledger_append(asngn_session *s, const asngn_ledger_entry *e) {
  asngn_ctx *c = s->ctx;
  asngn_buf line;
  xcdn_node_t *node;
  asngn_err err;

  err = led_reserve(s);
  if (err != ASNGN_OK) return err;
  asngn_buf_init(&line);
  node = led_build_node(e);
  if (node == NULL) {
    asngn_buf_free(&line);
    return ASNGN_ERR_NOMEM;
  }
  err = asngn_xnode_write(node, false, &line);
  xcdn_node_free(node);
  if (err != ASNGN_OK) {
    asngn_buf_free(&line);
    return err;
  }
  err = asngn_stream_append(c, &s->ledger_st, line.data, line.len);
  asngn_buf_free(&line);
  if (err != ASNGN_OK) return err;

  s->led[s->led_n++] = *e;
  led_account(s, e, true);
  return ASNGN_OK;
}

asngn_err asngn_ledger_set_feedback(asngn_session *s, size_t turn,
                                    int signal) {
  asngn_ctx *c = s->ctx;
  size_t i;
  asngn_ledger_entry *found = NULL;
  for (i = s->led_n; i > 0; i--) {
    if (s->led[i - 1].turn == turn) {
      found = &s->led[i - 1];
      break;
    }
  }
  if (found == NULL)
    return asngn_seterr(c, ASNGN_ERR_NOT_FOUND,
                        "ledger: no entry for turn %zu", turn);
  if (signal == 0) {
    found->has_user_fb = false;
    found->user_fb = 0;
  } else if (signal == 1 || signal == -1) {
    found->has_user_fb = true;
    found->user_fb = signal;
  } else {
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "ledger: feedback signal must be -1, 0 or +1");
  }

  {
    xcdn_value_t *obj = xcdn_value_object();
    xcdn_node_t *node = NULL;
    asngn_buf line;
    asngn_err e = ASNGN_ERR_NOMEM;
    char at[21];
    bool ok = obj != NULL;
    asngn_buf_init(&line);
    asngn_time_format_rfc3339(asngn_clock_now(&c->clock), at);
    ok = ok && asngn_xobj_put(obj, "turn", xcdn_value_int((int64_t)turn));
    ok = ok && asngn_xobj_put(obj, "user",
                              signal != 0 ? xcdn_value_int(signal)
                                          : xcdn_value_null());
    ok = ok && asngn_xobj_put(obj, "at", xcdn_value_datetime(at));
    if (ok) {
      node = xcdn_node_new(obj);
      if (node != NULL) {
        obj = NULL;
        ok = asngn_xnode_tag(node, "ledger_feedback");
      } else ok = false;
    }
    if (ok) e = asngn_xnode_write(node, false, &line);
    if (e == ASNGN_OK)
      e = asngn_stream_append(c, &s->ledger_st, line.data, line.len);
    if (node != NULL) xcdn_node_free(node);
    if (obj != NULL) xcdn_value_free(obj);
    asngn_buf_free(&line);
    return e;
  }
}

asngn_err asngn_ledger_replay(asngn_session *s) {
  asngn_ctx *c = s->ctx;
  char *path = os_path_join(s->dir, "ledger.xcdn");
  struct xcdn_document *docp = NULL;
  xcdn_document_t *doc;
  asngn_err e;
  size_t i;

  if (path == NULL) return ASNGN_ERR_NOMEM;
  e = asngn_stream_load(c, path, "ledger", &docp);
  free(path);
  if (e != ASNGN_OK) return e;
  doc = (xcdn_document_t *)docp;
  if (doc == NULL) return ASNGN_OK;

  for (i = 0; i < doc->values_len; i++) {
    xcdn_node_t *node = doc->values[i];
    if (xcdn_node_has_tag(node, "ledger_entry")) {
      asngn_ledger_entry le;
      if (!led_from_node(node, &le)) {
        asngn_log(c, ASNGN_LOG_WARN, "session",
                  "%s: ledger value %zu invalid; skipped", s->slug, i);
        continue;
      }
      e = led_reserve(s);
      if (e != ASNGN_OK) {
        xcdn_document_free(doc);
        return e;
      }
      s->led[s->led_n++] = le;
      led_account(s, &le, false);
    } else if (xcdn_node_has_tag(node, "ledger_feedback")) {
      const xcdn_value_t *obj = node->value;
      int64_t turn = 0, fb = 0;
      size_t j;
      if (obj == NULL || obj->type != XCDN_VAL_OBJECT) continue;
      if (!asngn_xint(asngn_xfield(obj, "turn"), &turn)) continue;
      for (j = s->led_n; j > 0; j--) {
        if (s->led[j - 1].turn == (size_t)turn) {
          const xcdn_value_t *u = asngn_xfield(obj, "user");
          if (u != NULL && asngn_xint(u, &fb) && (fb == 1 || fb == -1)) {
            s->led[j - 1].has_user_fb = true;
            s->led[j - 1].user_fb = (int)fb;
          } else {
            s->led[j - 1].has_user_fb = false;
            s->led[j - 1].user_fb = 0;
          }
          break;
        }
      }
    } else {
      asngn_log(c, ASNGN_LOG_WARN, "session",
                "%s: ledger value %zu has an unknown tag; skipped",
                s->slug, i);
    }
  }
  xcdn_document_free(doc);
  return ASNGN_OK;
}
