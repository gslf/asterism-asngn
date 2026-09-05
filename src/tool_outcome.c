/* Versioned call identity and model-visible results and errors. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

/* An identical command is only identical while the inputs it can observe are
 * unchanged.  The live fingerprint catches editor/tool filesystem changes;
 * world_epoch also catches successful mutating tools whose effects live
 * outside the fingerprinted tree.  Records are advanced to the post-call
 * state after a successful mutation, so a command cannot unblock an immediate
 * repeat merely by changing its own outputs. */
void asngn_call_state_key(const uint8_t intent[32], const uint8_t workspace[32],
                          uint64_t world_epoch, uint8_t out[32]) {
  static const char domain[] = "asngn-call-state-v2";
  asngn_sha256_ctx h;
  asngn_sha256_init(&h);
  asngn_sha256_update(&h, domain, sizeof domain);
  asngn_sha256_update(&h, intent, 32);
  asngn_sha256_update(&h, workspace, 32);
  asngn_sha256_update(&h, &world_epoch, sizeof world_epoch);
  asngn_sha256_final(&h, out);
}

/* ── CALL step ───────────────────────────────────────────────────────── */

/* Canonical form of an args object for the identical-call key:
 * whitespace outside string literals is dropped, so `{path: "a"}` and
 * `{ path:"a" }` hash equal. Key order is not normalized — the grammar
 * emits params in manifest order, so reorderings do not occur in
 * practice; astools' validator is the semantic authority. */
char *asngn_call_args(const char *args) {
  asngn_buf b;
  bool in_str = false, esc = false;
  const char *p;
  char *out;
  asngn_buf_init(&b);
  for (p = args != NULL ? args : "{}"; *p != '\0'; p++) {
    char ch = *p;
    if (in_str) {
      if (asngn_buf_appendc(&b, ch) != ASNGN_OK) goto oom;
      if (esc)
        esc = false;
      else if (ch == '\\')
        esc = true;
      else if (ch == '"')
        in_str = false;
      continue;
    }
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
    if (ch == '"') in_str = true;
    if (asngn_buf_appendc(&b, ch) != ASNGN_OK) goto oom;
  }
  out = asngn_buf_detach(&b);
  asngn_buf_free(&b);
  return out;
oom:
  asngn_buf_free(&b);
  return NULL;
}

/* Push one call outcome, prefixed with the originating call so the
 * models can tell WHICH arguments produced which RESULT/ERROR — without
 * the echo, an answer pass facing one failed and one successful call of
 * the same command cannot attribute the data to the right arguments. */
asngn_err asngn_call_outcome(asngn_ctx *c, asngn_turn_state *t, const char *ref, const char *cmd,
                             const char *args, const char *line) {
  asngn_buf b;
  asngn_err e;
  asngn_buf_init(&b);
  e = asngn_buf_printf(&b, "CALL %s.%s %s -> %s", ref, cmd, args != NULL ? args : "{}", line);
  if (e == ASNGN_OK) {
    char *masked = asngn_context_text(t->s, b.data);
    if (masked != NULL) {
      e = asngn_work_data(c, t, masked);
      free(masked);
    } else {
      e = ASNGN_ERR_NOMEM;
    }
  }
  asngn_buf_free(&b);
  return e;
}

asngn_err asngn_call_error(asngn_ctx *c, asngn_turn_state *t, const char *ref, const char *cmd,
                           const char *args, const char *code, const char *message) {
  struct astools_result_s r;
  char *line = NULL;
  asngn_err e;
  memset(&r, 0, sizeof r);
  r.ok = 0;
  r.error_code = (char *)code;
  r.error_message = (char *)message;
  if (c->astools != NULL && astools_call_format(c->astools, ref, cmd, &r, &line) == ASTOOLS_OK &&
      line != NULL) {
    e = asngn_call_outcome(c, t, ref, cmd, args, line);
    astools_free(line);
  } else {
    asngn_buf b;
    asngn_buf_init(&b);
    e = asngn_buf_printf(&b, "ERROR %s.%s {code: \"%s\",message: \"%s\"}", ref, cmd, code, message);
    if (e == ASNGN_OK) e = asngn_call_outcome(c, t, ref, cmd, args, b.data);
    asngn_buf_free(&b);
  }
  return e;
}

/* Echo the model's own declared fallback after a failed call, so the
 * next decision pass is steered by the plan the model itself committed
 * to when it issued the action object. */
void asngn_call_fallback(asngn_ctx *c, asngn_turn_state *t, const char *fallback) {
  asngn_buf b;
  if (fallback == NULL || fallback[0] == '\0') return;
  asngn_buf_init(&b);
  if (asngn_buf_printf(&b,
                       "[notice] the call failed \xE2\x80\x94 your "
                       "declared fallback: %s",
                       fallback) == ASNGN_OK)
    asngn_work_push(c, t, b.data);
  asngn_buf_free(&b);
}
