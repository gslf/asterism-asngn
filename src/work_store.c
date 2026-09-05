/* Acceptance WAL is authoritative even when a conversational turn rolls back. */
#include "work_state.h"
#include <stdlib.h>
#include <string.h>

void asngn_work_free(asngn_work_store *w) {
  if (w) { asngn_stream_close(&w->stream); free(w); }
}
asngn_err asngn_work_load(asngn_session *s) {
  char *path = os_path_join(s->dir,"work.xcdn");
  xcdn_document_t *doc = NULL;
  asngn_work_store *w = calloc(1,sizeof *w);
  asngn_err e = ASNGN_ERR_NOMEM;
  if (!path || !w) goto done;
  e = asngn_wal_load(s->ctx,path,&doc);
  if (e != ASNGN_OK) goto done;
  for (size_t i = 0; doc && i < doc->values_len; i++) {
    asngn_work_state next;
    e = asngn_work_decode(doc->values[i]->value,&next);
    if (e != ASNGN_OK) goto done;
    if (next.sequence != w->state.sequence+1 ||
        next.revision < w->state.revision || next.revision > w->state.revision+1) {
      e = ASNGN_ERR_PARSE; goto done;
    }
    if (next.revision == w->state.revision) {
      const asngn_work_definition *a = &next.definition, *b = &w->state.definition;
      bool same = a->count == b->count && !strcmp(a->goal,b->goal) && !strcmp(a->constraints,b->constraints);
      for (size_t j = 0; same && j < a->count; j++)
        same = asngn_work_criterion_equal(&a->criteria[j],&b->criteria[j]);
      if (!same) { e = ASNGN_ERR_PARSE; goto done; }
    }
    w->state = next;
  }
  e = asngn_stream_open(s->ctx,&w->stream,path,true);
  if (e == ASNGN_OK) { s->work = w; w = NULL; }
done:
  free(path); xcdn_document_free(doc); asngn_work_free(w); return e;
}
asngn_err asngn_work_save(asngn_session *s, asngn_work_state *next) {
  if (s->recovery_required) return ASNGN_ERR_IO;
  if (!s->work || next->revision > INT64_MAX || s->work->state.sequence >= INT64_MAX)
    return ASNGN_ERR_LIMIT;
  next->sequence = s->work->state.sequence+1;
  xcdn_value_t *v = asngn_work_encode(next);
  xcdn_node_t *node = v ? xcdn_node_new(v) : NULL;
  if (!node) { xcdn_value_free(v); return ASNGN_ERR_NOMEM; }
  asngn_buf b; asngn_buf_init(&b);
  asngn_err e = asngn_xnode_write(node,false,&b);
  if (e == ASNGN_OK) {
    e = asngn_wal_append(s->ctx,&s->work->stream,b.data,b.len);
    if (e == ASNGN_OK) s->work->state = *next;
    else s->recovery_required = true;
  }
  xcdn_node_free(node); asngn_buf_free(&b); return e;
}
asngn_err asngn_work_revoke(asngn_session *s) {
  if (!s->work || !s->work->state.revision) return ASNGN_OK;
  asngn_work_state next = s->work->state;
  bool changed = false;
  for (size_t i = 0; i < next.definition.count; i++)
    if (next.proofs[i].status != ASNGN_PROOF_NOT_RUN && next.proofs[i].status != ASNGN_PROOF_STALE) {
      next.proofs[i].status = ASNGN_PROOF_STALE; changed = true;
    }
  next.succeeded = 0;
  return changed ? asngn_work_save(s,&next) : ASNGN_OK;
}
