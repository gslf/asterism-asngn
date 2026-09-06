/* Approval durability is independent of the conversational commit. */
#include "approval.h"
#include <stdlib.h>

void asngn_approval_free(asngn_approval *a) {
  if (a) {
    free(a->arguments);
    free(a);
  }
}
void asngn_approval_store_free(asngn_approval_store *store) {
  if (store) {
    asngn_stream_close(&store->stream);
    asngn_approval_free(store->current);
    free(store);
  }
}

asngn_err asngn_approval_save(asngn_session *s, asngn_approval *next) {
  if (s->recovery_required) return ASNGN_ERR_IO;
  asngn_approval *before = s->approvals->current;
  if (before && before->sequence >= INT64_MAX) return ASNGN_ERR_LIMIT;
  next->sequence = before ? before->sequence + 1 : 1;
  if (!asngn_approval_follows(before, next)) return ASNGN_ERR_INVALID;
  xcdn_value_t *v = asngn_approval_encode(next);
  xcdn_node_t *node = v ? xcdn_node_new(v) : NULL;
  if (!node) {
    xcdn_value_free(v);
    return ASNGN_ERR_NOMEM;
  }
  asngn_buf b;
  asngn_buf_init(&b);
  asngn_err e = asngn_xnode_write(node, false, &b);
  if (e == ASNGN_OK) e = asngn_wal_append(s->ctx, &s->approvals->stream, b.data, b.len);
  xcdn_node_free(node);
  asngn_buf_free(&b);
  if (e == ASNGN_OK) {
    s->approvals->current = next;
    asngn_approval_free(before);
  } else if (e == ASNGN_ERR_IO) s->recovery_required = true;
  return e;
}

asngn_err asngn_approval_transition(asngn_session *s, asngn_approval_status status, int wide) {
  asngn_approval *before = s->approvals ? s->approvals->current : NULL;
  if (!before) return ASNGN_ERR_NOT_FOUND;
  asngn_approval *next = malloc(sizeof *next);
  if (!next) return ASNGN_ERR_NOMEM;
  *next = *before;
  next->arguments = asngn_strdup(before->arguments);
  if (!next->arguments) {
    free(next);
    return ASNGN_ERR_NOMEM;
  }
  next->status = status;
  next->session_wide = wide;
  asngn_err e = asngn_approval_save(s, next);
  if (e != ASNGN_OK) asngn_approval_free(next);
  return e;
}

asngn_err asngn_approval_load(asngn_session *s) {
  char *path = os_path_join(s->dir, "approvals.xcdn");
  xcdn_document_t *doc = NULL;
  asngn_approval_store *store = calloc(1, sizeof *store);
  asngn_err e = ASNGN_ERR_NOMEM;
  if (!path || !store) goto done;
  e = asngn_wal_load(s->ctx, path, &doc);
  if (e != ASNGN_OK) goto done;
  for (size_t i = 0; doc && i < doc->values_len; i++) {
    asngn_approval *next = NULL;
    e = asngn_approval_decode(doc->values[i]->value, &next);
    if (e == ASNGN_OK && !asngn_approval_follows(store->current, next)) e = ASNGN_ERR_PARSE;
    if (e != ASNGN_OK) {
      asngn_approval_free(next);
      goto done;
    }
    asngn_approval_free(store->current);
    store->current = next;
  }
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
  xcdn_document_free(doc);
  asngn_approval_store_free(store);
  return e;
}

asngn_err asngn_approval_get(asngn_session *s, asngn_approval **out) {
  if (!s || !out) return ASNGN_ERR_INVALID;
  *out = NULL;
  os_rwlock_rdlock(&s->lock);
  asngn_approval *current = s->approvals ? s->approvals->current : NULL;
  asngn_err e = current ? ASNGN_OK : ASNGN_ERR_NOT_FOUND;
  if (current) {
    *out = malloc(sizeof **out);
    if (*out) {
      **out = *current;
      (*out)->arguments = asngn_strdup(current->arguments);
      if (!(*out)->arguments) {
        free(*out);
        *out = NULL;
      }
    }
    if (!*out) e = ASNGN_ERR_NOMEM;
  }
  os_rwlock_rdunlock(&s->lock);
  return e;
}
