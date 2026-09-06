/* Acceptance WAL is authoritative even when a conversational turn rolls back. */
#include "work_state.h"
#include <stdlib.h>
#include <string.h>

void asngn_work_free(asngn_work_store *w) {
  if (w) { asngn_stream_close(&w->stream); free(w); }
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
  if (e == ASNGN_OK && b.len > ASNGN_WORK_FRAME_MAX) e = ASNGN_ERR_LIMIT;
  if (e == ASNGN_OK && !asngn_utf8_valid(b.data, b.len)) e = ASNGN_ERR_INVALID;
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
