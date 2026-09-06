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
 * is omitted together with its heading. Zone counts use the text counter and
 * can be estimates; whole-request admission applies uncertainty reserves
 * separately. Counts of fragments are not generally additive.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "work_state.h"
#include "context.h"

void asngn_prompt_free(asngn_prompt *p) {
  if (p == NULL) return;
  free(p->system_text);
  free(p->user_text);
  free(p->selection_json);
  memset(p, 0, sizeof *p);
}

/* Render the mandatory current message and instruction around a suffix of
 * optional working evidence.  The caller tokenizes this complete block, so
 * the zone limit is based on the exact prompt shape rather than a sum of
 * independently tokenized fragments (which is not generally additive). */
static asngn_err working_render(asngn_buf *b, const asngn_turn_state *t, const char *instruction,
                                size_t first, bool follows_verbatim, const char *contract) {
  asngn_err e;
  size_t i;

  e = asngn_buf_appends(b, follows_verbatim ? "\n## This turn\n" : "## This turn\n");
  if (e == ASNGN_OK && t != NULL && t->user_msg != NULL) {
    e = asngn_buf_appends(b, "user: ");
    if (e == ASNGN_OK) e = asngn_buf_appends(b, t->user_msg);
    if (e == ASNGN_OK) e = asngn_buf_appendc(b, '\n');
  }
  if (e == ASNGN_OK && contract) e = asngn_buf_appends(b, contract);
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

asngn_err asngn_context_assemble(asngn_ctx *c, asngn_session *s, asngn_turn_state *t,
                                 const char *base_override, const char *instruction, int count_slot,
                                 asngn_prompt *out) {
  asngn_buf sys, usr, contract;
  asngn_err e = ASNGN_OK;
  const char *base = base_override != NULL ? base_override
                                           : (c->cfg.base_prompt != NULL ? c->cfg.base_prompt : "");
  char *verb_text = NULL;
  size_t verb_tokens = 0;
  char *asper_system = NULL, *asper_context = NULL;
  size_t asper_system_tokens = 0, asper_context_tokens = 0;
  asngn_context_trace trace = {0};

  memset(out, 0, sizeof *out);
  asngn_buf_init(&sys);
  asngn_buf_init(&usr);
  asngn_buf_init(&contract);
  e = asngn_context_trace_init(&trace, c, s, t, count_slot);
  if (e != ASNGN_OK) goto fail;
  e = asngn_work_render(s, &contract);
  if (e != ASNGN_OK) goto fail;
  e = asngn_context_trace_item(&trace, "system", 0, base, "included", "base_instructions");
  if (e == ASNGN_OK)
    e = asngn_context_trace_item(&trace, "working", 0, t ? t->user_msg : NULL, "included",
                                 "current_message");
  if (e == ASNGN_OK)
    e = asngn_context_trace_item(&trace, "working", 0, contract.data, "included",
                                 "acceptance_contract");
  if (e == ASNGN_OK)
    e = asngn_context_trace_item(&trace, "working", 0, instruction, "included",
                                 "phase_instruction");
  bool catalog_used =
      t && t->phase == ASNGN_PHASE_ACTION && t->catalog && *t->catalog && !t->opts.no_tools;
  if (e == ASNGN_OK)
    e = asngn_context_trace_item(&trace, "catalog", 0, t ? t->catalog : NULL,
                                 catalog_used ? "included" : "excluded", "phase_and_policy");
  if (e != ASNGN_OK) goto fail;

  /* Asper owns every persistent/historical memory zone. */
  if (c->asper_ok && s != NULL && t != NULL) {
    e = asngn_siblings_context(c, s, base,
                               t->retrieval_query ? t->retrieval_query
                               : t->user_msg      ? t->user_msg
                                                  : "",
                               (size_t)c->cfg.memory_history_tokens,
                               (size_t)c->cfg.memory_checkpoint_tokens, count_slot, &asper_system,
                               &asper_context, &asper_system_tokens, &asper_context_tokens);
    if (e != ASNGN_OK) goto fail;
    e = asngn_context_trace_item(&trace, "memory_system", 0, asper_system, "included",
                                 "asper_materialization");
    if (e == ASNGN_OK)
      e = asngn_context_trace_item(&trace, "memory_context", 0, asper_context, "included",
                                   "asper_materialization");
    if (e != ASNGN_OK) goto fail;
    e = asngn_buf_appends(&sys, asper_system);
    if (e != ASNGN_OK) goto fail;
    out->tok_system = asngn_context_tokens(c, count_slot, base);
    out->tok_memory =
        asper_system_tokens > out->tok_system ? asper_system_tokens - out->tok_system : 0;
    if (asper_context[0]) {
      e = asngn_buf_appends(&usr, asper_context);
      if (e == ASNGN_OK && asper_context[strlen(asper_context) - 1] != '\n')
        e = asngn_buf_appendc(&usr, '\n');
      if (e != ASNGN_OK) goto fail;
      out->tok_memory += asper_context_tokens;
    }
  } else {
    e = asngn_buf_appends(&sys, base);
    if (e != ASNGN_OK) goto fail;
    out->tok_system = asngn_context_tokens(c, count_slot, base);
  }

  /* zone 3: tool catalog (astools renders its own "## Tools" heading) */
  if (catalog_used) {
    e = asngn_buf_appends(&sys, "\n\n");
    if (e == ASNGN_OK) e = asngn_buf_appends(&sys, t->catalog);
    if (e != ASNGN_OK) goto fail;
    out->tok_catalog = asngn_context_tokens(c, count_slot, t->catalog);
  }

  /* zone 4: in-process turns when Asper is disabled */
  if (!c->asper_ok && s != NULL) {
    e = asngn_context_verbatim(c, s, t, count_slot, &verb_text, &verb_tokens, &trace);
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
      e = working_render(&work, t, instruction, first, usr.len > 0, contract.data);
      if (e != ASNGN_OK) break;
      rendered_tokens = asngn_context_tokens(c, count_slot, work.data);
      if (rendered_tokens <= (size_t)c->cfg.working_tokens || t == NULL || first >= t->work_n)
        break;
      asngn_buf_free(&work);
      first++;
    }
    if (e != ASNGN_OK) {
      asngn_buf_free(&work);
      goto fail;
    }
    if (first > 0 && !c->asper_ok) {
      asngn_log(c, ASNGN_LOG_WARN, "context", "working zone over budget: %zu item(s) trimmed",
                first);
      asngn_tele_emit(c, "guard", NULL, NULL, s != NULL ? s->slug : NULL, t->led.turn,
                      "{guard: \"working_trim\"}");
    }
    out->tok_working = rendered_tokens;
    for (size_t i = 0; t && e == ASNGN_OK && i < t->work_n; i++)
      e = asngn_context_trace_item(&trace, "evidence", i, t->work[i].text,
                                   c->asper_ok ? "delegated"
                                   : i < first ? "excluded"
                                               : "included",
                                   c->asper_ok ? "asper_owns_events"
                                   : i < first ? "working_budget"
                                               : "recent_suffix");
    if (e != ASNGN_OK) {
      asngn_buf_free(&work);
      goto fail;
    }
    e = asngn_buf_append(&usr, work.data, work.len);
    asngn_buf_free(&work);
    if (e != ASNGN_OK) goto fail;
  }

  out->system_text = asngn_buf_detach(&sys);
  out->user_text = asngn_buf_detach(&usr);
  if (!out->system_text || !out->user_text) {
    e = ASNGN_ERR_NOMEM;
    goto fail;
  }
  e = asngn_context_trace_finish(&trace, out);
  if (e != ASNGN_OK) goto fail;
  asngn_tele_emit(c, "context_selection", NULL, NULL, s ? s->slug : NULL, t ? t->led.turn : 0,
                  out->selection_json);
  asngn_context_trace_free(&trace);
  free(asper_system);
  free(asper_context);
  free(verb_text);
  asngn_buf_free(&sys);
  asngn_buf_free(&usr);
  asngn_buf_free(&contract);
  return ASNGN_OK;

fail:
  asngn_context_trace_free(&trace);
  free(asper_system);
  free(asper_context);
  free(verb_text);
  asngn_buf_free(&sys);
  asngn_buf_free(&usr);
  asngn_buf_free(&contract);
  asngn_prompt_free(out);
  return e;
}
