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

static int string(const asmodel_json_value *o, const char *k, char *out, size_t cap) {
  const asmodel_json_value *v = asmodel_json_object_get(o,k);
  const char *s = asmodel_json_string_value(v);
  if (!s || asmodel_json_string_length(v) != strlen(s) || strlen(s) >= cap) return 0;
  memcpy(out,s,strlen(s)+1); return 1;
}
static int definition(const asmodel_json_value *o, asngn_work_definition *d) {
  if (asmodel_json_object_count(o) != 3 || !string(o,"goal",d->goal,sizeof d->goal) ||
      !string(o,"constraints",d->constraints,sizeof d->constraints)) return 0;
  const asmodel_json_value *a = asmodel_json_object_get(o,"criteria");
  d->count = asmodel_json_array_len(a);
  if (!d->count || d->count > ASNGN_WORK_CRITERIA_MAX) return 0;
  for (size_t i = 0; i < d->count; i++) {
    const asmodel_json_value *row = asmodel_json_array_at(a,i), *deps = asmodel_json_object_get(row,"depends_on");
    asngn_work_criterion *c = &d->criteria[i];
    if (asmodel_json_object_count(row) != 6 || !string(row,"id",c->id,sizeof c->id) ||
        !string(row,"requirement",c->requirement,sizeof c->requirement) ||
        !string(row,"command",c->command,sizeof c->command) ||
        !string(row,"path",c->path,sizeof c->path) || !string(row,"adapter",c->adapter,sizeof c->adapter) ||
        !asmodel_json_is_int(deps) || asmodel_json_int_value(deps) < 0 || asmodel_json_int_value(deps) > UINT32_MAX) return 0;
    c->depends_on = (uint32_t)asmodel_json_int_value(deps);
  }
  return 1;
}
asngn_err mcp_work_status(asngn_session *s, uint64_t revision, asmodel_json_value *o) {
  asngn_work_state *w = NULL;
  asngn_err e = asngn_session_work_get(s,&w);
  const char *name = e == ASNGN_ERR_NOT_FOUND ? "unconfirmed" : e != ASNGN_OK ? "unavailable" :
      revision != UINT64_MAX && w->revision != revision ? "superseded" :
      w->succeeded ? "succeeded" : "incomplete";
  int ok = asmodel_json_object_set(o,"task_state",asmodel_json_string(name)) == 0;
  if (w) {
    ok &= asmodel_json_object_set(o,"work_revision",asmodel_json_int((long long)w->revision)) == 0;
    ok &= asmodel_json_object_set(o,"work_sequence",asmodel_json_int((long long)w->sequence)) == 0;
    ok &= asmodel_json_object_set(o,"goal",asmodel_json_string(w->definition.goal)) == 0;
    ok &= asmodel_json_object_set(o,"constraints",asmodel_json_string(w->definition.constraints)) == 0;
    asmodel_json_value *checks = asmodel_json_array();
    ok &= checks != NULL;
    for (size_t i = 0; ok && i < w->definition.count; i++) {
      const asngn_work_criterion *c = &w->definition.criteria[i];
      const asngn_work_proof *p = &w->proofs[i];
      asmodel_json_value *check = asmodel_json_object();
      ok = check != NULL;
      ok &= asmodel_json_object_set(check,"id",asmodel_json_string(c->id)) == 0;
      ok &= asmodel_json_object_set(check,"requirement",asmodel_json_string(c->requirement)) == 0;
      ok &= asmodel_json_object_set(check,"command",asmodel_json_string(c->command)) == 0;
      ok &= asmodel_json_object_set(check,"path",asmodel_json_string(c->path)) == 0;
      ok &= asmodel_json_object_set(check,"adapter",asmodel_json_string(c->adapter)) == 0;
      ok &= asmodel_json_object_set(check,"depends_on",asmodel_json_int(c->depends_on)) == 0;
      ok &= asmodel_json_object_set(check,"status",asmodel_json_string(asngn_proof_status_name(p->status))) == 0;
      ok &= asmodel_json_object_set(check,"action_id",asmodel_json_string(p->action_id)) == 0;
      ok &= asmodel_json_object_set(check,"snapshot",asmodel_json_string(p->snapshot)) == 0;
      ok &= asmodel_json_object_set(check,"receipt_sha256",asmodel_json_string(p->receipt_sha256)) == 0;
      ok &= asmodel_json_array_push(checks,check) == 0;
    }
    ok &= asmodel_json_object_set(o,"criteria",checks) == 0;
  }
  asngn_free(w);
  return ok ? ASNGN_OK : ASNGN_ERR_NOMEM;
}
asngn_err mcp_work_request(asngn_session *s, const asmodel_json_value *args, asmodel_json_value **out) {
  *out = NULL;
  if (asmodel_json_typeof(args) != ASMODEL_JSON_OBJECT) return ASNGN_ERR_INVALID;
  for (size_t i = 0; i < asmodel_json_object_count(args); i++) {
    const char *key = asmodel_json_object_key_at(args,i);
    if (strcmp(key,"session") && strcmp(key,"mode") && strcmp(key,"expected_revision") &&
        strcmp(key,"definition")) return ASNGN_ERR_INVALID;
  }
  char mode[16] = "get";
  if (asmodel_json_object_get(args,"mode") && !string(args,"mode",mode,sizeof mode)) return ASNGN_ERR_INVALID;
  const asmodel_json_value *revision = asmodel_json_object_get(args,"expected_revision"), *d = asmodel_json_object_get(args,"definition");
  asngn_err e = ASNGN_OK;
  if (!strcmp(mode,"get")) {
    if (revision || d) return ASNGN_ERR_INVALID;
  } else {
    if (!asmodel_json_is_int(revision) || asmodel_json_int_value(revision) < 0) return ASNGN_ERR_INVALID;
    uint64_t expected = (uint64_t)asmodel_json_int_value(revision);
    if (!strcmp(mode,"define")) {
      asngn_work_definition spec = {0};
      if (!definition(d,&spec)) return ASNGN_ERR_INVALID;
      e = asngn_session_work_define(s,expected,&spec);
    } else if (!strcmp(mode,"invalidate") && !d) e = asngn_session_work_invalidate(s,expected);
    else return ASNGN_ERR_INVALID;
  }
  if (e != ASNGN_OK) return e;
  *out = asmodel_json_object();
  if (!*out) return ASNGN_ERR_NOMEM;
  e = mcp_work_status(s,UINT64_MAX,*out);
  if (e != ASNGN_OK) { asmodel_json_free(*out); *out = NULL; }
  return e;
}
