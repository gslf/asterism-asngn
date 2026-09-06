/* Execute a validated decision through the common runtime gates. */
#include "execution.h"
#include "blob.h"
#include <stdlib.h>
#include <string.h>

/* One `step` event per parsed decision: the action plus the model's
 * declared rationale (THINK notes stand in for their own why). Redacted
 * and flattened like the confirm event's args snippet, so the payload
 * cannot break the event line. */
static void step_tele(asngn_ctx *c, asngn_turn_state *t, const asngn_step *st) {
  char safe[121];
  char data[192];
  char *masked = NULL;
  size_t nm = 0, si = 0;
  const char *src = st->why != NULL ? st->why : (st->kind == ASNGN_STEP_THINK ? st->text : NULL);
  if (src != NULL) {
    if (asngn_redact(src, strlen(src), &masked, &nm) == ASNGN_OK && masked != NULL)
      src = masked;
    else
      src = "[redaction unavailable]";
  }
  for (; src != NULL && *src != '\0' && si + 1 < sizeof safe; src++) {
    unsigned char ch = (unsigned char)*src;
    if (ch == '"' || ch == '\\')
      safe[si++] = '\'';
    else
      safe[si++] = ch < 0x20 ? ' ' : (char)ch;
  }
  while (si > 0 && !asngn_utf8_valid(safe, si))
    si--;
  safe[si] = '\0';
  snprintf(data, sizeof data, "{action: \"%s\", why: \"%s\"}", asngn_step_name(st->kind), safe);
  asngn_tele_emit(c, "step", t->span_root, NULL, t->s->slug, t->led.turn, data);
  if (safe[0] != '\0') asngn_turn_stream_emit(t, ASNGN_STREAM_REASONING, safe);
  free(masked);
}

asngn_err asngn_action_apply(asngn_ctx *c, asngn_turn_state *t, asngn_step *st, bool call_now,
                             bool think_now, bool discover_now, bool *done) {
  asngn_session *s = t->s;
  asngn_err e = ASNGN_OK;
  *done = false;
  if (t->phase != ASNGN_PHASE_ACTION)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "decision applied outside the action phase");
  if (t->cancel) return ASNGN_ERR_CANCELLED;
  if (asngn_turn_expired(c, t)) return ASNGN_ERR_TIMEOUT;
  if (t->steps >= c->cfg.max_steps) return ASNGN_ERR_LIMIT;
  t->steps++;
  t->led.duration_ms = 0; /* set at commit */
  step_tele(c, t, st);

  switch (st->kind) {
  case ASNGN_STEP_ANSWER:
    /* Hard phase boundary: a generate turn cannot enter the response
     * phase until a content-bearing write/edit actually succeeded. */
    if (asngn_generation_needs_artifact(c, t)) {
      asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn,
                      "{guard: \"outcome_gate\"}");
      os_rwlock_wrlock(&c->lock);
      c->stats.guard_trips++;
      os_rwlock_wrunlock(&c->lock);
      return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                          "decision selected ANSWER before producing the "
                          "required workspace artifact");
    }
    *done = true;
    return ASNGN_OK;
  case ASNGN_STEP_DISCOVER:
    t->thinks_row = 0;
    if (!discover_now) {
      t->futile_row++;
      e = asngn_work_push(c, t, "[notice] tool discovery is disabled for this turn");
    } else {
      e = asngn_tools_select(c, t, st->text);
      if (e == ASNGN_OK)
        e = asngn_work_push(
            c, t, "[notice] tool selection replaced; use the current catalog for the next action");
      t->futile_row = 0;
    }
    if (e != ASNGN_OK) {
      return e;
    }
    break;
  case ASNGN_STEP_CLARIFY:
    t->clarify = true;
    t->answer = st->text;
    st->text = NULL;
    *done = true;
    return ASNGN_OK;
  case ASNGN_STEP_THINK:
    t->thinks_total++;
    if (!think_now) {
      /* Defense in depth for a backend that ignored the constrained
       * grammar.  Keep THINK unavailable until it chooses a real step. */
      t->futile_row++;
      t->think_mute = true;
    } else {
      t->thinks_row++;
      t->futile_row = 0;
      asngn_buf b;
      asngn_buf_init(&b);
      if (asngn_buf_printf(&b, "THINK: %s", st->text) == ASNGN_OK) asngn_work_push(c, t, b.data);
      asngn_buf_free(&b);
      if (t->thinks_row >= c->cfg.think_limit) {
        /* Do not discard analysis or force the response phase.  Remove
         * THINK for exactly one constrained decision so the model must use
         * the work it just did. */
        t->think_mute = true;
        asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn,
                        "{guard: \"think_limit\"}");
        os_rwlock_wrlock(&c->lock);
        c->stats.guard_trips++;
        os_rwlock_wrunlock(&c->lock);
        asngn_work_push(c, t,
                        "[notice] thinking budget complete \xE2\x80\x94 "
                        "use the analysis and take the next useful step");
      }
    }
    break;
  case ASNGN_STEP_RECALL: {
    char *block = NULL;
    uint8_t rkey[32];
    t->thinks_row = 0;
    /* recall guard: an identical question, or a fourth recall in one
     * turn, only floods the working zone (trimming out tool results)
     * without adding information — same failure mode as THINK loops */
    asngn_sha256(st->text != NULL ? st->text : "", st->text != NULL ? strlen(st->text) : 0, rkey);
    {
      size_t ri;
      bool seen = false;
      for (ri = 0; ri < t->recall_keys_n; ri++) {
        if (memcmp(rkey, t->recall_keys[ri], 32) == 0) {
          seen = true;
          break;
        }
      }
      if (seen || t->recalls_total >= 3) {
        t->futile_row++;
        asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn,
                        "{guard: \"recall_limit\"}");
        os_rwlock_wrlock(&c->lock);
        c->stats.guard_trips++;
        os_rwlock_wrunlock(&c->lock);
        asngn_work_push(c, t,
                        "[notice] memory already consulted \xE2\x80"
                        "\x94 act or answer");
        break;
      }
    }
    t->futile_row = 0;
    memcpy(t->recall_keys[t->recall_keys_n++], rkey, 32);
    t->recalls_total++;
    if (asngn_siblings_recall(c, st->text, &block) == ASNGN_OK && block != NULL) {
      char *masked = asngn_context_text(s, block);
      if (masked != NULL) {
        char *digested = NULL;
        if (asngn_digest_item(c, s, t, "recall", masked, strlen(masked), &t->led.sv_digest,
                              &t->led.gt_aux, &digested) == ASNGN_OK &&
            digested != NULL) {
          asngn_work_data(c, t, digested);
          free(digested);
        } else {
          asngn_work_data(c, t, masked);
        }
        free(masked);
      }
      free(block);
    }
    asngn_tele_emit(c, "recall", t->span_root, NULL, s->slug, t->led.turn, NULL);
    break;
  }
  case ASNGN_STEP_OPEN: {
    t->thinks_row = 0;
    if (st->blob_n < 1 || (size_t)st->blob_n > s->blobs_n) {
      t->futile_row++;
      asngn_work_push(c, t, "[notice] no such blob");
    } else {
      asngn_blob *b = &s->blobs[st->blob_n - 1];
      char *slice = NULL;
      size_t used = 0, free_tok, free_chars, wi;
      for (wi = 0; wi < t->work_n; wi++)
        used += t->work[wi].tokens;
      free_tok = (size_t)c->cfg.working_tokens > used ? (size_t)c->cfg.working_tokens - used : 0;
      if (free_tok < 64) free_tok = 64; /* always make some progress */
      free_chars = free_tok * 4;
      asngn_err read = asngn_blob_read(c, b, st->blob_offset, free_chars, &slice);
      if (read == ASNGN_OK && slice != NULL) {
        t->futile_row = 0;
        asngn_work_data(c, t, slice);
        free(slice);
      } else {
        t->futile_row++;
        asngn_work_push(
            c, t,
            read == ASNGN_OK
                ? "[notice] end of blob"
                : "[notice] evidence range unavailable: check its byte offset and UTF-8 boundary");
      }
    }
    break;
  }
  case ASNGN_STEP_CALL:
    t->thinks_row = 0;
    if (!call_now) {
      t->futile_row++;
      asngn_work_push(c, t, "[notice] CALL is unavailable for this decision");
      break;
    }
    /* synthesize the call line for astools' authoritative parser:
     * the action object carries the call as its input field */
    {
      asngn_buf cl;
      asngn_buf_init(&cl);
      e = asngn_buf_printf(&cl, "CALL %s.%s %s", st->call_ref, st->call_cmd, st->call_args);
      if (e == ASNGN_OK) e = asngn_call_execute(c, t, cl.data, st->fallback);
      asngn_buf_free(&cl);
    }
    return e;
  default:
    return ASNGN_ERR_PROTOCOL;
  }
  return ASNGN_OK;
}
