/* Application output contracts. Providers receive schemas, never protocol names. */
#include "asngn_internal.h"
#include "astools.h"
#include "asmodel_json.h"
#include <stdlib.h>
#include <string.h>

static asmodel_json_value *string_schema(size_t max, const char *constant) {
  asmodel_json_value *v = asmodel_json_object();
  int bad = asmodel_json_object_set(v, "type", asmodel_json_string("string"));
  if (constant) bad |= asmodel_json_object_set(v, "const", asmodel_json_string(constant));
  else {
    bad |= asmodel_json_object_set(v, "minLength", asmodel_json_int(1));
    bad |= asmodel_json_object_set(v, "maxLength", asmodel_json_int((long long)max));
  }
  if (bad) { asmodel_json_free(v); return NULL; }
  return v;
}
static int field(asmodel_json_value *props, asmodel_json_value *required, const char *key, asmodel_json_value *v) {
  int bad = asmodel_json_object_set(props, key, v);
  bad |= asmodel_json_array_push(required, asmodel_json_string(key));
  return bad;
}

static asmodel_json_value *variant(const char *action, const char *tool,
                          const asmodel_json_value *args, size_t blobs, int draft) {
  asmodel_json_value *v = asmodel_json_object(), *props = asmodel_json_object(), *req = asmodel_json_array();
  int input = strcmp(action, "answer") != 0;
  int why = input && strcmp(action, "think") != 0;
  int recovery = !strcmp(action, "call") || !strcmp(action, "recall");
  int bad = field(props, req, "action", string_schema(0, action));
  if (why) bad |= field(props, req, "why", string_schema(ASNGN_STEP_META_MAX, NULL));
  if (tool) {
    bad |= field(props, req, "tool", string_schema(0, tool));
    bad |= field(props, req, draft ? "path" : "arguments",
                 draft ? string_schema(512, NULL) : asmodel_json_clone(args));
  } else if (input) {
    asmodel_json_value *s = string_schema(ASNGN_STEP_TEXT_MAX, NULL);
    if (!strcmp(action, "open")) {
      asmodel_json_value *handles = asmodel_json_array();
      for (size_t i = 1; i <= blobs; i++) {
        char handle[32]; snprintf(handle, sizeof handle, "B%zu", i);
        bad |= asmodel_json_array_push(handles, asmodel_json_string(handle));
      }
      bad |= asmodel_json_object_set(s, "enum", handles);
    }
    bad |= field(props, req, "input", s);
  }
  if (recovery) {
    bad |= field(props, req, "success", string_schema(ASNGN_STEP_META_MAX, NULL));
    bad |= field(props, req, "fallback", string_schema(ASNGN_STEP_META_MAX, NULL));
  }
  bad |= asmodel_json_object_set(v, "type", asmodel_json_string("object"));
  bad |= asmodel_json_object_set(v, "properties", props);
  bad |= asmodel_json_object_set(v, "required", req);
  bad |= asmodel_json_object_set(v, "additionalProperties", asmodel_json_bool(0));
  if (bad) { asmodel_json_free(v); return NULL; }
  return v;
}

asngn_err asngn_protocol_steps(asngn_ctx *c, bool call, bool recall,
    bool think, size_t blobs, bool draft, char **out) {
  asmodel_json_value *root = asmodel_json_object(), *variants = asmodel_json_array(), *commands = NULL;
  char *text = NULL;
  int bad = 0;
  if (!out || (call && !c)) { asmodel_json_free(root); asmodel_json_free(variants); return ASNGN_ERR_INVALID; }
  *out = NULL;
  if (call) {
    if (!c->astools || astools_command_schemas(c->astools, &text) != ASTOOLS_OK ||
        asmodel_json_parse(text, strlen(text), &commands)) bad = 1;
    for (size_t i = 0; !bad && i < asmodel_json_array_len(commands); i++) {
      const asmodel_json_value *cmd = asmodel_json_array_at(commands, i);
      const char *tool = asmodel_json_string_value(asmodel_json_object_get(cmd, "tool"));
      if (!tool) { bad = 1; break; }
      if (draft && (!strcmp(tool, "edit.replace") || !strcmp(tool, "edit.insert") ||
                    !strcmp(tool, "edit.patch"))) continue;
      bad |= asmodel_json_array_push(variants, variant("call", tool,
          asmodel_json_object_get(cmd, "arguments"), 0, draft && !strcmp(tool, "fs.write")));
    }
  }
  if (recall) bad |= asmodel_json_array_push(variants, variant("recall", NULL, NULL, 0, 0));
  if (blobs) bad |= asmodel_json_array_push(variants, variant("open", NULL, NULL, blobs, 0));
  if (think) bad |= asmodel_json_array_push(variants, variant("think", NULL, NULL, 0, 0));
  bad |= asmodel_json_array_push(variants, variant("clarify", NULL, NULL, 0, 0));
  bad |= asmodel_json_array_push(variants, variant("answer", NULL, NULL, 0, 0));
  bad |= asmodel_json_object_set(root, "oneOf", variants);
  if (!bad) *out = asmodel_json_write(root, 0);
  asmodel_json_free(root); asmodel_json_free(commands); astools_free(text);
  return *out ? ASNGN_OK : ASNGN_ERR_PROTOCOL;
}

const char *asngn_protocol_scalar_schema(asngn_task_kind kind) {
  if (kind == ASNGN_TASK_CLASSIFY) return
      "{\"type\":\"object\",\"properties\":{"
      "\"class\":{\"type\":\"string\",\"enum\":[\"SIMPLE\",\"MODERATE\",\"COMPLEX\"]},"
      "\"detail\":{\"type\":\"string\",\"enum\":[\"TERSE\",\"NORMAL\",\"RICH\"]},"
      "\"mode\":{\"type\":\"string\",\"enum\":[\"DIRECT\",\"PLAN\"]},"
      "\"task\":{\"type\":\"string\",\"enum\":[\"CHAT\",\"LOOKUP\",\"EXPLAIN\",\"EDIT\",\"BUILD\",\"GENERATE\",\"REFACTOR\",\"DEBUG\"]}},"
      "\"required\":[\"class\",\"detail\",\"mode\",\"task\"],\"additionalProperties\":false}";
  if (kind == ASNGN_TASK_JUDGE) return
      "{\"type\":\"object\",\"properties\":{"
      "\"score\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},"
      "\"critique\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512}},"
      "\"required\":[\"score\",\"critique\"],\"additionalProperties\":false}";
  return NULL;
}
