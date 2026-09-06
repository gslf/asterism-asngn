/* A read names its offset; there is no hidden cursor to advance or lose. */
#include "blob.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

asngn_err asngn_blob_parse(const asmodel_json_value *input, asngn_step *out) {
  const asmodel_json_value *b = asmodel_json_object_get(input, "blob"),
                           *o = asmodel_json_object_get(input, "offset");
  long long number = asmodel_json_int_value(b), offset = asmodel_json_int_value(o);
  if (asmodel_json_typeof(input) != ASMODEL_JSON_OBJECT || asmodel_json_object_count(input) != 2 ||
      !asmodel_json_is_int(b) || !asmodel_json_is_int(o) || number < 1 || number > INT_MAX ||
      offset < 0 || offset > UINT32_MAX)
    return ASNGN_ERR_PROTOCOL;
  out->blob_n = (int)number;
  out->blob_offset = (size_t)offset;
  return ASNGN_OK;
}
asmodel_json_value *asngn_blob_schema(size_t count) {
  static const char schema[] =
      "{\"type\":\"object\",\"properties\":{\"blob\":{\"type\":\"integer\",\"minimum\":1},"
      "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4294967295}},"
      "\"required\":[\"blob\",\"offset\"],\"additionalProperties\":false}";
  asmodel_json_value *v = NULL;
  if (asmodel_json_parse(schema, sizeof schema - 1, &v)) return NULL;
  asmodel_json_value *values = asmodel_json_array();
  int bad = values == NULL;
  for (size_t i = 1; !bad && i <= count; i++)
    bad = asmodel_json_array_push(values, asmodel_json_int((long long)i));
  if (bad) {
    asmodel_json_free(values);
    asmodel_json_free(v);
    return NULL;
  }
  asmodel_json_value *props = asmodel_json_object_get(v, "properties");
  bad = asmodel_json_object_set(asmodel_json_object_get(props, "blob"), "enum", values);
  if (bad) {
    asmodel_json_free(v);
    return NULL;
  }
  return v;
}
asngn_err asngn_blob_read(asngn_ctx *c, const asngn_blob *blob, size_t offset, size_t max_bytes,
                          char **out) {
  if (!out) return ASNGN_ERR_INVALID;
  *out = NULL;
  if (!c || !blob || !blob->object_ref[0] || offset > blob->size || max_bytes < 4)
    return ASNGN_ERR_INVALID;
  if (offset == blob->size) return ASNGN_OK;
  if (max_bytes > ASNGN_BLOB_READ_MAX) max_bytes = ASNGN_BLOB_READ_MAX;
  void *raw = NULL;
  size_t len = 0;
  asngn_err e = asngn_siblings_object_read(c, blob->object_ref, offset, max_bytes + 3, &raw, &len);
  if (e != ASNGN_OK) return e;
  const char *text = raw;
  size_t take = len < max_bytes ? len : max_bytes;
  while (take && take < len && ((unsigned char)text[take] & 0xc0) == 0x80)
    take--;
  if (!take || !asngn_utf8_valid(text, take)) {
    free(raw);
    return ASNGN_ERR_INVALID;
  }
  asngn_buf b = {0};
  e = asngn_buf_printf(&b, "[%s; %s; bytes %zu-%zu of %zu]\n", blob->label, blob->object_ref,
                       offset, offset + take, blob->size);
  if (e == ASNGN_OK) e = asngn_buf_append(&b, text, take);
  free(raw);
  if (e == ASNGN_OK) *out = asngn_buf_detach(&b);
  asngn_buf_free(&b);
  return e;
}
