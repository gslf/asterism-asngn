/* Hash the complete admitted IR; bound detail independently from prompt size. */
#include "context.h"
#include <stdlib.h>
#include <string.h>

static void number(asngn_sha256_ctx *hash, uint64_t value) {
  unsigned char bytes[8];
  for (size_t i = 0; i < 8; i++) bytes[i] = (unsigned char)(value >> (8 * i));
  asngn_sha256_update(hash, bytes, sizeof bytes);
}
static void field(asngn_sha256_ctx *hash, const char *text) {
  size_t len = text ? strlen(text) : 0;
  number(hash, text != NULL);
  number(hash, len);
  if (len) asngn_sha256_update(hash, text, len);
}
static asmodel_json_value *block_item(const asmodel_message *message, size_t i, size_t j) {
  const asmodel_block *block = &message->blocks[j];
  asmodel_json_value *v = asngn_context_fingerprint(block->text);
  if (!v) return NULL;
  int bad = asmodel_json_object_set(v, "kind", asmodel_json_string("message_block")) ||
    asmodel_json_object_set(v, "message", asmodel_json_int((long long)i)) ||
    asmodel_json_object_set(v, "block", asmodel_json_int((long long)j)) ||
    asmodel_json_object_set(v, "role", asmodel_json_string(asmodel_role_name(message->role))) ||
    asmodel_json_object_set(v, "block_kind", asmodel_json_int(block->kind));
  if (!bad && block->id)
    bad = asmodel_json_object_set(v, "correlation", asngn_context_fingerprint(block->id));
  if (!bad && block->name)
    bad = asmodel_json_object_set(v, "tool_name", asngn_context_fingerprint(block->name));
  if (bad) { asmodel_json_free(v); return NULL; }
  return v;
}
static asmodel_json_value *tool_item(const asmodel_tool_schema *tool, size_t i) {
  asmodel_json_value *v = asmodel_json_object();
  if (!v) return NULL;
  int bad = asmodel_json_object_set(v, "kind", asmodel_json_string("tool_schema")) ||
    asmodel_json_object_set(v, "index", asmodel_json_int((long long)i)) ||
    asmodel_json_object_set(v, "name", asngn_context_fingerprint(tool->name)) ||
    asmodel_json_object_set(v, "description", asngn_context_fingerprint(tool->description)) ||
    asmodel_json_object_set(v, "parameters", asngn_context_fingerprint(tool->parameters));
  if (bad) { asmodel_json_free(v); return NULL; }
  return v;
}
static int input_items(asmodel_json_value *root, const asmodel_input *input,
                       const char *grammar, const asmodel_generate_params *params) {
  asngn_sha256_ctx hash;
  asngn_sha256_init(&hash);
  field(&hash, "asterism-input-contract-v1");
  field(&hash, grammar);
  field(&hash, params->output_schema);
  const asmodel_tools *tools = params->tools;
  number(&hash, tools != NULL);
  number(&hash, tools ? tools->choice : 0);
  number(&hash, tools ? tools->count : 0);
  asmodel_json_value *items = asmodel_json_array();
  if (!items || asmodel_json_object_set(root, "items", items)) return 1;
  size_t total = 0;
  /* Tool contracts get detail first. Omitted blocks still contribute to the
   * complete digest, and message/block indexes identify every retained item. */
  for (size_t i = 0; tools && i < tools->count; i++) {
    const asmodel_tool_schema *tool = &tools->schemas[i];
    field(&hash, tool->name);
    field(&hash, tool->description);
    field(&hash, tool->parameters);
    if (total++ < ASNGN_CONTEXT_TRACE_ITEMS && asmodel_json_array_push(items, tool_item(tool, i)))
      return 1;
  }
  number(&hash, input->count);
  for (size_t i = 0; i < input->count; i++) {
    const asmodel_message *m = &input->messages[i];
    number(&hash, m->role);
    number(&hash, m->count);
    for (size_t j = 0; j < m->count; j++) {
      const asmodel_block *b = &m->blocks[j];
      number(&hash, b->kind);
      field(&hash, b->id); field(&hash, b->name); field(&hash, b->text);
      if (total++ < ASNGN_CONTEXT_TRACE_ITEMS && asmodel_json_array_push(items, block_item(m, i, j)))
        return 1;
    }
  }
  uint8_t bytes[32]; char hex[65];
  asngn_sha256_final(&hash, bytes);
  asngn_sha256_hex(bytes, sizeof bytes, hex);
  size_t omitted = total > ASNGN_CONTEXT_TRACE_ITEMS ? total - ASNGN_CONTEXT_TRACE_ITEMS : 0;
  return asmodel_json_object_set(root, "input_contract_sha256", asmodel_json_string(hex)) ||
    asmodel_json_object_set(root, "messages", asmodel_json_int((long long)input->count)) ||
    asmodel_json_object_set(root, "tools", asmodel_json_int((long long)(tools ? tools->count : 0))) ||
    asmodel_json_object_set(root, "items_total", asmodel_json_int((long long)total)) ||
    asmodel_json_object_set(root, "items_omitted", asmodel_json_int((long long)omitted));
}
char *asngn_request_trace(asngn_ctx *c, const asngn_turn_state *turn, int slot,
                           asngn_task_kind task, const asmodel_input *input,
                           const char *grammar, const asmodel_generate_params *params,
                           const asngn_context_diagnostics *budget, asngn_err admission) {
  if (!c || slot < 0 || (size_t)slot >= c->models_n || !params || !budget) return NULL;
  asmodel_json_value *root = asmodel_json_object();
  if (!root) return NULL;
  int bad = 0;
  bool measured = admission == ASNGN_OK || admission == ASNGN_ERR_CONTEXT;
#define SET(key, value) bad = bad || asmodel_json_object_set(root, key, value)
  SET("schema", asmodel_json_int(1));
  SET("scope", asmodel_json_string("model_adapter_input"));
  SET("model", asmodel_json_string(c->models[slot].cfg.id));
  SET("task", asmodel_json_string(asngn_task_name(task)));
  SET("phase", turn ? asmodel_json_int(turn->phase) : asmodel_json_null());
  SET("admission", asmodel_json_string(asngn_err_name(admission)));
  SET("observed_snapshot", asmodel_json_string(turn && turn->s ? turn->s->workspace.fingerprint : ""));
  SET("count_basis", asmodel_json_string("conservative_admission; counter_quality_not_exposed"));
  SET("prompt_total", measured ? asmodel_json_int((long long)budget->prompt_total) : asmodel_json_null());
  SET("prompt_budget", measured ? asmodel_json_int((long long)budget->prompt_budget) : asmodel_json_null());
  SET("output_reserve", measured ? asmodel_json_int((long long)budget->output_reserve) : asmodel_json_null());
  SET("safety_margin", measured ? asmodel_json_int((long long)budget->safety_margin) : asmodel_json_null());
  SET("reasoning", asmodel_json_int(params->reasoning));
  SET("max_tokens", asmodel_json_int(params->max_tokens));
  SET("temperature", asmodel_json_double(params->temperature));
  SET("top_p", asmodel_json_double(params->top_p));
  SET("repeat_penalty", asmodel_json_double(params->repeat_penalty));
  SET("tool_choice", asmodel_json_int(params->tools ? params->tools->choice : 0));
  SET("constraint_required", asmodel_json_bool(params->require_constraint));
  SET("output_schema", params->output_schema ? asngn_context_fingerprint(params->output_schema) : asmodel_json_null());
  SET("grammar", grammar ? asngn_context_fingerprint(grammar) : asmodel_json_null());
#undef SET
  /* Only admission success/context overflow proves input validation ran. Never
   * traverse an invalid caller structure just to produce diagnostic metadata. */
  if (!bad && measured)
    bad = input_items(root, input, grammar, params);
  char *result = bad ? NULL : asmodel_json_write(root, 0);
  asmodel_json_free(root);
  if (result && strlen(result) > 60000) { free(result); return NULL; }
  return result;
}

void asngn_request_result(asngn_ctx *c, const asngn_turn_state *turn, const char *request,
                           int slot, asngn_task_kind task, const asmodel_generation_info *info,
                           asngn_err result, int64_t ms, bool attempted) {
  asmodel_json_value *v = asmodel_json_object();
  if (!v) return;
  int bad = 0;
#define SET(key, value) bad = bad || asmodel_json_object_set(v, key, value)
  SET("model", asmodel_json_string(c->models[slot].cfg.id));
  SET("task", asmodel_json_string(asngn_task_name(task)));
  SET("outcome", asmodel_json_string(asngn_err_name(result)));
  SET("tokens_in", asmodel_json_int(info->input_tokens));
  SET("tokens_out", asmodel_json_int(info->output_tokens));
  SET("ms", asmodel_json_int(ms));
  SET("usage_known", asmodel_json_bool(info->usage_known));
  SET("finish_reason", asmodel_json_int(info->finish_reason));
  SET("runtime_dispatch_attempted", asmodel_json_bool(attempted));
#undef SET
  char *text = bad ? NULL : asmodel_json_write(v, 0);
  asmodel_json_free(v);
  if (text) asngn_tele_emit(c, attempted ? "model_call" : "model_not_run", request,
                             turn ? turn->led.turn_id : NULL,
                             turn && turn->s ? turn->s->slug : NULL, turn ? turn->led.turn : 0, text);
  free(text);
}
