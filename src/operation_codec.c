/* Keep record shape and provenance validation at the durable boundary. */
#include "operation_record.h"
#include "asmodel_json.h"
#include <limits.h>
#include <string.h>

char *asngn_operation_encode(const asngn_operation *op, const char *state,
    int64_t delta, int input, int output, bool known, asngn_err outcome) {
  asmodel_json_value *v = asmodel_json_object();
  if (!v) return NULL;
  int bad = 0;
#define SET(key, value) bad = bad || asmodel_json_object_set(v,key,value)
  SET("schema",asmodel_json_int(2));
  SET("id",asmodel_json_string(op->id));
  SET("request_id",asmodel_json_string(op->request_id));
  SET("state",asmodel_json_string(state));
  SET("model",asmodel_json_string(op->model));
  SET("kind",asmodel_json_string(op->kind));
  SET("day",asmodel_json_int(op->day));
  SET("budget_delta",asmodel_json_int(delta));
  SET("input_tokens",asmodel_json_int(input));
  SET("output_tokens",asmodel_json_int(output));
  SET("usage_known",asmodel_json_bool(known));
  SET("outcome",asmodel_json_string(asngn_err_name(outcome)));
#undef SET
  char *text = bad ? NULL : asmodel_json_write(v,0);
  asmodel_json_free(v); return text;
}

static const char *string(const asmodel_json_value *v, const char *key, size_t cap, bool empty) {
  const asmodel_json_value *field = asmodel_json_object_get(v,key);
  const char *text = asmodel_json_string_value(field);
  size_t bytes = asmodel_json_string_length(field);
  return text && bytes <= cap && (empty || bytes) && strlen(text) == bytes ? text : NULL;
}
static bool number(const asmodel_json_value *v, const char *key, int64_t *out) {
  const asmodel_json_value *field = asmodel_json_object_get(v,key);
  if (!asmodel_json_is_int(field)) return false;
  *out = asmodel_json_int_value(field); return true;
}
asngn_err asngn_operation_decode(const char *text, size_t bytes, asngn_operation_record *out) {
  asmodel_json_value *v = NULL;
  if (bytes > 4096) return ASNGN_ERR_LIMIT;
  if (asmodel_json_parse(text,bytes,&v)) return ASNGN_ERR_PARSE;
  memset(out,0,sizeof *out);
  const char *id = string(v,"id",36,false), *request = string(v,"request_id",128,true);
  const char *model = string(v,"model",ASMODEL_ID_MAX,false), *kind = string(v,"kind",32,false);
  const char *state = string(v,"state",16,false), *outcome = string(v,"outcome",32,false);
  const asmodel_json_value *known = asmodel_json_object_get(v,"usage_known");
  bool valid_outcome = false;
  for (int i = ASNGN_OK; outcome && i <= ASNGN_ERR_LIMIT; i++)
    if (!strcmp(outcome,asngn_err_name((asngn_err)i))) {
      valid_outcome = true; out->outcome = (asngn_err)i;
    }
  int64_t schema;
  bool valid = asmodel_json_object_count(v) == 12 && id && asngn_uuid_valid(id) &&
      request && model && kind && state && (!strcmp(state,"reserved") || !strcmp(state,"settled")) &&
      valid_outcome && number(v,"schema",&schema) && schema == 2 &&
      number(v,"day",&out->day) && out->day >= INT64_MIN/86400 && out->day <= INT64_MAX/86400 &&
      number(v,"budget_delta",&out->delta) &&
      number(v,"input_tokens",&out->input) && out->input >= 0 && out->input <= INT_MAX &&
      number(v,"output_tokens",&out->output) && out->output >= 0 && out->output <= INT_MAX &&
      asmodel_json_typeof(known) == ASMODEL_JSON_BOOL;
  if (valid) {
    strcpy(out->id,id); out->reserved = !strcmp(state,"reserved");
    out->known = asmodel_json_bool_value(known);
    if (out->reserved && strcmp(outcome,"ASNGN_OK")) valid = false;
    /* One-byte length framing is unambiguous for these bounded UTF-8 fields. */
    asngn_sha256_ctx hash; asngn_sha256_init(&hash);
    const char *fields[] = {model,kind,request};
    for (size_t i = 0; i < 3; i++) {
      uint8_t n = (uint8_t)strlen(fields[i]);
      asngn_sha256_update(&hash,&n,1); asngn_sha256_update(&hash,fields[i],n);
    }
    asngn_sha256_final(&hash,out->identity);
  }
  asmodel_json_free(v); return valid ? ASNGN_OK : ASNGN_ERR_PARSE;
}
