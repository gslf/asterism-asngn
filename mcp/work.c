/* Host contract transport. There is deliberately no endpoint to submit proof. */
#include "work.h"
#include <stdlib.h>
#include <string.h>

const char MCP_WORK_SCHEMA[] =
  "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
  "\"session\":{\"type\":\"string\"},\"mode\":{\"enum\":[\"get\",\"define\",\"invalidate\"]},"
  "\"expected_revision\":{\"type\":\"integer\",\"minimum\":0},"
  "\"definition\":{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"goal\",\"constraints\",\"criteria\"],\"properties\":{"
  "\"goal\":{\"type\":\"string\",\"maxLength\":512},\"constraints\":{\"type\":\"string\",\"maxLength\":512},"
  "\"criteria\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":16,\"items\":{"
  "\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"id\",\"requirement\",\"command\",\"path\",\"adapter\",\"depends_on\"],\"properties\":{"
  "\"id\":{\"type\":\"string\",\"maxLength\":32},\"requirement\":{\"type\":\"string\",\"maxLength\":256},"
  "\"command\":{\"enum\":[\"build\",\"test\",\"lint\",\"diagnostics\"]},"
  "\"path\":{\"type\":\"string\",\"maxLength\":256},"
  "\"adapter\":{\"enum\":[\"auto\",\"cmake\",\"cargo\",\"npm\",\"python\"]},"
  "\"depends_on\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":32767}}}}}}}}";

static int string(const jx_value *o, const char *k, char *out, size_t cap) {
  const jx_value *v = jx_object_get(o,k);
  const char *s = jx_string_value(v);
  if (!s || jx_string_length(v) != strlen(s) || strlen(s) >= cap) return 0;
  memcpy(out,s,strlen(s)+1); return 1;
}
static int definition(const jx_value *o, asngn_work_definition *d) {
  if (jx_object_count(o) != 3 || !string(o,"goal",d->goal,sizeof d->goal) ||
      !string(o,"constraints",d->constraints,sizeof d->constraints)) return 0;
  const jx_value *a = jx_object_get(o,"criteria");
  d->count = jx_array_len(a);
  if (!d->count || d->count > ASNGN_WORK_CRITERIA_MAX) return 0;
  for (size_t i = 0; i < d->count; i++) {
    const jx_value *row = jx_array_at(a,i), *deps = jx_object_get(row,"depends_on");
    asngn_work_criterion *c = &d->criteria[i];
    if (jx_object_count(row) != 6 || !string(row,"id",c->id,sizeof c->id) ||
        !string(row,"requirement",c->requirement,sizeof c->requirement) ||
        !string(row,"command",c->command,sizeof c->command) ||
        !string(row,"path",c->path,sizeof c->path) || !string(row,"adapter",c->adapter,sizeof c->adapter) ||
        !jx_is_int(deps) || jx_int_value(deps) < 0 || jx_int_value(deps) > UINT32_MAX) return 0;
    c->depends_on = (uint32_t)jx_int_value(deps);
  }
  return 1;
}
asngn_err mcp_work_status(asngn_session *s, uint64_t revision, jx_value *o) {
  asngn_work_state *w = NULL;
  asngn_err e = asngn_session_work_get(s,&w);
  const char *name = e == ASNGN_ERR_NOT_FOUND ? "unconfirmed" : e != ASNGN_OK ? "unavailable" :
      revision != UINT64_MAX && w->revision != revision ? "superseded" :
      w->succeeded ? "succeeded" : "incomplete";
  int ok = jx_object_set(o,"task_state",jx_string(name)) == 0;
  if (w) {
    ok &= jx_object_set(o,"work_revision",jx_int((long long)w->revision)) == 0;
    ok &= jx_object_set(o,"work_sequence",jx_int((long long)w->sequence)) == 0;
    ok &= jx_object_set(o,"goal",jx_string(w->definition.goal)) == 0;
    ok &= jx_object_set(o,"constraints",jx_string(w->definition.constraints)) == 0;
    jx_value *checks = jx_array();
    ok &= checks != NULL;
    for (size_t i = 0; ok && i < w->definition.count; i++) {
      const asngn_work_criterion *c = &w->definition.criteria[i];
      const asngn_work_proof *p = &w->proofs[i];
      jx_value *check = jx_object();
      ok = check != NULL;
      ok &= jx_object_set(check,"id",jx_string(c->id)) == 0;
      ok &= jx_object_set(check,"requirement",jx_string(c->requirement)) == 0;
      ok &= jx_object_set(check,"command",jx_string(c->command)) == 0;
      ok &= jx_object_set(check,"path",jx_string(c->path)) == 0;
      ok &= jx_object_set(check,"adapter",jx_string(c->adapter)) == 0;
      ok &= jx_object_set(check,"depends_on",jx_int(c->depends_on)) == 0;
      ok &= jx_object_set(check,"status",jx_string(asngn_proof_status_name(p->status))) == 0;
      ok &= jx_object_set(check,"action_id",jx_string(p->action_id)) == 0;
      ok &= jx_object_set(check,"snapshot",jx_string(p->snapshot)) == 0;
      ok &= jx_object_set(check,"receipt_sha256",jx_string(p->receipt_sha256)) == 0;
      ok &= jx_array_push(checks,check) == 0;
    }
    ok &= jx_object_set(o,"criteria",checks) == 0;
  }
  asngn_free(w);
  return ok ? ASNGN_OK : ASNGN_ERR_NOMEM;
}
asngn_err mcp_work_request(asngn_session *s, const jx_value *args, jx_value **out) {
  *out = NULL;
  if (jx_typeof(args) != JX_OBJECT) return ASNGN_ERR_INVALID;
  for (size_t i = 0; i < jx_object_count(args); i++) {
    const char *key = jx_object_key_at(args,i);
    if (strcmp(key,"session") && strcmp(key,"mode") && strcmp(key,"expected_revision") &&
        strcmp(key,"definition")) return ASNGN_ERR_INVALID;
  }
  char mode[16] = "get";
  if (jx_object_get(args,"mode") && !string(args,"mode",mode,sizeof mode)) return ASNGN_ERR_INVALID;
  const jx_value *revision = jx_object_get(args,"expected_revision"), *d = jx_object_get(args,"definition");
  asngn_err e = ASNGN_OK;
  if (!strcmp(mode,"get")) {
    if (revision || d) return ASNGN_ERR_INVALID;
  } else {
    if (!jx_is_int(revision) || jx_int_value(revision) < 0) return ASNGN_ERR_INVALID;
    uint64_t expected = (uint64_t)jx_int_value(revision);
    if (!strcmp(mode,"define")) {
      asngn_work_definition spec = {0};
      if (!definition(d,&spec)) return ASNGN_ERR_INVALID;
      e = asngn_session_work_define(s,expected,&spec);
    } else if (!strcmp(mode,"invalidate") && !d) e = asngn_session_work_invalidate(s,expected);
    else return ASNGN_ERR_INVALID;
  }
  if (e != ASNGN_OK) return e;
  *out = jx_object();
  if (!*out) return ASNGN_ERR_NOMEM;
  e = mcp_work_status(s,UINT64_MAX,*out);
  if (e != ASNGN_OK) { jx_free(*out); *out = NULL; }
  return e;
}
