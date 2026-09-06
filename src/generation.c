/* Generation watchdog and propagation of cancellation. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

/* ── model call wrapper with the stall watchdog ───────────────────────── */

typedef struct {
  asngn_ctx *c;
  asngn_turn_state *t;
  asngn_token_fn inner;
  void *inner_ud;
} watch_ud;

static void watch_token_cb(const char *piece, void *ud) {
  watch_ud *w = ud;
  w->c->call_last_ms = asngn_clock_mono_ms(&w->c->clock);
  if (w->t->cancel) w->c->call_cancel = 1;
  if (w->inner != NULL) w->inner(piece, w->inner_ud);
}

/* Preserve provider cancellations unless the runtime observed a stall. */
asngn_err asngn_generate_input(asngn_ctx *c, asngn_turn_state *t, int slot, asngn_task_kind task,
                               const asmodel_input *input, const char *gbnf, const char *schema,
                               const asmodel_tools *tools, int max_tokens, asngn_token_fn cb,
                               void *cb_ud, char **out_text, int *out_in, int *out_out) {
  if (out_text) *out_text = NULL;
  if (out_in) *out_in = 0;
  if (out_out) *out_out = 0;
  if (tools) asmodel_tool_calls_clear(tools->output);
  if (!c || !t || !out_text) return ASNGN_ERR_INVALID;
  watch_ud w;
  asngn_err e;
  int64_t now = asngn_clock_mono_ms(&c->clock);
  if (t->deadline_mono > 0 && now >= t->deadline_mono) return ASNGN_ERR_TIMEOUT;
  w.c = c;
  w.t = t;
  w.inner = cb;
  w.inner_ud = cb_ud;
  /* arm/disarm under q_mu so the watchdog can never cancel a call that
   * already ended (stall_tick holds the same mutex) */
  os_mutex_lock(&c->q_mu);
  c->call_cancel = t->cancel ? 1 : 0;
  c->call_stalled = false;
  c->call_started_ms = now;
  c->call_last_ms = now;
  c->call_turn = t;
  c->call_active = 1;
  os_mutex_unlock(&c->q_mu);
  e = asngn_models_generate_input(c, slot, task, input, gbnf, schema, tools, max_tokens,
                                  t->deadline_mono, watch_token_cb, &w, &c->call_cancel, out_text,
                                  out_in, out_out, t);
  os_mutex_lock(&c->q_mu);
  bool stalled = c->call_stalled;
  c->call_active = 0;
  c->call_turn = NULL;
  os_mutex_unlock(&c->q_mu);
  if (e == ASNGN_ERR_CANCELLED && stalled && !t->cancel) {
    /* the watchdog, not the user: report as a stall */
    asngn_tele_emit(c, "guard", NULL, NULL, t->s->slug, t->led.turn, "{guard: \"stall\"}");
    os_rwlock_wrlock(&c->lock);
    c->stats.guard_trips++;
    os_rwlock_wrunlock(&c->lock);
    return ASNGN_ERR_TIMEOUT;
  }
  return e;
}

asngn_err asngn_generate_watched(asngn_ctx *c, asngn_turn_state *t, int slot, asngn_task_kind task,
                                 const char *sys, const char *usr, const char *gbnf,
                                 const char *schema, int max_tokens, asngn_token_fn cb, void *cb_ud,
                                 char **out_text, int *out_in, int *out_out) {
  asmodel_text_input pair;
  asmodel_input_pair(&pair, sys, usr);
  return asngn_generate_input(c, t, slot, task, &pair.input, gbnf, schema, NULL, max_tokens, cb,
                              cb_ud, out_text, out_in, out_out);
}
