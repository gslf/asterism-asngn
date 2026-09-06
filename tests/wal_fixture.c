#include "wal_fixture.h"
#include "asngn_internal.h"
#include "xcdn.h"

static asngn_err collect(void *userdata, const char *record, size_t bytes) {
  xcdn_document_t *out = userdata, *frame = xcdn_parse_str(record, bytes, NULL);
  if (!frame)
    return ASNGN_ERR_PARSE;
  asngn_err e = ASNGN_OK;
  for (size_t i = 0; e == ASNGN_OK && i < frame->values_len; i++) {
    if (!asngn_xdoc_push(out, frame->values[i]))
      e = ASNGN_ERR_NOMEM;
    else
      frame->values[i] = NULL;
  }
  xcdn_document_free(frame);
  return e;
}
asngn_err asngn_test_wal_load(asngn_ctx *ctx, const char *path, xcdn_document_t **out) {
  *out = NULL;
  if (!os_file_exists(path))
    return ASNGN_OK;
  xcdn_document_t *doc = xcdn_document_new();
  if (!doc)
    return ASNGN_ERR_NOMEM;
  asngn_err e = asngn_wal_visit(ctx, path, 16u * 1024u * 1024u, collect, doc);
  if (e == ASNGN_OK)
    *out = doc;
  else
    xcdn_document_free(doc);
  return e;
}
