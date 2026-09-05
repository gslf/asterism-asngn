/* Full bounded state frames keep replay independent of conversation commits. */
#include "work_state.h"
#include <string.h>

static bool put(xcdn_value_t *v, const char *k, const char *s) {
  return asngn_xobj_put(v,k,xcdn_value_string(s));
}
xcdn_value_t *asngn_work_encode(const asngn_work_state *w) {
  xcdn_value_t *v = xcdn_value_object(), *a = xcdn_value_array();
  bool ok = v && a;
  ok = ok && asngn_xobj_put(v,"schema",xcdn_value_int(1)) &&
       asngn_xobj_put(v,"revision",xcdn_value_int((int64_t)w->revision)) &&
       asngn_xobj_put(v,"sequence",xcdn_value_int((int64_t)w->sequence)) &&
       put(v,"goal",w->definition.goal) && put(v,"constraints",w->definition.constraints);
  for (size_t i = 0; ok && i < w->definition.count; i++) {
    const asngn_work_criterion *c = &w->definition.criteria[i];
    const asngn_work_proof *p = &w->proofs[i];
    xcdn_value_t *row = xcdn_value_object();
    ok = row && put(row,"id",c->id) && put(row,"requirement",c->requirement) &&
         put(row,"command",c->command) && put(row,"path",c->path) && put(row,"adapter",c->adapter) &&
         asngn_xobj_put(row,"depends_on",xcdn_value_int(c->depends_on)) &&
         put(row,"status",asngn_proof_status_name(p->status)) && put(row,"action_id",p->action_id) &&
         put(row,"snapshot",p->snapshot) && put(row,"receipt_sha256",p->receipt_sha256);
    if (ok) ok = asngn_xarr_push(a,row);
    else xcdn_value_free(row);
  }
  if (ok) { ok = asngn_xobj_put(v,"criteria",a); a = NULL; }
  xcdn_value_free(a);
  if (!ok) { xcdn_value_free(v); v = NULL; }
  return v;
}
static bool object(const xcdn_value_t *v, size_t count) {
  if (!v || v->type != XCDN_VAL_OBJECT || v->data.object.len != count) return false;
  for (size_t i = 0; i < count; i++)
    for (size_t j = 0; j < i; j++)
      if (!strcmp(v->data.object.entries[i].key,v->data.object.entries[j].key)) return false;
  return true;
}
static bool string(const xcdn_value_t *v, const char *key, char *out, size_t cap) {
  const char *s = asngn_xstr(asngn_xfield(v,key));
  if (!s || strlen(s) >= cap) return false;
  memcpy(out,s,strlen(s)+1); return true;
}
static bool number(const xcdn_value_t *v, const char *key, uint64_t *out) {
  int64_t n;
  if (!asngn_xint(asngn_xfield(v,key),&n) || n < 0) return false;
  *out = (uint64_t)n; return true;
}
static bool hash(const char *s) {
  return strlen(s) == 64 && strspn(s,"0123456789abcdef") == 64;
}
asngn_err asngn_work_decode(const xcdn_value_t *v, asngn_work_state *w) {
  uint64_t schema;
  memset(w,0,sizeof *w);
  if (!object(v,6) || !number(v,"schema",&schema) || schema != 1 ||
      !number(v,"revision",&w->revision) || !w->revision ||
      !number(v,"sequence",&w->sequence) || !w->sequence ||
      !string(v,"goal",w->definition.goal,sizeof w->definition.goal) ||
      !string(v,"constraints",w->definition.constraints,sizeof w->definition.constraints)) return ASNGN_ERR_PARSE;
  const xcdn_value_t *a = asngn_xfield(v,"criteria");
  if (!a || a->type != XCDN_VAL_ARRAY || !a->data.array.len ||
      a->data.array.len > ASNGN_WORK_CRITERIA_MAX) return ASNGN_ERR_PARSE;
  w->definition.count = a->data.array.len;
  for (size_t i = 0; i < w->definition.count; i++) {
    const xcdn_value_t *row = a->data.array.items[i]->value;
    asngn_work_criterion *c = &w->definition.criteria[i];
    asngn_work_proof *p = &w->proofs[i];
    uint64_t deps;
    char status[32];
    if (!object(row,10) || !string(row,"id",c->id,sizeof c->id) ||
        !string(row,"requirement",c->requirement,sizeof c->requirement) ||
        !string(row,"command",c->command,sizeof c->command) ||
        !string(row,"path",c->path,sizeof c->path) || !string(row,"adapter",c->adapter,sizeof c->adapter) ||
        !number(row,"depends_on",&deps) || deps > UINT32_MAX ||
        !string(row,"status",status,sizeof status) ||
        !string(row,"action_id",p->action_id,sizeof p->action_id) ||
        !string(row,"snapshot",p->snapshot,sizeof p->snapshot) ||
        !string(row,"receipt_sha256",p->receipt_sha256,sizeof p->receipt_sha256)) return ASNGN_ERR_PARSE;
    c->depends_on = (uint32_t)deps;
    int k;
    for (k = ASNGN_PROOF_NOT_RUN; k <= ASNGN_PROOF_BLOCKED; k++)
      if (!strcmp(status,asngn_proof_status_name((asngn_proof_status)k))) break;
    if (k > ASNGN_PROOF_BLOCKED) return ASNGN_ERR_PARSE;
    p->status = (asngn_proof_status)k;
    if ((p->action_id[0] && !asngn_uuid_valid(p->action_id)) ||
        (p->snapshot[0] && !hash(p->snapshot)) ||
        (p->receipt_sha256[0] && !hash(p->receipt_sha256))) return ASNGN_ERR_PARSE;
    if (p->status == ASNGN_PROOF_PASSED &&
        (!p->action_id[0] || !p->snapshot[0] || !p->receipt_sha256[0])) return ASNGN_ERR_PARSE;
  }
  return asngn_work_definition_valid(&w->definition) ? ASNGN_OK : ASNGN_ERR_PARSE;
}
