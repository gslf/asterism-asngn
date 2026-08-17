/*
 * context.c — zoned prompt assembly.
 *
 * The assembled prompt is a fixed sequence of zones, split across the
 * (system, user) message pair every backend consumes:
 *
 *   system_text: [1 system+directive] [2 memory] [3 catalog] [4 summary]
 *   user_text:   [5 verbatim] [6 working + current message + instruction]
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

/* Select the verbatim zone: pinned turns first (chronological),
 * then the most recent unpinned turns, chosen newest-first under the
 * remaining budget, rendered chronologically. The current in-flight user
 * message is excluded (it renders in the working zone). Whole turns
 * only. */
static asngn_err verbatim_build(asngn_ctx *c, asngn_session *s,
                                const asngn_turn_state *t, int count_slot,
                                char **out_text, size_t *out_tokens) {
  size_t budget = (size_t)c->cfg.verbatim_tokens;
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
    if (!tr->pinned || tr->folded) continue;
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
    if (tr->pinned || tr->folded) continue;
    {
      asngn_buf one;
      size_t cost;
      asngn_buf_init(&one);
      e = verb_render(&one, tr);
      if (e != ASNGN_OK) { asngn_buf_free(&one); goto out; }
      cost = zone_tokens(c, count_slot, one.data);
      asngn_buf_free(&one);
      if (used + cost > budget) break; /* older turns are larger holes */
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
                                 const char *memory_block,
                                 const char *instruction, int count_slot,
                                 asngn_prompt *out) {
  asngn_buf sys, usr;
  asngn_err e = ASNGN_OK;
  const char *base = c->cfg.base_prompt != NULL ? c->cfg.base_prompt : "";
  char *verb_text = NULL;
  size_t verb_tokens = 0;
  size_t i;

  memset(out, 0, sizeof *out);
  asngn_buf_init(&sys);
  asngn_buf_init(&usr);

  /* zone 1+2: base prompt (+ memory block, which embeds the base) */
  if (memory_block != NULL && memory_block[0] != '\0') {
    e = asngn_buf_appends(&sys, memory_block);
    if (e != ASNGN_OK) goto fail;
    if (asngn_str_has_prefix(memory_block, base)) {
      out->tok_system = zone_tokens(c, count_slot, base);
      out->tok_memory =
          zone_tokens(c, count_slot, memory_block + strlen(base));
    } else {
      out->tok_memory = zone_tokens(c, count_slot, memory_block);
    }
  } else {
    e = asngn_buf_appends(&sys, base);
    if (e != ASNGN_OK) goto fail;
    out->tok_system = zone_tokens(c, count_slot, base);
  }

  /* zone 3: tool catalog (astools renders its own "## Tools" heading) */
  if (t != NULL && t->catalog != NULL && t->catalog[0] != '\0' &&
      !t->opts.no_tools) {
    e = asngn_buf_appends(&sys, "\n\n");
    if (e == ASNGN_OK) e = asngn_buf_appends(&sys, t->catalog);
    if (e != ASNGN_OK) goto fail;
    out->tok_catalog = zone_tokens(c, count_slot, t->catalog);
  }

  /* zone 4: rolling summary */
  if (s != NULL && s->summary != NULL && s->summary[0] != '\0') {
    size_t stoks = zone_tokens(c, count_slot, s->summary);
    if (stoks <= (size_t)c->cfg.summary_tokens) {
      e = asngn_buf_appends(&sys, "\n\n## Conversation summary\n");
      if (e == ASNGN_OK) e = asngn_buf_appends(&sys, s->summary);
      if (e != ASNGN_OK) goto fail;
      out->tok_summary = stoks;
    } else {
      asngn_log(c, ASNGN_LOG_WARN, "context",
                "summary over budget (%zu > %d tok); omitted pending "
                "re-compaction", stoks, c->cfg.summary_tokens);
    }
  }

  /* zone 5: verbatim turns */
  if (s != NULL) {
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
    size_t wtoks = 0;
    asngn_buf_init(&work);
    e = asngn_buf_appends(&work, usr.len > 0 ? "\n## This turn\n"
                                             : "## This turn\n");
    if (e == ASNGN_OK && t != NULL && t->user_msg != NULL) {
      e = asngn_buf_appends(&work, "user: ");
      if (e == ASNGN_OK) e = asngn_buf_appends(&work, t->user_msg);
      if (e == ASNGN_OK) e = asngn_buf_appendc(&work, '\n');
    }
    if (e == ASNGN_OK && t != NULL) {
      /* whole-item working budget: drop oldest items when over;
       * digestion keeps single items small, so drops are rare */
      size_t total = 0, first = 0;
      for (i = 0; i < t->work_n; i++) total += t->work[i].tokens;
      while (first < t->work_n &&
             total > (size_t)c->cfg.working_tokens) {
        total -= t->work[first].tokens;
        first++;
      }
      if (first > 0) {
        asngn_log(c, ASNGN_LOG_WARN, "context",
                  "working zone over budget: %zu item(s) trimmed", first);
        asngn_tele_emit(c, "guard", NULL, NULL,
                        s != NULL ? s->slug : NULL,
                        t->led.turn, "{guard: \"working_trim\"}");
      }
      for (i = first; i < t->work_n && e == ASNGN_OK; i++) {
        e = asngn_buf_appends(&work, t->work[i].text);
        if (e == ASNGN_OK && t->work[i].text[0] != '\0' &&
            t->work[i].text[strlen(t->work[i].text) - 1] != '\n')
          e = asngn_buf_appendc(&work, '\n');
        if (e == ASNGN_OK) wtoks += t->work[i].tokens;
      }
    }
    if (e != ASNGN_OK) {
      asngn_buf_free(&work);
      goto fail;
    }
    e = asngn_buf_append(&usr, work.data, work.len);
    asngn_buf_free(&work);
    if (e != ASNGN_OK) goto fail;
    if (t != NULL && t->user_msg != NULL)
      wtoks += zone_tokens(c, count_slot, t->user_msg);
    out->tok_working = wtoks;
  }
  if (instruction != NULL && instruction[0] != '\0') {
    e = asngn_buf_appendc(&usr, '\n');
    if (e == ASNGN_OK) e = asngn_buf_appends(&usr, instruction);
    if (e == ASNGN_OK) e = asngn_buf_appendc(&usr, '\n');
    if (e != ASNGN_OK) goto fail;
    out->tok_working += zone_tokens(c, count_slot, instruction);
  }

  out->system_text = asngn_buf_detach(&sys);
  out->user_text = asngn_buf_detach(&usr);
  free(verb_text);
  asngn_buf_free(&sys);
  asngn_buf_free(&usr);
  if (out->system_text == NULL || out->user_text == NULL) {
    asngn_prompt_free(out);
    return ASNGN_ERR_NOMEM;
  }
  return ASNGN_OK;

fail:
  free(verb_text);
  asngn_buf_free(&sys);
  asngn_buf_free(&usr);
  asngn_prompt_free(out);
  return e;
}
