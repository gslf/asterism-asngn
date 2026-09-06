/* Archived observations are distinct from live jobs and current acceptance proof. */
#include "recovery.h"
#include "work.h"
#include <stdlib.h>
#include <string.h>

static asmodel_json_value *string(const char *text) {
  return asmodel_json_string(text ? text : "");
}
asngn_err mcp_task_recover(asngn_session *s, const char *id, asmodel_json_value **out) {
  asngn_task_record *r = NULL;
  *out = NULL;
  asngn_err e = asngn_session_task_read(s, id, &r);
  if (e != ASNGN_OK)
    return e;
  asmodel_json_value *o = asmodel_json_object();
  int ok = o != NULL;
#define PUT(key, value) (ok &= asmodel_json_object_set(o, key, value) == 0)
#define STRING(key, value) PUT(key, string(value))
  STRING("task_id", r->task_id);
  STRING("state", r->state == ASNGN_TASK_FINISHED    ? "finished"
                  : r->state == ASNGN_TASK_COMMITTED ? "turn_committed"
                                                     : "interrupted");
  PUT("turn_committed", asmodel_json_bool(r->turn_committed));
  PUT("action_uncertain", asmodel_json_bool(r->action_uncertain));
  PUT("admitted_work_revision", asmodel_json_int((long long)r->work_revision));
  PUT("execution_resumed", asmodel_json_bool(0));
  PUT("events_replayed", asmodel_json_bool(0));
  STRING("input", r->input);
  STRING("answer", r->answer);
  STRING("action_id", r->action_id);
  STRING("last_action", r->last_action);
  STRING("last_observation", r->last_observation);
  if (r->state == ASNGN_TASK_FINISHED)
    STRING("outcome", asngn_err_name(r->outcome));
#undef STRING
#undef PUT
  if (ok)
    e = mcp_work_status(s, r->work_revision, o);
  else
    e = ASNGN_ERR_NOMEM;
  /* Leave room for the enclosing JSON text envelope; never truncate evidence. */
  char *encoded = e == ASNGN_OK ? asmodel_json_write(o, 0) : NULL;
  if (e == ASNGN_OK)
    e = !encoded                               ? ASNGN_ERR_NOMEM
        : strlen(encoded) > 3u * 1024u * 1024u ? ASNGN_ERR_LIMIT
                                               : ASNGN_OK;
  free(encoded);
  asngn_task_record_free(r);
  if (e == ASNGN_OK)
    *out = o;
  else
    asmodel_json_free(o);
  return e;
}
