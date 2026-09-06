/* Stream the authoritative journal; conversation projection never replays a tool. */
#include "asngn_internal.h"
#include "xcdn.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  asngn_session *session;
  char pending[37], committed[37], action[37];
  size_t actions;
  char *checkpoint;
} recovery;

static void interrupted(recovery *r) {
  if (*r->pending) {
    r->session->interrupted_turns++;
    r->session->uncertain_actions += r->actions;
  }
  r->pending[0] = r->action[0] = 0;
  r->actions = 0;
}
static asngn_err observe(void *ud, const char *text, size_t bytes) {
  recovery *r = ud;
  xcdn_document_t *doc = xcdn_parse_str(text, bytes, NULL);
  const xcdn_value_t *v = doc && doc->values_len == 1 ? doc->values[0]->value : NULL;
  const char *state = asngn_xstr(asngn_xfield(v, "state")), *id = asngn_xstr(asngn_xfield(v, "id"));
  asngn_err e = ASNGN_ERR_PARSE;
  if (!state || !asngn_uuid_valid(id))
    goto done;
  if (!strcmp(state, "started")) {
    interrupted(r);
    r->committed[0] = 0;
    memcpy(r->pending, id, 37);
    e = ASNGN_OK;
  } else if (!strcmp(state, "finished")) {
    bool committed;
    asngn_err outcome;
    const char *answer = asngn_xstr(asngn_xfield(v, "answer"));
    if (!asngn_xbool(asngn_xfield(v, "committed"), &committed) || !answer ||
        !asngn_turn_outcome(v, &outcome) || (outcome == ASNGN_OK && !committed) ||
        strcmp(id, committed ? r->committed : r->pending))
      goto done;
    /* A recorded failure is finished; its unobserved effects can remain uncertain. */
    r->session->uncertain_actions += r->actions;
    r->pending[0] = r->action[0] = r->committed[0] = 0;
    r->actions = 0;
    e = ASNGN_OK;
  } else if (!strcmp(id, r->pending)) {
    const char *action = asngn_xstr(asngn_xfield(v, "action_id"));
    if (!strcmp(state, "action")) {
      if (*r->action || !asngn_uuid_valid(action))
        goto done;
      memcpy(r->action, action, 37);
      r->actions++;
      int64_t epoch;
      if (asngn_xint(asngn_xfield(v, "world_epoch"), &epoch) && epoch > 0 &&
          (uint64_t)epoch > r->session->world_epoch)
        r->session->world_epoch = (uint64_t)epoch;
      e = ASNGN_OK;
    } else if (!strcmp(state, "observed")) {
      if (!action || !*r->action || strcmp(action, r->action))
        goto done;
      r->actions--;
      r->action[0] = 0;
      e = ASNGN_OK;
    } else if (!strcmp(state, "committed")) {
      e = asngn_turn_project(r->session, v);
      if (e != ASNGN_OK)
        goto done;
      const char *checkpoint = asngn_xstr(asngn_xfield(v, "checkpoint"));
      char *copy = checkpoint ? asngn_strdup(checkpoint) : NULL;
      if (checkpoint && !copy) {
        e = ASNGN_ERR_NOMEM;
        goto done;
      }
      free(r->checkpoint);
      r->checkpoint = copy;
      memcpy(r->committed, id, 37);
      r->pending[0] = r->action[0] = 0;
      r->actions = 0;
    }
  }
done:
  xcdn_document_free(doc);
  return e;
}
asngn_err asngn_turn_recover(asngn_session *s) {
  recovery r = {.session = s};
  s->interrupted_turns = s->uncertain_actions = 0;
  asngn_err e = asngn_wal_visit(s->ctx, s->journal_st.path, 16u * 1024u * 1024u, observe, &r);
  interrupted(&r);
  if (e == ASNGN_OK)
    e = asngn_turn_checkpoint(s, r.checkpoint);
  if (e == ASNGN_OK)
    e = asngn_session_save_manifest(s);
  free(r.checkpoint);
  s->recovery_required = e != ASNGN_OK;
  return e;
}
