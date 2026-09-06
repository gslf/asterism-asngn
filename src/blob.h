/* Explicit ranges in content-addressed, redacted tool evidence. */
#ifndef ASNGN_BLOB_H
#define ASNGN_BLOB_H
#include "asmodel_json.h"
#include "asngn_internal.h"
#define ASNGN_BLOB_READ_MAX 32768
asngn_err asngn_blob_parse(const asmodel_json_value *input, asngn_step *out);
asmodel_json_value *asngn_blob_schema(size_t count);
asngn_err asngn_blob_read(asngn_ctx *c, const asngn_blob *blob, size_t offset, size_t max_bytes,
                          char **out);
#endif
