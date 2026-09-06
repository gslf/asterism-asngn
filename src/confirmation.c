/* Persist review data and decisions before allowing a mutating action. */
#include "approval.h"
#include "asmodel_json.h"
#include <stdlib.h>
#include <string.h>

static asngn_err publish(asngn_ctx *c, asngn_turn_state *t, const asngn_tool_note *note) {
  asngn_approval *a = NULL;
  asngn_err e = asngn_approval_get(t->s, &a);
  if (e != ASNGN_OK) return e;
  size_t n = strlen(a->arguments);
  if (n > 160) n = 160;
  while (n && !asngn_utf8_valid(a->arguments, n))
    n--;
  char *snippet = asngn_strndup(a->arguments, n);
  asmodel_json_value *v = snippet ? asmodel_json_string(snippet) : NULL;
  char *quoted = asmodel_json_write(v, 0);
  asmodel_json_free(v);
  free(snippet);
  asngn_buf b;
  asngn_buf_init(&b);
  e = quoted
          ? asngn_buf_printf(&b,
                             "{confirm_id:u\"%s\",tool:\"%s\",command:\"%s\",args:%s,"
                             "arguments_sha256:\"%s\",package_sha256:\"%s\",snapshot:\"%s\","
                             "destructive:%s,read_only:%s}",
                             a->id, a->tool_ref, a->command, quoted, a->arguments_sha256,
                             a->package_sha256, a->snapshot, note->destructive ? "true" : "false",
                             note->read_only ? "true" : "false")
          : ASNGN_ERR_NOMEM;
  if (e == ASNGN_OK)
    asngn_tele_emit(c, "confirm", t->span_root, NULL, t->s->slug, t->led.turn, b.data);
  asngn_buf_free(&b);
  free(quoted);
  asngn_approval_free(a);
  return e;
}

asngn_err asngn_call_confirm(asngn_ctx *c, asngn_turn_state *t,
                             const astools_selected_command *tool, const char *args,
                             const asngn_tool_note *note, bool *allowed, const char **deny_code) {
  asngn_session *s = t->s;
  char grant[65], id[37], snapshot[65];
  *allowed = false;
  *deny_code = "astools/denied";
  t->approval_id[0] = 0;
  if ((note->read_only && !note->destructive) || c->cfg.autoconfirm == ASNGN_CONFIRM_ALLOW ||
      (c->cfg.autoconfirm == ASNGN_CONFIRM_PROMPT &&
       t->security_profile == ASNGN_SECURITY_AUTOMATION_CI && !note->destructive)) {
    *allowed = true;
    return ASNGN_OK;
  }
  if (c->cfg.autoconfirm == ASNGN_CONFIRM_DENY) {
    *deny_code = "asngn/confirm-required";
    return ASNGN_OK;
  }
  asngn_approval_grant_key(t, tool, grant);
  for (size_t i = 0; i < s->allow_n; i++)
    if (!strcmp(s->allow[i], grant)) {
      *allowed = true;
      return ASNGN_OK;
    }

  asngn_err e = asngn_approval_prepare(t, tool, args, id, snapshot);
  if (e != ASNGN_OK) return e;
  os_mutex_lock(&c->confirm.mu);
  snprintf(c->confirm.id, sizeof c->confirm.id, "%s", id);
  c->confirm.decided = c->confirm.allow = c->confirm.session_wide = 0;
  c->confirm.session = s;
  os_mutex_unlock(&c->confirm.mu);
  e = publish(c, t, note);
  os_mutex_lock(&c->confirm.mu);
  while (e == ASNGN_OK && !c->confirm.decided && !t->cancel && !asngn_turn_expired(c, t)) {
    os_rwlock_rdlock(&s->lock);
    bool broken = s->recovery_required;
    os_rwlock_rdunlock(&s->lock);
    if (broken) {
      e = ASNGN_ERR_IO;
      break;
    }
    os_cond_timedwait(&c->confirm.cv, &c->confirm.mu, 100);
  }
  bool decided = c->confirm.decided != 0, allow = c->confirm.allow != 0;
  int wide = c->confirm.session_wide;
  c->confirm.id[0] = 0;
  c->confirm.decided = 0;
  c->confirm.session = NULL;
  os_mutex_unlock(&c->confirm.mu);

  asngn_approval_status status = ASNGN_APPROVAL_INTERRUPTED;
  if (e == ASNGN_OK && decided && !t->cancel && !asngn_turn_expired(c, t)) {
    if (!allow) return ASNGN_OK; /* The denial is already durable. */
    char current[65];
    e = asngn_approval_snapshot(s, current);
    if (e == ASNGN_OK && !strcmp(snapshot, current) &&
        astools_selection_validate(t->tool_selection, tool->tool, args) == ASTOOLS_OK)
      status = ASNGN_APPROVAL_CONSUMED;
    else {
      status = ASNGN_APPROVAL_INVALIDATED;
      *deny_code = "asngn/approval-stale";
    }
  } else {
    *deny_code = t->cancel ? "astools/cancelled" : "asngn/confirm-timeout";
  }
  os_rwlock_wrlock(&s->lock);
  asngn_approval *a = s->approvals->current;
  asngn_err saved = ASNGN_OK;
  if (a->status == ASNGN_APPROVAL_PENDING || a->status == ASNGN_APPROVAL_APPROVED)
    saved = asngn_approval_transition(s, status, a->session_wide);
  os_rwlock_wrunlock(&s->lock);
  if (e != ASNGN_OK) return e;
  if (saved != ASNGN_OK) return saved;
  *allowed = status == ASNGN_APPROVAL_CONSUMED;
  if (*allowed) memcpy(t->approval_id, id, 37);
  if (*allowed && wide) {
    char **next = realloc(s->allow, (s->allow_n + 1) * sizeof *next);
    if (next) {
      s->allow = next;
      s->allow[s->allow_n] = asngn_strdup(grant);
      if (s->allow[s->allow_n]) s->allow_n++;
    }
  }
  return ASNGN_OK;
}
