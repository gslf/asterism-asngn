/* Only the dispatch path calls this module, after journaling the raw receipt. */
#include "work_state.h"
#include <stdlib.h>
#include <string.h>

static const char *field(const xcdn_value_t *v, const char *key) {
  return asngn_xstr(asngn_xfield(v,key));
}
static bool target(const asngn_session *s, const asngn_work_criterion *c,
                   const xcdn_value_t *args) {
  const char *path = field(args,"path"), *adapter = field(args,"adapter");
  char *expected = !strcmp(c->path,".") ? asngn_strdup(s->workspace.canonical_root) :
      os_path_join(s->workspace.canonical_root,c->path);
  if (!path || !strcmp(path,".")) path = s->workspace.canonical_root;
  bool ok = expected && (!strcmp(path,c->path) || !strcmp(path,expected));
  if (adapter && strcmp(adapter,"auto") && strcmp(c->adapter,"auto"))
    ok = ok && !strcmp(adapter,c->adapter);
  free(expected); return ok;
}
static bool receipt_target(const asngn_session *s, const asngn_work_criterion *c,
                           const xcdn_value_t *receipt) {
  const char *cwd = field(receipt,"cwd"), *adapter = field(receipt,"adapter");
  char *expected = !strcmp(c->path,".") ? asngn_strdup(s->workspace.canonical_root) :
      os_path_join(s->workspace.canonical_root,c->path);
  bool ok = expected && cwd && !strcmp(cwd,expected) && adapter &&
      (!strcmp(c->adapter,"auto") || !strcmp(c->adapter,adapter));
  free(expected); return ok;
}
static asngn_proof_status status(const char *cmd, bool ok, const char *text,
                                  const xcdn_value_t *v) {
  const char *action = field(v,"action"), *value = field(v,"verification_status");
  int64_t schema, exit;
  if (!ok) return ASNGN_PROOF_INFRASTRUCTURE_ERROR;
  if (!value || !action || strcmp(action,cmd) ||
      !asngn_xint(asngn_xfield(v,"verification_schema"),&schema) || schema != 1 ||
      !asngn_xint(asngn_xfield(v,"exit_code"),&exit)) return ASNGN_PROOF_INCONCLUSIVE;
  if (exit || !strcmp(value,"failed")) return ASNGN_PROOF_FAILED;
  if (!strcmp(value,"passed") && asngn_verification_result_ok(text)) {
    int64_t collected, skipped;
    if (!strcmp(cmd,"test") &&
        (!asngn_xint(asngn_xfield(v,"tests_collected"),&collected) ||
         !asngn_xint(asngn_xfield(v,"tests_skipped"),&skipped) ||
         skipped < 0 || collected <= skipped)) return ASNGN_PROOF_INCONCLUSIVE;
    return ASNGN_PROOF_PASSED;
  }
  if (!strcmp(value,"not_run")) return ASNGN_PROOF_NOT_RUN;
  if (!strcmp(value,"infrastructure_error")) return ASNGN_PROOF_INFRASTRUCTURE_ERROR;
  return ASNGN_PROOF_INCONCLUSIVE;
}
asngn_err asngn_work_observe(asngn_turn_state *t, const char *ref,
    const char *cmd, const char *args, bool tool_ok, const char *result,
    const char *before, const char *after) {
  asngn_session *s = t->s;
  if (!asngn_verification_command(ref,cmd,args)) return ASNGN_OK;
  os_rwlock_wrlock(&s->lock);
  asngn_err e = ASNGN_OK;
  xcdn_document_t *ad = NULL, *rd = NULL;
  if (!s->work || !s->work->state.revision) goto done;
  if (!asngn_uuid_valid(t->action_id)) { e = ASNGN_ERR_INVALID; goto done; }
  ad = xcdn_parse(args ? args : "{}",NULL);
  rd = result ? xcdn_parse(result,NULL) : NULL;
  const xcdn_value_t *av = ad && ad->values_len == 1 ? ad->values[0]->value : NULL;
  const xcdn_value_t *rv = rd && rd->values_len == 1 ? rd->values[0]->value : NULL;
  asngn_work_state next = s->work->state;
  asngn_work_refresh(&next,after);
  bool changed = false;
  for (size_t i = 0; av && i < next.definition.count; i++) {
    const asngn_work_criterion *c = &next.definition.criteria[i];
    if (strcmp(c->command,cmd) || !target(s,c,av)) continue;
    asngn_work_proof *p = &next.proofs[i];
    memset(p,0,sizeof *p);
    p->status = !tool_ok ? ASNGN_PROOF_INFRASTRUCTURE_ERROR :
        receipt_target(s,c,rv) ? status(cmd,tool_ok,result,rv) : ASNGN_PROOF_INCONCLUSIVE;
    if (!before || !*before || !after || strcmp(before,after)) p->status = ASNGN_PROOF_STALE;
    memcpy(p->action_id,t->action_id,sizeof p->action_id);
    if (after) snprintf(p->snapshot,sizeof p->snapshot,"%s",after);
    if (result) {
      uint8_t hash[32]; asngn_sha256(result,strlen(result),hash);
      asngn_sha256_hex(hash,sizeof hash,p->receipt_sha256);
    }
    changed = true;
  }
  /* A failed prerequisite also invalidates a previously passed dependant.
   * A later prerequisite success does not resurrect the blocked proof. */
  asngn_work_refresh(&next,after);
  for (size_t i = 0; i < next.definition.count; i++)
    changed = changed || next.proofs[i].status != s->work->state.proofs[i].status;
  if (changed) e = asngn_work_save(s,&next);
done:
  xcdn_document_free(ad); xcdn_document_free(rd);
  os_rwlock_wrunlock(&s->lock); return e;
}
