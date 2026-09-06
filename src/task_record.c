/* Read one durable task without restoring a decoder, approval or external effect. */
#include "asngn_internal.h"
#include "xcdn.h"
#include <stdlib.h>
#include <string.h>

void asngn_task_record_free(asngn_task_record *r) {
  if (!r)
    return;
  free(r->input);
  free(r->answer);
  free(r->last_action);
  free(r->last_observation);
  free(r);
}
static asngn_err copy(char **to, const char *text) {
  if (!text)
    return ASNGN_ERR_PARSE;
  char *s = asngn_strdup(text);
  if (!s)
    return ASNGN_ERR_NOMEM;
  free(*to);
  *to = s;
  return ASNGN_OK;
}
static const char *field(const xcdn_value_t *v, const char *key) {
  return asngn_xstr(asngn_xfield(v, key));
}
static asngn_err observe(void *ud, const char *text, size_t bytes) {
  asngn_task_record *r = ud;
  xcdn_document_t *doc = xcdn_parse_str(text, bytes, NULL);
  const xcdn_value_t *v = doc && doc->values_len == 1 ? doc->values[0]->value : NULL;
  const char *id = field(v, "id"), *state = field(v, "state");
  asngn_err e = ASNGN_ERR_PARSE;
  if (!asngn_uuid_valid(id) || !state)
    goto done;
  if (strcmp(id, r->task_id)) {
    e = ASNGN_OK;
    goto done;
  }
  if (!strcmp(state, "started")) {
    int64_t revision;
    if (r->input || !asngn_xint(asngn_xfield(v, "work_revision"), &revision) || revision < 0)
      goto done;
    r->work_revision = (uint64_t)revision;
    e = copy(&r->input, field(v, "input"));
  } else if (r->input && r->state != ASNGN_TASK_FINISHED) {
    if (!strcmp(state, "action") && !r->turn_committed && !r->action_uncertain) {
      const char *action = field(v, "action_id");
      if (!asngn_uuid_valid(action))
        goto done;
      memcpy(r->action_id, action, 37);
      r->action_uncertain = 1;
      free(r->last_observation);
      r->last_observation = NULL;
      e = copy(&r->last_action, field(v, "input"));
    } else if (!strcmp(state, "observed") && !r->turn_committed && r->action_uncertain) {
      const char *action = field(v, "action_id");
      if (!action || strcmp(action, r->action_id))
        goto done;
      r->action_uncertain = 0;
      e = copy(&r->last_observation, field(v, "input"));
    } else if (!strcmp(state, "committed") && !r->turn_committed) {
      r->state = ASNGN_TASK_COMMITTED;
      r->turn_committed = 1;
      e = copy(&r->answer, field(asngn_xfield(v, "assistant"), "text"));
    } else if (!strcmp(state, "finished")) {
      bool committed;
      if (!asngn_turn_outcome(v, &r->outcome) || !field(v, "answer") ||
          !asngn_xbool(asngn_xfield(v, "committed"), &committed) ||
          committed != !!r->turn_committed || (r->outcome == ASNGN_OK && !committed))
        goto done;
      r->state = ASNGN_TASK_FINISHED;
      e = r->turn_committed ? ASNGN_OK : copy(&r->answer, field(v, "answer"));
    }
  }
done:
  xcdn_document_free(doc);
  return e;
}
asngn_err asngn_session_task_read(asngn_session *s, const char *id, asngn_task_record **out) {
  if (!out)
    return ASNGN_ERR_INVALID;
  *out = NULL;
  if (!s || !asngn_uuid_valid(id))
    return ASNGN_ERR_INVALID;
  os_rwlock_rdlock(&s->lock);
  if (s->busy || s->recovery_required) {
    os_rwlock_rdunlock(&s->lock);
    return ASNGN_ERR_BUSY;
  }
  asngn_task_record *r = calloc(1, sizeof *r);
  asngn_err e = ASNGN_ERR_NOMEM;
  if (r) {
    memcpy(r->task_id, id, 37);
    e = asngn_wal_inspect(s->ctx, s->journal_st.path, 16u * 1024u * 1024u, observe, r);
    if (e == ASNGN_OK && !r->input)
      e = ASNGN_ERR_NOT_FOUND;
  }
  os_rwlock_rdunlock(&s->lock);
  if (e == ASNGN_OK)
    *out = r;
  else
    asngn_task_record_free(r);
  return e;
}
