/* Decimal strings keep signed 64-bit counters intact in every JSON client. */
#include "consumption.h"
#include <stdio.h>

static asmodel_json_value *decimal(int64_t n) {
  char text[32];
  snprintf(text, sizeof text, "%lld", (long long)n);
  return asmodel_json_string(text);
}
static asmodel_json_value *totals(const asngn_consumption_totals *v) {
  asmodel_json_value *out = asmodel_json_object();
  if (!out)
    return NULL;
  int bad = 0;
#define FIELD(name) bad = bad || asmodel_json_object_set(out, #name, decimal(v->name))
  FIELD(calls);
  FIELD(unsettled_calls);
  FIELD(unknown_calls);
  FIELD(failed_calls);
  FIELD(cancelled_calls);
  FIELD(known_input_tokens);
  FIELD(known_output_tokens);
  FIELD(unsettled_tokens);
  FIELD(unknown_tokens);
  FIELD(charged_tokens);
#undef FIELD
  if (bad) {
    asmodel_json_free(out);
    return NULL;
  }
  return out;
}
asngn_err mcp_consumption_read(asngn_ctx *ctx, asmodel_json_value **out) {
  *out = NULL;
  asngn_consumption v;
  asngn_err e = asngn_get_consumption(ctx, &v);
  if (e != ASNGN_OK)
    return e;
  asmodel_json_value *o = asmodel_json_object();
  if (!o)
    return ASNGN_ERR_NOMEM;
  int bad = asmodel_json_object_set(o, "schema", asmodel_json_int(1)) ||
            asmodel_json_object_set(o, "scope", asmodel_json_string("engine_store")) ||
            asmodel_json_object_set(o, "unit", asmodel_json_string("tokens")) ||
            asmodel_json_object_set(o, "time_basis", asmodel_json_string("reservation_utc_day")) ||
            asmodel_json_object_set(o, "utc_day", asmodel_json_int(v.utc_day)) ||
            asmodel_json_object_set(o, "lifetime", totals(&v.lifetime)) ||
            asmodel_json_object_set(o, "today", totals(&v.today));
  if (bad) {
    asmodel_json_free(o);
    return ASNGN_ERR_NOMEM;
  }
  *out = o;
  return ASNGN_OK;
}
