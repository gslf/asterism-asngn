/* Keep only bounded acceptance state; complete invalid frames are never repaired away. */
#include "work_state.h"
#include <stdlib.h>
#include <string.h>

static bool follows(const asngn_work_state *before, const asngn_work_state *next) {
  if (next->sequence != before->sequence + 1 || next->revision < before->revision ||
      next->revision > before->revision + 1)
    return false;
  if (next->revision != before->revision)
    return true;
  const asngn_work_definition *a = &next->definition, *b = &before->definition;
  if (a->count != b->count || strcmp(a->goal, b->goal) || strcmp(a->constraints, b->constraints))
    return false;
  for (size_t i = 0; i < a->count; i++)
    if (!asngn_work_criterion_equal(&a->criteria[i], &b->criteria[i]))
      return false;
  return true;
}
static asngn_err replay(void *userdata, const char *record, size_t bytes) {
  asngn_work_store *store = userdata;
  if (!asngn_utf8_valid(record, bytes))
    return ASNGN_ERR_PARSE;
  xcdn_document_t *doc = xcdn_parse_str(record, bytes, NULL);
  asngn_work_state next;
  asngn_err e = doc && doc->values_len == 1 ? asngn_work_decode(doc->values[0]->value, &next)
                                            : ASNGN_ERR_PARSE;
  xcdn_document_free(doc);
  if (e == ASNGN_OK)
    e = asngn_xcanonical_match(asngn_work_encode(&next), record, bytes);
  if (e == ASNGN_OK && !follows(&store->state, &next))
    e = ASNGN_ERR_PARSE;
  if (e == ASNGN_OK)
    store->state = next;
  return e;
}
asngn_err asngn_work_load(asngn_session *s) {
  char *path = os_path_join(s->dir, "work.xcdn");
  asngn_work_store *store = calloc(1, sizeof *store);
  asngn_err e = ASNGN_ERR_NOMEM;
  if (!path || !store)
    goto done;
  e = asngn_wal_visit(s->ctx, path, ASNGN_WORK_FRAME_MAX, replay, store);
  if (e == ASNGN_OK)
    e = asngn_stream_open(s->ctx, &store->stream, path, true);
  if (e == ASNGN_OK) {
    s->work = store;
    store = NULL;
  }
done:
  free(path);
  asngn_work_free(store);
  return e;
}
