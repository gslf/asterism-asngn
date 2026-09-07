/* Local transcript selection when ⁂ asper is disabled. */
#include "context.h"
#include <stdlib.h>
#include <string.h>

/* Render one transcript turn as a verbatim line block. */
static asngn_err verb_render(asngn_buf *b, const asngn_turn *t) {
  asngn_err e = asngn_buf_appends(b, t->role);
  if (e == ASNGN_OK) e = asngn_buf_appends(b, ": ");
  if (e == ASNGN_OK) e = asngn_buf_appends(b, t->text);
  if (e == ASNGN_OK) e = asngn_buf_appendc(b, '\n');
  return e;
}

/* Select the verbatim zone: pinned turns first (chronological),
 * then the most recent unpinned turns, chosen newest-first under the
 * remaining budget, rendered chronologically. The current in-flight user
 * message is excluded (it renders in the working zone). Whole turns
 * only. */
asngn_err asngn_context_verbatim(asngn_ctx *c, asngn_session *s, const asngn_turn_state *t,
                                 int count_slot, char **out_text, size_t *out_tokens,
                                 asngn_context_trace *trace) {
  size_t budget = (size_t)c->cfg.memory_history_tokens;
  size_t used = 0;
  size_t n = s->log_n;
  size_t i;
  bool *take = NULL;
  asngn_buf b;
  asngn_err e = ASNGN_OK;
  size_t skip_last = 0;

  *out_text = NULL;
  *out_tokens = 0;
  if (n == 0) return ASNGN_OK;
  /* the freshly ingested user message lives in the working zone */
  if (t != NULL && s->log[n - 1].pinned == false && strcmp(s->log[n - 1].role, "user") == 0 &&
      t->user_msg != NULL && strcmp(s->log[n - 1].text, t->user_msg) == 0)
    skip_last = 1;

  take = calloc(n, sizeof *take);
  if (take == NULL) return ASNGN_ERR_NOMEM;

  /* pass 1: pinned turns, oldest first, whole-item budget */
  for (i = 0; i < n - skip_last; i++) {
    const asngn_turn *tr = &s->log[i];
    if (!tr->pinned) continue;
    {
      asngn_buf one;
      size_t cost;
      asngn_buf_init(&one);
      e = verb_render(&one, tr);
      if (e != ASNGN_OK) {
        asngn_buf_free(&one);
        goto out;
      }
      cost = asngn_context_tokens(c, count_slot, one.data);
      asngn_buf_free(&one);
      if (used + cost <= budget) {
        take[i] = true;
        used += cost;
      }
    }
  }
  /* pass 2: unpinned, newest first */
  for (i = n - skip_last; i > 0; i--) {
    const asngn_turn *tr = &s->log[i - 1];
    if (tr->pinned) continue;
    {
      asngn_buf one;
      size_t cost;
      asngn_buf_init(&one);
      e = verb_render(&one, tr);
      if (e != ASNGN_OK) {
        asngn_buf_free(&one);
        goto out;
      }
      cost = asngn_context_tokens(c, count_slot, one.data);
      asngn_buf_free(&one);
      /* A large recent turn does not imply older turns are larger.  Keep
       * scanning so a small but useful earlier exchange can still fit. */
      if (used + cost > budget) continue;
      take[i - 1] = true;
      used += cost;
    }
  }

  /* render: pinned first (chronological), then taken unpinned
   * (chronological) */
  asngn_buf_init(&b);
  for (i = 0; i < n && e == ASNGN_OK; i++)
    if (take[i] && s->log[i].pinned) e = verb_render(&b, &s->log[i]);
  for (i = 0; i < n && e == ASNGN_OK; i++)
    if (take[i] && !s->log[i].pinned) e = verb_render(&b, &s->log[i]);
  if (e != ASNGN_OK) {
    asngn_buf_free(&b);
    goto out;
  }
  if (b.len > 0) {
    *out_text = asngn_buf_detach(&b);
    if (*out_text == NULL) e = ASNGN_ERR_NOMEM;
    else *out_tokens = used;
  }
  asngn_buf_free(&b);

  for (i = 0; e == ASNGN_OK && i < n; i++) {
    const char *reason = i >= n - skip_last ? "current_message_in_working"
                         : !take[i]         ? "history_budget"
                         : s->log[i].pinned ? "pinned"
                                            : "recent";
    e = asngn_context_trace_item(trace, "verbatim", i, s->log[i].text,
                                 take[i] ? "included" : "excluded", reason);
  }

out:
  free(take);
  return e;
}
