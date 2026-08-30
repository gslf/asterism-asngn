/*
 * context.c — zoned prompt assembly.
 *
 * The assembled prompt is a fixed sequence of zones, split across the
 * (system, user) message pair every backend consumes:
 *
 *   system_text: [1 system+directive] [2 Asper semantic memory] [3 catalog]
 *   user_text:   [4 Asper scoped context] [5 current operational turn]
 *
 * Assembly is a pure function of session state, configuration, and turn
 * state: identical inputs produce byte-identical prompts (golden-tested).
 * Trimming operates on whole items only; a zone that contributes nothing
 * is omitted together with its heading. Token counts are measured with
 * the tokenizer of the model that will consume the prompt; the
 * per-zone split is counted zone-by-zone.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

void asngn_prompt_free(asngn_prompt *p) {
  if (p == NULL) return;
  free(p->system_text);
  free(p->user_text);
  memset(p, 0, sizeof *p);
}

static asngn_err context_check(asngn_ctx *c, int slot,
                               const char *system_text,
                               const char *user_text, int output_reserve,
                               const asngn_prompt *zones) {
  asngn_context_diagnostics d;
  size_t attributed;
  int exact;
  if (c == NULL || slot < 0 || (size_t)slot >= c->models_n)
    return ASNGN_ERR_INVALID;
  memset(&d, 0, sizeof d);
  d.n_ctx = c->models[slot].cfg.ctx > 0
                ? (size_t)c->models[slot].cfg.ctx : 32768u;
  d.output_reserve = output_reserve > 0
                         ? (size_t)output_reserve
                         : (size_t)c->cfg.rich_tokens;
  d.safety_margin = (size_t)c->cfg.safety_margin;
  d.prompt_budget = d.output_reserve + d.safety_margin < d.n_ctx
                        ? d.n_ctx - d.output_reserve - d.safety_margin : 0;
  exact = asngn_models_count_prompt(c, slot, system_text, user_text);
  d.prompt_total = exact > 0 ? (size_t)exact : 0;
  if (zones != NULL) {
    d.system = zones->tok_system;
    d.memory = zones->tok_memory;
    d.catalog = zones->tok_catalog;
    d.summary = zones->tok_summary;
    d.verbatim = zones->tok_verbatim;
    d.working = zones->tok_working;
  } else {
    int sn = asngn_models_count_tokens(c, slot, system_text);
    int un = asngn_models_count_tokens(c, slot, user_text);
    d.system = sn > 0 ? (size_t)sn : 0;
    d.working = un > 0 ? (size_t)un : 0;
  }
  attributed = d.system + d.memory + d.catalog + d.summary +
               d.verbatim + d.working;
  d.overhead = d.prompt_total > attributed ? d.prompt_total - attributed : 0;
  if (d.prompt_total <= d.prompt_budget) return ASNGN_OK;

  os_mutex_lock(&c->err_mu);
  c->context_diag = d;
  snprintf(c->errbuf, sizeof c->errbuf,
           "context budget exceeded: prompt=%zu budget=%zu "
           "(n_ctx=%zu output=%zu safety=%zu; system=%zu memory=%zu "
           "catalog=%zu summary=%zu verbatim=%zu working=%zu overhead=%zu)",
           d.prompt_total, d.prompt_budget, d.n_ctx, d.output_reserve,
           d.safety_margin, d.system, d.memory, d.catalog, d.summary,
           d.verbatim, d.working, d.overhead);
  os_mutex_unlock(&c->err_mu);
  return ASNGN_ERR_CONTEXT;
}

asngn_err asngn_context_validate(asngn_ctx *c, int count_slot,
                                 const asngn_prompt *prompt,
                                 int output_reserve) {
  if (prompt == NULL) return ASNGN_ERR_INVALID;
  return context_check(c, count_slot, prompt->system_text, prompt->user_text,
                       output_reserve, prompt);
}

asngn_err asngn_context_validate_text(asngn_ctx *c, int count_slot,
                                      const char *system_text,
                                      const char *user_text,
                                      int output_reserve) {
  return context_check(c, count_slot,
                       system_text != NULL ? system_text : "",
                       user_text != NULL ? user_text : "", output_reserve,
                       NULL);
}

/* Count tokens of a zone snippet; empty text is zero. */
static size_t zone_tokens(asngn_ctx *c, int slot, const char *text) {
  int n;
  if (text == NULL || text[0] == '\0') return 0;
  n = asngn_models_count_tokens(c, slot, text);
  return n > 0 ? (size_t)n : 0;
}

/* Render one transcript turn as a verbatim line block. */
static asngn_err verb_render(asngn_buf *b, const asngn_turn *t) {
  asngn_err e = asngn_buf_appends(b, t->role);
  if (e == ASNGN_OK) e = asngn_buf_appends(b, ": ");
  if (e == ASNGN_OK) e = asngn_buf_appends(b, t->text);
  if (e == ASNGN_OK) e = asngn_buf_appendc(b, '\n');
  return e;
}

/* Render the mandatory current message and instruction around a suffix of
 * optional working evidence.  The caller tokenizes this complete block, so
 * the zone limit is based on the exact prompt shape rather than a sum of
 * independently tokenized fragments (which is not generally additive). */
static asngn_err working_render(asngn_buf *b, const asngn_turn_state *t,
                                const char *instruction, size_t first,
                                bool follows_verbatim) {
  asngn_err e;
  size_t i;

  e = asngn_buf_appends(b, follows_verbatim ? "\n## This turn\n"
                                           : "## This turn\n");
  if (e == ASNGN_OK && t != NULL && t->user_msg != NULL) {
    e = asngn_buf_appends(b, "user: ");
    if (e == ASNGN_OK) e = asngn_buf_appends(b, t->user_msg);
    if (e == ASNGN_OK) e = asngn_buf_appendc(b, '\n');
  }
  if (e == ASNGN_OK && t != NULL) {
    for (i = first; i < t->work_n && e == ASNGN_OK; i++) {
      e = asngn_buf_appends(b, t->work[i].text);
      if (e == ASNGN_OK && t->work[i].text[0] != '\0' &&
          t->work[i].text[strlen(t->work[i].text) - 1] != '\n')
        e = asngn_buf_appendc(b, '\n');
    }
  }
  if (e == ASNGN_OK && instruction != NULL && instruction[0] != '\0') {
    e = asngn_buf_appendc(b, '\n');
    if (e == ASNGN_OK) e = asngn_buf_appends(b, instruction);
    if (e == ASNGN_OK) e = asngn_buf_appendc(b, '\n');
  }
  return e;
}

/* Select the verbatim zone: pinned turns first (chronological),
 * then the most recent unpinned turns, chosen newest-first under the
 * remaining budget, rendered chronologically. The current in-flight user
 * message is excluded (it renders in the working zone). Whole turns
 * only. */
static asngn_err verbatim_build(asngn_ctx *c, asngn_session *s,
                                const asngn_turn_state *t, int count_slot,
                                char **out_text, size_t *out_tokens) {
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
  if (t != NULL && s->log[n - 1].pinned == false &&
      strcmp(s->log[n - 1].role, "user") == 0 && t->user_msg != NULL &&
      strcmp(s->log[n - 1].text, t->user_msg) == 0)
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
      if (e != ASNGN_OK) { asngn_buf_free(&one); goto out; }
      cost = zone_tokens(c, count_slot, one.data);
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
      if (e != ASNGN_OK) { asngn_buf_free(&one); goto out; }
      cost = zone_tokens(c, count_slot, one.data);
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

out:
  free(take);
  return e;
}

asngn_err asngn_context_assemble(asngn_ctx *c, asngn_session *s,
                                 asngn_turn_state *t,
                                 const char *base_override,
                                 const char *instruction, int count_slot,
                                 asngn_prompt *out) {
  asngn_buf sys, usr;
  asngn_err e = ASNGN_OK;
  const char *base = base_override != NULL
                         ? base_override
                         : (c->cfg.base_prompt != NULL ? c->cfg.base_prompt : "");
  char *verb_text = NULL;
  size_t verb_tokens = 0;
  char *asper_system = NULL, *asper_context = NULL;
  size_t asper_system_tokens = 0, asper_context_tokens = 0;

  memset(out, 0, sizeof *out);
  asngn_buf_init(&sys);
  asngn_buf_init(&usr);

  /* Asper owns every persistent/historical memory zone. */
  if (c->asper_ok && s != NULL && t != NULL) {
    e = asngn_siblings_context(c, s->slug, base,
                               t->user_msg ? t->user_msg : "",
                               (size_t)c->cfg.memory_history_tokens,
                               (size_t)c->cfg.memory_checkpoint_tokens,
                               count_slot,
                               &asper_system, &asper_context,
                               &asper_system_tokens, &asper_context_tokens);
    if (e != ASNGN_OK) goto fail;
    e = asngn_buf_appends(&sys, asper_system);
    if (e != ASNGN_OK) goto fail;
    out->tok_system = zone_tokens(c, count_slot, base);
    out->tok_memory = asper_system_tokens > out->tok_system
                          ? asper_system_tokens - out->tok_system : 0;
    if (asper_context[0]) {
      e = asngn_buf_appends(&usr, asper_context);
      if (e == ASNGN_OK &&
          asper_context[strlen(asper_context) - 1] != '\n')
        e = asngn_buf_appendc(&usr, '\n');
      if (e != ASNGN_OK) goto fail;
      out->tok_memory += asper_context_tokens;
    }
  } else {
    e = asngn_buf_appends(&sys, base);
    if (e != ASNGN_OK) goto fail;
    out->tok_system = zone_tokens(c, count_slot, base);
  }

  /* zone 3: tool catalog (astools renders its own "## Tools" heading) */
  if (t != NULL && t->phase == ASNGN_PHASE_ACTION &&
      t->catalog != NULL && t->catalog[0] != '\0' && !t->opts.no_tools) {
    e = asngn_buf_appends(&sys, "\n\n");
    if (e == ASNGN_OK) e = asngn_buf_appends(&sys, t->catalog);
    if (e != ASNGN_OK) goto fail;
    out->tok_catalog = zone_tokens(c, count_slot, t->catalog);
  }

  /* zone 4: in-process turns when Asper is disabled */
  if (!c->asper_ok && s != NULL) {
    e = verbatim_build(c, s, t, count_slot, &verb_text, &verb_tokens);
    if (e != ASNGN_OK) goto fail;
    if (verb_text != NULL) {
      e = asngn_buf_appends(&usr, "## Conversation\n");
      if (e == ASNGN_OK) e = asngn_buf_appends(&usr, verb_text);
      if (e != ASNGN_OK) goto fail;
      out->tok_verbatim = verb_tokens;
    }
  }

  /* zone 6: this turn — current message, working items, instruction */
  {
    asngn_buf work;
    size_t first = c->asper_ok && t != NULL ? t->work_n : 0;
    size_t rendered_tokens = 0;
    for (;;) {
      asngn_buf_init(&work);
      e = working_render(&work, t, instruction, first, usr.len > 0);
      if (e != ASNGN_OK) break;
      rendered_tokens = zone_tokens(c, count_slot, work.data);
      if (rendered_tokens <= (size_t)c->cfg.working_tokens || t == NULL ||
          first >= t->work_n)
        break;
      asngn_buf_free(&work);
      first++;
    }
    if (e != ASNGN_OK) {
      asngn_buf_free(&work);
      goto fail;
    }
    if (first > 0) {
      asngn_log(c, ASNGN_LOG_WARN, "context",
                "working zone over budget: %zu item(s) trimmed", first);
      asngn_tele_emit(c, "guard", NULL, NULL,
                      s != NULL ? s->slug : NULL,
                      t->led.turn, "{guard: \"working_trim\"}");
    }
    out->tok_working = rendered_tokens;
    e = asngn_buf_append(&usr, work.data, work.len);
    asngn_buf_free(&work);
    if (e != ASNGN_OK) goto fail;
  }

  out->system_text = asngn_buf_detach(&sys);
  out->user_text = asngn_buf_detach(&usr);
  free(asper_system);
  free(asper_context);
  free(verb_text);
  asngn_buf_free(&sys);
  asngn_buf_free(&usr);
  if (out->system_text == NULL || out->user_text == NULL) {
    asngn_prompt_free(out);
    return ASNGN_ERR_NOMEM;
  }
  return ASNGN_OK;

fail:
  free(asper_system);
  free(asper_context);
  free(verb_text);
  asngn_buf_free(&sys);
  asngn_buf_free(&usr);
  asngn_prompt_free(out);
  return e;
}
