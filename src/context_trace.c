/* Describe the actual bounded selection, without claiming calibrated utility. */
#include "context.h"
#include <stdlib.h>
#include <string.h>

static asmodel_json_value *fingerprint(const char *text) {
  size_t len = text ? strlen(text) : 0;
  uint8_t hash[32];
  char hex[65];
  asngn_sha256(text ? text : "", len, hash);
  asngn_sha256_hex(hash, sizeof hash, hex);
  asmodel_json_value *v = asmodel_json_object();
  if (!v) return NULL;
  if (asmodel_json_object_set(v, "bytes", asmodel_json_int((long long)len)) ||
      asmodel_json_object_set(v, "sha256", asmodel_json_string(hex))) {
    asmodel_json_free(v);
    return NULL;
  }
  return v;
}
void asngn_context_trace_free(asngn_context_trace *trace) {
  asmodel_json_free(trace->root);
  memset(trace, 0, sizeof *trace);
}
asngn_err asngn_context_trace_init(asngn_context_trace *trace, asngn_ctx *c, asngn_session *s,
                                   asngn_turn_state *t, int slot) {
  memset(trace, 0, sizeof *trace);
  if (!c || slot < 0 || (size_t)slot >= c->models_n) return ASNGN_ERR_INVALID;
  trace->root = asmodel_json_object();
  if (!trace->root) return ASNGN_ERR_NOMEM;
  trace->items = asmodel_json_array();
  int bad = asmodel_json_object_set(trace->root, "items", trace->items);
#define SET(key, value) bad = bad || asmodel_json_object_set(trace->root, key, value)
  SET("schema", asmodel_json_int(1));
  SET("policy", asmodel_json_string("zones-v1"));
  SET("scope", asmodel_json_string("assembled_zones"));
  SET("model", asmodel_json_string(c->models[slot].cfg.id));
  SET("phase", asmodel_json_int(t ? t->phase : ASNGN_PHASE_ACTION));
  SET("observed_snapshot", asmodel_json_string(s ? s->workspace.fingerprint : ""));
  SET("memory_owner", asmodel_json_string(c->asper_ok ? "asper" : "local"));
  SET("count_basis", asmodel_json_string("text_counter_quality_unknown; request_admission_separate"));
  SET("working_budget", asmodel_json_int(c->cfg.working_tokens));
  SET("history_budget", asmodel_json_int(c->cfg.memory_history_tokens));
#undef SET
  if (bad) {
    asngn_context_trace_free(trace);
    return ASNGN_ERR_NOMEM;
  }
  return ASNGN_OK;
}
asngn_err asngn_context_trace_item(asngn_context_trace *trace, const char *zone, size_t index,
                                   const char *text, const char *decision, const char *reason) {
  if (trace->seen++ >= ASNGN_CONTEXT_TRACE_ITEMS) return ASNGN_OK;
  asmodel_json_value *v = fingerprint(text);
  if (!v) return ASNGN_ERR_NOMEM;
  int bad = asmodel_json_object_set(v, "zone", asmodel_json_string(zone)) ||
            asmodel_json_object_set(v, "index", asmodel_json_int((long long)index)) ||
            asmodel_json_object_set(v, "decision", asmodel_json_string(decision)) ||
            asmodel_json_object_set(v, "reason", asmodel_json_string(reason));
  if (bad) {
    asmodel_json_free(v);
    return ASNGN_ERR_NOMEM;
  }
  return asmodel_json_array_push(trace->items, v) ? ASNGN_ERR_NOMEM : ASNGN_OK;
}
asngn_err asngn_context_trace_finish(asngn_context_trace *trace, asngn_prompt *prompt) {
  asmodel_json_value *zones = asmodel_json_object();
  if (!zones) return ASNGN_ERR_NOMEM;
  int bad = 0;
#define ZONE(name)                                                                                 \
  bad = bad ||                                                                                     \
        asmodel_json_object_set(zones, #name, asmodel_json_int((long long)prompt->tok_##name))
  ZONE(system);
  ZONE(memory);
  ZONE(catalog);
  ZONE(summary);
  ZONE(verbatim);
  ZONE(working);
#undef ZONE
  if (bad) {
    asmodel_json_free(zones);
    return ASNGN_ERR_NOMEM;
  }
  bad = asmodel_json_object_set(trace->root, "zone_tokens", zones) ||
        asmodel_json_object_set(trace->root, "system", fingerprint(prompt->system_text)) ||
        asmodel_json_object_set(trace->root, "user", fingerprint(prompt->user_text)) ||
        asmodel_json_object_set(trace->root, "items_total",
                                asmodel_json_int((long long)trace->seen)) ||
        asmodel_json_object_set(
            trace->root, "items_omitted",
            asmodel_json_int((long long)(trace->seen > ASNGN_CONTEXT_TRACE_ITEMS
                                             ? trace->seen - ASNGN_CONTEXT_TRACE_ITEMS
                                             : 0)));
  if (bad) return ASNGN_ERR_NOMEM;
  prompt->selection_json = asmodel_json_write(trace->root, 0);
  return prompt->selection_json ? ASNGN_OK : ASNGN_ERR_NOMEM;
}
