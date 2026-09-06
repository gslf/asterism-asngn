/* Validate complete frames before repairing a torn tail or exposing approval state. */
#include "approval.h"
#include <stdlib.h>

static asngn_err replay(void *userdata, const char *record, size_t bytes) {
  asngn_approval_store *store = userdata;
  if (!asngn_utf8_valid(record, bytes))
    return ASNGN_ERR_PARSE;
  xcdn_document_t *doc = xcdn_parse_str(record, bytes, NULL);
  asngn_approval *next = NULL;
  asngn_err e = doc && doc->values_len == 1 ? asngn_approval_decode(doc->values[0]->value, &next)
                                            : ASNGN_ERR_PARSE;
  if (e == ASNGN_OK) {
    /* The pinned parser replaces duplicate keys and stores strings as C strings.
     * Require the exact writer form before accepting that potentially lossy DOM. */
    e = asngn_xcanonical_match(asngn_approval_encode(next), record, bytes);
  }
  xcdn_document_free(doc);
  if (e == ASNGN_OK && !asngn_approval_follows(store->current, next))
    e = ASNGN_ERR_PARSE;
  if (e == ASNGN_OK) {
    asngn_approval_free(store->current);
    store->current = next;
  } else
    asngn_approval_free(next);
  return e;
}

asngn_err asngn_approval_load(asngn_session *s) {
  char *path = os_path_join(s->dir, "approvals.xcdn");
  asngn_approval_store *store = calloc(1, sizeof *store);
  asngn_err e = ASNGN_ERR_NOMEM;
  if (!path || !store)
    goto done;
  e = asngn_wal_visit(s->ctx, path, ASNGN_APPROVAL_FRAME_MAX, replay, store);
  if (e != ASNGN_OK)
    goto done;
  e = asngn_stream_open(s->ctx, &store->stream, path, true);
  if (e == ASNGN_OK) {
    s->approvals = store;
    store = NULL;
    asngn_approval *a = s->approvals->current;
    if (a && (a->status == ASNGN_APPROVAL_PENDING || a->status == ASNGN_APPROVAL_APPROVED))
      e = asngn_approval_transition(s, ASNGN_APPROVAL_INTERRUPTED, a->session_wide);
  }
done:
  free(path);
  asngn_approval_store_free(store);
  return e;
}
