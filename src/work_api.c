/* Public contract updates serialize with admission and use revision checks. */
#include "work_state.h"
#include <stdlib.h>
#include <string.h>

asngn_err asngn_session_work_define(asngn_session *s, uint64_t revision,
                                    const asngn_work_definition *d) {
  if (!s || !asngn_work_definition_valid(d)) return ASNGN_ERR_INVALID;
  os_rwlock_wrlock(&s->lock);
  asngn_err e = ASNGN_OK;
  if (s->busy || !s->work || s->work->state.revision != revision) e = ASNGN_ERR_BUSY;
  else if (revision >= INT64_MAX) e = ASNGN_ERR_LIMIT;
  else {
    asngn_work_state next = {0};
    const asngn_work_state *old = &s->work->state;
    uint32_t retained = 0;
    next.revision = revision+1; next.definition = *d;
    for (size_t i = 0; i < d->count; i++) {
      const asngn_work_criterion *c = &d->criteria[i];
      if (i < old->definition.count && !strcmp(d->constraints,old->definition.constraints) &&
          asngn_work_criterion_equal(c,&old->definition.criteria[i]) &&
          (c->depends_on & retained) == c->depends_on) {
        next.proofs[i] = old->proofs[i]; retained |= 1u << i;
      }
    }
    e = asngn_work_save(s,&next);
  }
  os_rwlock_wrunlock(&s->lock); return e;
}
asngn_err asngn_session_work_invalidate(asngn_session *s, uint64_t revision) {
  if (!s) return ASNGN_ERR_INVALID;
  os_rwlock_wrlock(&s->lock);
  asngn_err e = ASNGN_OK;
  if (s->busy || !s->work || s->work->state.revision != revision) e = ASNGN_ERR_BUSY;
  else if (!revision) e = ASNGN_ERR_NOT_FOUND;
  else if (revision >= INT64_MAX) e = ASNGN_ERR_LIMIT;
  else {
    asngn_work_state next = s->work->state;
    next.revision++; next.succeeded = 0;
    memset(next.proofs,0,sizeof next.proofs);
    e = asngn_work_save(s,&next);
  }
  os_rwlock_wrunlock(&s->lock); return e;
}
asngn_err asngn_session_work_get(asngn_session *s, asngn_work_state **out) {
  if (!s || !out) return ASNGN_ERR_INVALID;
  *out = NULL;
  os_rwlock_rdlock(&s->lock);
  asngn_err e = ASNGN_OK;
  if (s->recovery_required) e = ASNGN_ERR_IO;
  else if (!s->work || !s->work->state.revision) e = ASNGN_ERR_NOT_FOUND;
  else {
    asngn_workspace_info ws = s->workspace;
    *out = malloc(sizeof **out);
    if (!*out) e = ASNGN_ERR_NOMEM;
    else {
      **out = s->work->state;
      if (asngn_workspace_snapshot(&ws,NULL) != ASNGN_OK) ws.fingerprint[0] = 0;
      asngn_work_refresh(*out,ws.fingerprint);
    }
  }
  os_rwlock_rdunlock(&s->lock); return e;
}
asngn_err asngn_work_render(asngn_session *s, asngn_buf *out) {
  if (!s || !s->work || !s->work->state.revision) return ASNGN_OK;
  asngn_work_state *w = NULL;
  asngn_err e = asngn_session_work_get(s,&w);
  if (e == ASNGN_ERR_NOT_FOUND) return ASNGN_OK;
  if (e != ASNGN_OK) return e;
  e = asngn_buf_printf(out,"\n## Acceptance contract (revision %llu)\nGoal: %s\nConstraints: %s\n"
      "Declared checks: %s. Their coverage is limited to these requirements.\n",
      (unsigned long long)w->revision,w->definition.goal,w->definition.constraints,
      w->succeeded ? "passed" : "incomplete");
  for (size_t i = 0; e == ASNGN_OK && i < w->definition.count; i++) {
    const asngn_work_criterion *c = &w->definition.criteria[i];
    e = asngn_buf_printf(out,"[%s] %s: %s; project.%s path=%s adapter=%s; requires=",
        asngn_proof_status_name(w->proofs[i].status),c->id,c->requirement,c->command,c->path,c->adapter);
    for (size_t j = 0; e == ASNGN_OK && j < i; j++)
      if (c->depends_on & (1u << j)) e = asngn_buf_printf(out," %s",w->definition.criteria[j].id);
    if (e == ASNGN_OK) e = asngn_buf_appendc(out,'\n');
  }
  free(w); return e;
}
