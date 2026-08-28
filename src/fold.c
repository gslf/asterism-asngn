/*
 * fold.c — folding and summary maintenance.
 *
 * When the verbatim zone exceeds its budget, the oldest unpinned turns
 * fold into the rolling summary in user+assistant pairs until occupancy
 * falls to <= 70% of the budget. One fold is one compressor call; when
 * the compressor is unavailable, folds degrade to head-tail extraction
 * (counted as summary debt and retried later). Folding changes the
 * prompt, never the record: folded turns stay in the transcript with
 * folded: true.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

/* embedded verbatim. */
static const char FOLD_INSTRUCTION[] =
    "You compress the older part of a conversation so an assistant can "
    "keep working with less text. You will see the current summary and "
    "the turns to fold into it.\n"
    "\n"
    "Rewrite the summary so that it:\n"
    "- keeps decisions, facts, names, numbers, file paths, error "
    "messages,\n"
    "  and open questions;\n"
    "- drops greetings, fillers, and superseded attempts;\n"
    "- never invents anything that is not in the input;\n"
    "- stays under the given length, as plain prose, no headers.\n"
    "\n"
    "Answer with the new summary text only.";

/* First sentence of a text, capped at 1024 bytes (UTF-8 safe), for the
 * extractive fallback. */
static char *first_sentence(const char *text) {
  size_t i, end = 0;
  size_t len = strlen(text);
  for (i = 0; i < len; i++) {
    char ch = text[i];
    if (ch == '\n') { end = i; break; }
    if ((ch == '.' || ch == '!' || ch == '?') &&
        (i + 1 == len || text[i + 1] == ' ' || text[i + 1] == '\n')) {
      end = i + 1;
      break;
    }
  }
  if (end == 0) end = len;
  if (end > 1024) {
    end = 1024;
    while (end > 0 && ((unsigned char)text[end] & 0xC0) == 0x80) end--;
  }
  return asngn_strndup(text, end);
}

/* Last sentence of a text, capped at 1024 bytes. */
static char *last_sentence(const char *text) {
  size_t len = strlen(text);
  size_t start = 0, i;
  /* trim trailing whitespace for the boundary scan */
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' '))
    len--;
  for (i = len; i > 1; i--) {
    char ch = text[i - 2]; /* boundary char before the final run */
    if (ch == '\n' ||
        ((ch == '.' || ch == '!' || ch == '?') && text[i - 1] == ' ')) {
      start = i - 1;
      break;
    }
  }
  while (start < len && text[start] == ' ') start++;
  if (len - start > 1024) start = len - 1024;
  while (start < len && ((unsigned char)text[start] & 0xC0) == 0x80)
    start++;
  return asngn_strndup(text + start, len - start);
}

/* Occupancy of the would-be verbatim zone in tokens. */
static size_t fold_occupancy(asngn_ctx *c, asngn_session *s, int slot) {
  size_t i, total = 0;
  for (i = 0; i < s->log_n; i++) {
    const asngn_turn *t = &s->log[i];
    int n;
    if (t->folded) continue;
    n = asngn_models_count_tokens(c, slot, t->text);
    total += (n > 0 ? (size_t)n : 0) + 3; /* "role: " + newline overhead */
  }
  return total;
}

static int fold_count_slot(asngn_ctx *c) {
  int slot = asngn_models_slot_for_role(c, ASNGN_ROLE_GENERATOR);
  if (slot < 0) slot = asngn_models_slot_for_role(c, ASNGN_ROLE_PLANNER);
  return slot;
}

bool asngn_fold_needed(asngn_ctx *c, asngn_session *s) {
  bool needed;
  os_rwlock_rdlock(&s->lock);
  needed = fold_occupancy(c, s, fold_count_slot(c)) >
           (size_t)c->cfg.verbatim_tokens;
  os_rwlock_rdunlock(&s->lock);
  return needed;
}

/* One compressor generation; returns NULL on failure (caller falls back). */
static char *fold_compress(asngn_ctx *c, const char *user_prompt,
                           int max_tokens, size_t *aux_tokens) {
  int slot = asngn_models_slot_for_role(c, ASNGN_ROLE_COMPRESSOR);
  char *text = NULL;
  int tin = 0, tout = 0;
  asngn_err e;
  if (slot < 0) return NULL;
  e = asngn_models_generate(c, slot, ASNGN_TASK_COMPRESS, FOLD_INSTRUCTION,
                            user_prompt, NULL, max_tokens, NULL, NULL,
                            &c->bg_cancel, &text, &tin, &tout);
  if (e != ASNGN_OK) {
    free(text);
    return NULL;
  }
  if (aux_tokens != NULL) *aux_tokens += (size_t)(tout > 0 ? tout : 0);
  if (text != NULL) {
    /* single-block plain prose: trim surrounding whitespace */
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' '))
      text[--len] = '\0';
  }
  if (text == NULL || text[0] == '\0') {
    free(text);
    return NULL;
  }
  return text;
}

/* Fold one user+assistant pair (indices u, a in the log).
 *
 * Locking contract (D14 freeze fix): the caller has claimed the session
 * (s->busy) and does NOT hold s->lock. The prompt is snapshotted under a
 * read hold, the compressor call runs with no session lock held — on CPU
 * it can take minutes, and holding s->lock across it froze every caller
 * of the session API, including asngn_submit on the TUI thread — and the
 * apply re-validates the pair under the write hold. */
static asngn_err fold_pair(asngn_ctx *c, asngn_session *s, size_t u,
                           size_t a, size_t *aux_tokens) {
  asngn_buf prompt;
  char *fresh = NULL;
  asngn_err e = ASNGN_OK;

  asngn_buf_init(&prompt);
  os_rwlock_rdlock(&s->lock);
  e = asngn_buf_printf(&prompt,
                       "Current summary:\n%s\n\nTurns to fold:\n"
                       "user: %s\nassistant: %s\n\n"
                       "Length limit: about %d tokens.",
                       s->summary != NULL && s->summary[0] != '\0'
                           ? s->summary
                           : "(empty)",
                       s->log[u].text,
                       a < s->log_n ? s->log[a].text : "(none)",
                       c->cfg.fold_tokens);
  os_rwlock_rdunlock(&s->lock);
  if (e != ASNGN_OK) {
    asngn_buf_free(&prompt);
    return e;
  }
  fresh = fold_compress(c, prompt.data, c->cfg.fold_tokens, aux_tokens);
  asngn_buf_free(&prompt);
  if (fresh == NULL && c->bg_cancel) {
    /* the compressor was cancelled (yield to a submit, or shutdown):
     * discard this pair untouched — the re-queued fold redoes it */
    return ASNGN_OK;
  }

  os_rwlock_wrlock(&s->lock);
  /* re-validate: a /pin may have claimed either half while the
   * compressor ran (pins take s->lock, not the busy flag) — a pinned
   * turn must not fold, so drop this fold and let the next scan skip
   * the pair. */
  if (s->log[u].pinned || s->log[u].folded ||
      (a < s->log_n && (s->log[a].pinned || s->log[a].folded))) {
    os_rwlock_wrunlock(&s->lock);
    free(fresh);
    return ASNGN_OK;
  }

  if (fresh != NULL) {
    free(s->summary);
    s->summary = fresh;
  } else {
    /* extractive fallback: head of the user turn, tail of the answer */
    char *head = first_sentence(s->log[u].text);
    char *tail = a < s->log_n ? last_sentence(s->log[a].text) : NULL;
    asngn_buf nb;
    asngn_buf_init(&nb);
    if (s->summary != NULL && s->summary[0] != '\0') {
      e = asngn_buf_appends(&nb, s->summary);
      if (e == ASNGN_OK) e = asngn_buf_appendc(&nb, '\n');
    }
    if (e == ASNGN_OK && head != NULL) {
      e = asngn_buf_appends(&nb, head);
      if (e == ASNGN_OK) e = asngn_buf_appendc(&nb, '\n');
    }
    if (e == ASNGN_OK && tail != NULL) {
      e = asngn_buf_appends(&nb, tail);
      if (e == ASNGN_OK) e = asngn_buf_appendc(&nb, '\n');
    }
    free(head);
    free(tail);
    if (e != ASNGN_OK) {
      os_rwlock_wrunlock(&s->lock);
      asngn_buf_free(&nb);
      return e;
    }
    free(s->summary);
    s->summary = asngn_buf_detach(&nb);
    asngn_buf_free(&nb);
    if (s->summary == NULL) {
      os_rwlock_wrunlock(&s->lock);
      return ASNGN_ERR_NOMEM;
    }
    s->summary_debt++;
    asngn_log(c, ASNGN_LOG_WARN, "context",
              "%s: extractive fold fallback (debt %zu)", s->slug,
              s->summary_debt);
  }

  s->log[u].folded = true;
  if (a < s->log_n) s->log[a].folded = true;
  os_rwlock_wrunlock(&s->lock);
  asngn_tele_emit(c, "fold", NULL, NULL, s->slug, s->turns,
                  fresh != NULL ? "{mode: \"compressor\"}"
                                : "{mode: \"extractive\"}");
  return ASNGN_OK;
}

asngn_err asngn_fold_recompact(asngn_ctx *c, asngn_session *s,
                               size_t *aux_tokens) {
  asngn_buf prompt;
  char *fresh;
  int target = c->cfg.summary_tokens / 2;
  asngn_err e;

  /* same locking contract as fold_pair: snapshot under the read hold,
   * compress unlocked, apply + save under the write hold */
  os_rwlock_rdlock(&s->lock);
  if (s->summary == NULL || s->summary[0] == '\0') {
    os_rwlock_rdunlock(&s->lock);
    return ASNGN_OK;
  }
  asngn_buf_init(&prompt);
  e = asngn_buf_printf(&prompt,
                       "Current summary:\n%s\n\nTurns to fold:\n(none — "
                       "rewrite the summary to at most half its length, "
                       "keeping the most important facts)\n\n"
                       "Length limit: about %d tokens.",
                       s->summary, target);
  os_rwlock_rdunlock(&s->lock);
  if (e != ASNGN_OK) {
    asngn_buf_free(&prompt);
    return e;
  }
  fresh = fold_compress(c, prompt.data, target, aux_tokens);
  asngn_buf_free(&prompt);
  if (fresh == NULL && c->bg_cancel) return ASNGN_OK; /* cancelled */
  os_rwlock_wrlock(&s->lock);
  if (s->summary == NULL || s->summary[0] == '\0') {
    /* emptied while the compressor ran; nothing left to shrink */
    os_rwlock_wrunlock(&s->lock);
    free(fresh);
    return ASNGN_OK;
  }
  if (fresh != NULL) {
    free(s->summary);
    s->summary = fresh;
  } else {
    /* extractive shrink: keep the leading lines that fit the target */
    int slot = fold_count_slot(c);
    size_t keep = strlen(s->summary);
    while (keep > 0) {
      char saved = s->summary[keep];
      int n;
      s->summary[keep] = '\0';
      n = asngn_models_count_tokens(c, slot, s->summary);
      s->summary[keep] = saved;
      if (n >= 0 && (size_t)n <= (size_t)target) break;
      /* drop the last line */
      while (keep > 0 && s->summary[keep - 1] != '\n') keep--;
      if (keep > 0) keep--;
    }
    s->summary[keep] = '\0';
    s->summary_debt++;
  }
  e = asngn_session_save_summary(s);
  os_rwlock_wrunlock(&s->lock);
  return e;
}

asngn_err asngn_fold_run(asngn_ctx *c, asngn_session *s, bool aggressive,
                         size_t *aux_tokens) {
  int slot = fold_count_slot(c);
  size_t budget = (size_t)c->cfg.verbatim_tokens;
  size_t floor = aggressive ? 0 : (budget * 7) / 10;
  bool changed = false;
  asngn_err e = ASNGN_OK;

  /* Contract: the caller has claimed the session (s->busy set, so no
   * turn can run) and does NOT hold s->lock; every model call below
   * happens with the session lock released (fold_pair / fold_recompact
   * take their own short read/write holds). */
  for (;;) {
    size_t u = (size_t)-1, a = (size_t)-1, i;
    bool stop;
    if (c->bg_cancel) break; /* engine shutting down */
    os_rwlock_rdlock(&s->lock);
    if (fold_occupancy(c, s, slot) <= floor || s->log_n == 0) {
      os_rwlock_rdunlock(&s->lock);
      break;
    }
    /* oldest unpinned, unfolded turn opens the pair */
    for (i = 0; i < s->log_n; i++) {
      if (s->log[i].folded || s->log[i].pinned) continue;
      u = i;
      break;
    }
    stop = u == (size_t)-1 ||           /* everything pinned or folded */
           (aggressive && u + 1 >= s->log_n); /* keep the live tail    */
    if (!stop) {
      for (i = u + 1; i < s->log_n; i++) {
        if (s->log[i].folded || s->log[i].pinned) continue;
        if (strcmp(s->log[i].role, "assistant") == 0) a = i;
        break; /* only the immediate next unfolded turn pairs up */
      }
    }
    os_rwlock_rdunlock(&s->lock);
    if (stop) break;
    e = fold_pair(c, s, u, a != (size_t)-1 ? a : s->log_n, aux_tokens);
    if (e != ASNGN_OK) return e;
    changed = true;
  }

  if (changed) {
    /* the summary breathes: shrink when it grew past its budget */
    int n;
    os_rwlock_rdlock(&s->lock);
    n = asngn_models_count_tokens(c, slot, s->summary);
    os_rwlock_rdunlock(&s->lock);
    if (n > c->cfg.summary_tokens && !c->bg_cancel) {
      e = asngn_fold_recompact(c, s, aux_tokens);
      if (e != ASNGN_OK) return e;
    }
    os_rwlock_wrlock(&s->lock);
    e = asngn_session_save_summary(s);
    if (e == ASNGN_OK) e = asngn_session_rewrite_transcript(s);
    if (e == ASNGN_OK) e = asngn_session_save_manifest(s);
    os_rwlock_wrunlock(&s->lock);
    if (e != ASNGN_OK) return e;
    asngn_log(c, ASNGN_LOG_INFO, "context", "%s: fold complete", s->slug);
  }
  return ASNGN_OK;
}
