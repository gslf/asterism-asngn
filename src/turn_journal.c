/* Write-ahead turn journal. A fsynced committed frame is the decision;
 * transcript/ledger/manifest are idempotent projections. Recovery NEVER
 * repeats tool actions: an action without a commit has an unknown outcome. */
#include <stdlib.h>
#include <string.h>
#include "asngn_internal.h"
#include "work_state.h"
#include "xcdn.h"
#include "asper.h"

static asngn_err boundary(asngn_ctx *c, const char *point) {
  return c->fault && c->fault(c->fault_ud, point) ? ASNGN_ERR_IO : ASNGN_OK;
}
static asngn_err append(asngn_session *s, xcdn_value_t *v) {
  asngn_buf b;
  asngn_err e;
  xcdn_node_t *node = xcdn_node_new(v);
  if (!node) {
    xcdn_value_free(v);
    return ASNGN_ERR_NOMEM;
  }
  asngn_buf_init(&b);
  e = asngn_xnode_write(node, false, &b);
  xcdn_node_free(node);
  if (e == ASNGN_OK)
    e = asngn_wal_append(s->ctx, &s->journal_st, b.data, b.len);
  asngn_buf_free(&b);
  return e;
}
static xcdn_value_t *frame(const char *id, const char *state) {
  xcdn_value_t *v = xcdn_value_object();
  if (!v)
    return NULL;
  if (!asngn_xobj_put(v, "id", xcdn_value_string(id)) ||
      !asngn_xobj_put(v, "state", xcdn_value_string(state))) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}
asngn_err asngn_turn_journal(asngn_turn_state *t, const char *state,
                             const char *action) {
  xcdn_value_t *v = frame(t->span_root, state);
  asngn_err e;
  if (!v)
    return ASNGN_ERR_NOMEM;
  if (!asngn_xobj_put(v, "schema", xcdn_value_int(1)) ||
      !asngn_xobj_put(v, "work_revision", xcdn_value_int((int64_t)t->work_revision)) ||
      !asngn_xobj_put(v, "action_id", xcdn_value_string(t->action_id)) ||
      !asngn_xobj_put(v, "approval_id", xcdn_value_string(t->approval_id)) ||
      !asngn_xobj_put(v, "verification_snapshot", xcdn_value_string(t->verification_snapshot)) ||
      !asngn_xobj_put(v, "verification_ok", xcdn_value_bool(t->verification_ok)) ||
      !asngn_xobj_put(v, "at",
                      xcdn_value_int(asngn_clock_now(&t->s->ctx->clock))) ||
      !asngn_xobj_put(v, "input", xcdn_value_string(action ? action : "")) ||
      !asngn_xobj_put(v, "workspace",
                      xcdn_value_string(t->s->workspace.canonical_root)) ||
      !asngn_xobj_put(v, "commit", xcdn_value_string(t->s->workspace.head)) ||
      !asngn_xobj_put(
          v, "world_epoch",
          xcdn_value_int(
              (int64_t)t->s->world_epoch +
              (!strcmp(state, "action") && t->action_mutates ? 1 : 0)))) {
    xcdn_value_free(v);
    return ASNGN_ERR_NOMEM;
  }
  os_rwlock_wrlock(&t->s->lock);
  if (t->s->recovery_required) {
    xcdn_value_free(v);
    os_rwlock_wrunlock(&t->s->lock);
    return ASNGN_ERR_IO;
  }
  e = append(t->s, v);
  if (e == ASNGN_OK)
    e = boundary(t->s->ctx, state);
  /* Never append another frame behind a possibly incomplete write. */
  if (e != ASNGN_OK) t->s->recovery_required = true;
  os_rwlock_wrunlock(&t->s->lock);
  return e;
}
bool asngn_turn_outcome(const xcdn_value_t *v, asngn_err *outcome) {
  int64_t schema;
  if (!asngn_xint(asngn_xfield(v,"schema"),&schema) || schema != 1) return false;
  const char *name = asngn_xstr(asngn_xfield(v,"outcome"));
  for (int i = ASNGN_OK; name && i <= ASNGN_ERR_LIMIT; i++)
    if (!strcmp(name,asngn_err_name((asngn_err)i))) { *outcome = (asngn_err)i; return true; }
  return false;
}
asngn_err asngn_turn_finished(asngn_turn_state *t, asngn_err outcome) {
  asngn_session *s = t->s;
  if (s->recovery_required) return ASNGN_ERR_IO;
  if (outcome == ASNGN_OK && !t->tx_committed) return ASNGN_ERR_PROTOCOL;
  xcdn_value_t *v = frame(t->span_root,"finished");
  if (!v) return ASNGN_ERR_NOMEM;
  if (!asngn_xobj_put(v,"schema",xcdn_value_int(1)) ||
      !asngn_xobj_put(v,"outcome",xcdn_value_string(asngn_err_name(outcome))) ||
      !asngn_xobj_put(v,"committed",xcdn_value_bool(t->tx_committed)) ||
      !asngn_xobj_put(v,"answer",xcdn_value_string(!t->tx_committed && t->answer ? t->answer : ""))) {
    xcdn_value_free(v); return ASNGN_ERR_NOMEM;
  }
  asngn_err e = boundary(s->ctx,"before_finished");
  if (e == ASNGN_OK) { e = append(s,v); v = NULL; }
  xcdn_value_free(v);
  if (e == ASNGN_OK) e = boundary(s->ctx,"finished");
  if (e != ASNGN_OK) s->recovery_required = true;
  return e;
}
asngn_err asngn_turn_checkpoint(asngn_session *s, const char *text) {
  char *current = NULL;
  asngn_err e = ASNGN_OK;
  if (!text || !s->ctx->asper_ok)
    return ASNGN_OK;
  asper_err ae = asper_checkpoint_load(s->ctx->asper, s->slug, &current);
  if (ae != ASPER_OK || !current || strcmp(current, text))
    e = asngn_siblings_checkpoint(s->ctx, s->slug, text, NULL);
  asper_free(current);
  return e;
}
asngn_err asngn_turn_project(asngn_session *s, const xcdn_value_t *v) {
  asngn_turn u, a;
  asngn_ledger_entry led;
  xcdn_node_t node;
  asngn_err e = ASNGN_ERR_PARSE;
  const char *id = asngn_xstr(asngn_xfield(v, "id"));
  memset(&u, 0, sizeof u);
  memset(&a, 0, sizeof a);
  memset(&node, 0, sizeof node);
  if (!id || !asngn_uuid_valid(id))
    return ASNGN_ERR_PARSE;
  node.value = (xcdn_value_t *)asngn_xfield(v, "assistant");
  if (!asngn_turn_parse(&node, &a) || strcmp(a.turn_id, id))
    goto done;
  node.value = (xcdn_value_t *)asngn_xfield(v, "ledger");
  if (!asngn_ledger_parse(&node, &led) || strcmp(led.turn_id, id) ||
      led.turn != a.n)
    goto done;
  node.value = (xcdn_value_t *)asngn_xfield(v, "user");
  if (node.value) {
    if (!asngn_turn_parse(&node, &u) || strcmp(u.turn_id, id))
      goto done;
    e = asngn_session_append_turn(s, &u);
    if (e != ASNGN_OK)
      goto done;
    e = boundary(s->ctx, "project_user");
    if (e != ASNGN_OK)
      goto done;
  }
  e = asngn_session_append_turn(s, &a);
  if (e != ASNGN_OK)
    goto done;
  e = boundary(s->ctx, "project_assistant");
  if (e != ASNGN_OK)
    goto done;
  e = asngn_ledger_append(s, &led);
  if (e != ASNGN_OK)
    goto done;
  e = boundary(s->ctx, "project_ledger");
  if (e != ASNGN_OK)
    goto done;
  if (s->turns < a.n)
    s->turns = a.n;
  free(s->last_answer);
  s->last_answer = asngn_strdup(a.text);
  if (u.text) {
    free(s->last_user_msg);
    s->last_user_msg = asngn_strdup(u.text);
  }
  s->last_answer_turn = a.n;
  s->last_capped = led.capped;
  e = asngn_session_save_manifest(s);
  if (e == ASNGN_OK)
    e = boundary(s->ctx, "project_manifest");
done:
  free(u.text);
  free(a.text);
  return e;
}
asngn_err asngn_turn_commit(asngn_turn_state *t, const asngn_turn *answer) {
  asngn_session *s = t->s;
  xcdn_value_t *v = frame(t->span_root, "committed");
  (void)asngn_verification_current(t);
  xcdn_node_t *node;
  asngn_buf b;
  asngn_err e;
  if (!v)
    return ASNGN_ERR_NOMEM;
  if (!t->continuation && s->log_n > t->log_before &&
      !asngn_xobj_put_node(v, "user", asngn_turn_node(&s->log[t->log_before])))
    goto oom;
  if (!asngn_xobj_put_node(v, "assistant", asngn_turn_node(answer)) ||
      !asngn_xobj_put_node(v, "ledger", asngn_ledger_node(&t->led)))
    goto oom;
  {
    asngn_buf cp;
    asngn_buf_init(&cp);
    asngn_work_state work = {0};
    if (s->work) work = s->work->state;
    asngn_work_refresh(&work,s->workspace.fingerprint);
    e = asngn_buf_printf(
        &cp,
        "goal: %s\nturn_state: committed\ntask_state: %s\nwork_revision: %llu\nphase: response\nturn_id: %s\n"
        "artifact_written: %s\nverification_attempted: %s\nverification_ok: %s",
        t->user_msg ? t->user_msg : "",
        work.revision ? (work.succeeded ? "succeeded" : "incomplete") : "unconfirmed",
        (unsigned long long)work.revision, t->span_root,
        t->artifact_written ? "true" : "false",
        t->verification_attempted ? "true" : "false",
        t->verification_ok ? "true" : "false");
    if (e == ASNGN_OK &&
        !asngn_xobj_put(v, "checkpoint", xcdn_value_string(cp.data)))
      e = ASNGN_ERR_NOMEM;
    asngn_buf_free(&cp);
    if (e != ASNGN_OK)
      goto oom;
  }
  node = xcdn_node_new(v);
  if (!node)
    goto oom;
  asngn_buf_init(&b);
  e = asngn_xnode_write(node, false, &b);
  if (e == ASNGN_OK)
    e = boundary(s->ctx, "before_commit");
  if (e == ASNGN_OK) {
    /* Once append is attempted, fsync errors have an uncertain outcome.
     * Require reopen/recovery instead of allowing another live turn. */
    s->recovery_required = true;
    e = asngn_wal_append(s->ctx, &s->journal_st, b.data, b.len);
    if (e == ASNGN_OK) {
      t->tx_committed = true;
      e = boundary(s->ctx, "committed");
      if (e == ASNGN_OK)
        e = asngn_turn_project(s, v);
      if (e == ASNGN_OK)
        e = asngn_turn_checkpoint(s,asngn_xstr(asngn_xfield(v,"checkpoint")));
      if (e == ASNGN_OK)
        s->recovery_required = false;
    }
  }
  asngn_buf_free(&b);
  xcdn_node_free(node);
  return e;
oom:
  xcdn_value_free(v);
  return ASNGN_ERR_NOMEM;
}
