/* Provider-safe names and strict engine control arguments; no authority from metadata. */
#include "asmodel_json.h"
#include "native.h"
#include "blob.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void control(asngn_native_contract *out, asngn_step_kind kind, const char *name,
                    const char *description, const char *schema) {
  size_t n = out->count++;
  out->schemas[n] = (asmodel_tool_schema){name, description, schema};
  out->kinds[n] = kind;
}

asngn_err asngn_native_contract_build(asngn_ctx *c, asngn_turn_state *t, bool call_now,
                                      asngn_native_contract *out) {
  memset(out, 0, sizeof *out);
  size_t n = astools_selection_count(t->tool_selection);
  if (n > 59)
    return asngn_seterr(
        c, ASNGN_ERR_CONFIG,
        "native actions reserve five control functions; configure tool_limit <= 59");
  for (size_t i = 0; call_now && i < n; i++) {
    const astools_selected_command *v = astools_selection_get(t->tool_selection, i);
    uint8_t hash[32];
    char hex[65];
    asngn_sha256_ctx sha;
    asngn_sha256_init(&sha);
    asngn_sha256_update(&sha, v->ref, strlen(v->ref) + 1);
    asngn_sha256_update(&sha, v->command, strlen(v->command) + 1);
    asngn_sha256_update(&sha, v->content_sha256, strlen(v->content_sha256));
    asngn_sha256_final(&sha, hash);
    asngn_sha256_hex(hash, 32, hex);
    snprintf(out->names[i], sizeof out->names[i], "t_%.60s", hex);
    snprintf(out->descriptions[i], sizeof out->descriptions[i], "%s: %.1024s", v->tool, v->summary);
    size_t bytes = strlen(out->descriptions[i]);
    while (bytes && !asngn_utf8_valid(out->descriptions[i], bytes))
      out->descriptions[i][--bytes] = 0;
    for (size_t j = 0; j < i; j++)
      if (!strcmp(out->names[i], out->names[j])) return ASNGN_ERR_PROTOCOL;
    out->schemas[i] = (asmodel_tool_schema){out->names[i], out->descriptions[i], v->arguments};
    out->commands[i] = v;
    out->kinds[i] = ASNGN_STEP_CALL;
    out->count++;
  }
  if (c->astools_ok && !t->opts.no_tools)
    control(out, ASNGN_STEP_DISCOVER, "asterism_discover",
            "Replace the tool shortlist by name or purpose.",
            "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\",\"minLength\":1,"
            "\"maxLength\":2048}},\"required\":[\"input\"],\"additionalProperties\":false}");
  if (c->asper_ok)
    control(out, ASNGN_STEP_RECALL, "asterism_recall", "Retrieve memory evidence for a question.",
            "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\",\"minLength\":1,"
            "\"maxLength\":2048}},\"required\":[\"input\"],\"additionalProperties\":false}");
  if (t->s->blobs_n)
    control(out, ASNGN_STEP_OPEN, "asterism_open",
            "Read a redacted evidence blob at an explicit byte offset.",
            "{\"type\":\"object\",\"properties\":{\"blob\":{\"type\":\"integer\",\"minimum\":1},"
            "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4294967295}},"
            "\"required\":[\"blob\",\"offset\"],\"additionalProperties\":false}");
  control(out, ASNGN_STEP_CLARIFY, "asterism_clarify",
          "Ask the user for missing information needed to proceed.",
          "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\",\"minLength\":1,"
          "\"maxLength\":2048}},\"required\":[\"input\"],\"additionalProperties\":false}");
  if (!asngn_generation_needs_artifact(c, t))
    control(out, ASNGN_STEP_ANSWER, "asterism_finish",
            "End actions and prepare a user response. This does not certify task success.",
            "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
  return ASNGN_OK;
}

static asngn_err control_args(const char *args, asngn_turn_state *t, asngn_step *step) {
  asmodel_json_value *obj = NULL;
  if (asmodel_json_parse(args, strlen(args), &obj)) return ASNGN_ERR_PROTOCOL;
  asngn_err e = ASNGN_ERR_PROTOCOL;
  size_t fields = asmodel_json_object_count(obj);
  if (asmodel_json_typeof(obj) != ASMODEL_JSON_OBJECT) goto out;
  if (step->kind == ASNGN_STEP_ANSWER) {
    if (!fields) e = ASNGN_OK;
    goto out;
  }
  if (step->kind == ASNGN_STEP_OPEN) {
    e = asngn_blob_parse(obj, step);
    if (e == ASNGN_OK && (size_t)step->blob_n > t->s->blobs_n) e = ASNGN_ERR_PROTOCOL;
  } else {
    if (fields != 1) goto out;
    asmodel_json_value *v = asmodel_json_object_get(obj, "input");
    const char *s = asmodel_json_string_value(v);
    size_t n = asmodel_json_string_length(v);
    if (s && n && n <= ASNGN_STEP_TEXT_MAX && n == strlen(s)) {
      step->text = asngn_strdup(s);
      e = step->text ? ASNGN_OK : ASNGN_ERR_NOMEM;
    }
  }
out:
  asmodel_json_free(obj);
  return e;
}

asngn_err asngn_native_steps(asngn_ctx *c, asngn_turn_state *t,
                             const asngn_native_contract *contract, const asmodel_tool_calls *calls,
                             asngn_step steps[32]) {
  memset(steps, 0, sizeof(*steps) * 32);
  if (t->steps >= c->cfg.max_steps || !calls->count || calls->count > 32 ||
      calls->count > (size_t)(c->cfg.max_steps - t->steps))
    return ASNGN_ERR_LIMIT;
  for (size_t i = 0; i < calls->count; i++) {
    const asmodel_tool_call *call = &calls->calls[i];
    size_t j;
    for (j = 0; j < contract->count && strcmp(call->name, contract->schemas[j].name); j++) {
    }
    if (j == contract->count) return ASNGN_ERR_PROTOCOL;
    const astools_selected_command *v = contract->commands[j];
    steps[i].kind = contract->kinds[j];
    if (calls->count > 1 && (!v || !v->read_only || v->destructive))
      return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                          "native batch must contain only read-only, non-destructive tools");
    if (v) {
      astools_err ae = astools_selection_validate(t->tool_selection, v->tool, call->arguments);
      if (ae != ASTOOLS_OK)
        return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "native batch rejected before execution: %s",
                            astools_last_error(c->astools));
      const char *dot = strrchr(v->tool, '.');
      steps[i].call_ref = asngn_strndup(v->tool, (size_t)(dot - v->tool));
      steps[i].call_cmd = asngn_strdup(v->command);
      steps[i].call_args = asngn_strdup(call->arguments);
      if (!steps[i].call_ref || !steps[i].call_cmd || !steps[i].call_args) return ASNGN_ERR_NOMEM;
    } else {
      asngn_err e = control_args(call->arguments, t, &steps[i]);
      if (e != ASNGN_OK) return asngn_seterr(c, e, "invalid arguments for %s", call->name);
    }
  }
  return ASNGN_OK;
}
