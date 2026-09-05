/* Application output contracts. Providers receive schemas, never protocol names. */
#include "asngn_internal.h"
#include "astools.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

static jx_value *string_schema(size_t max, const char *constant) {
  jx_value *v = jx_object();
  int bad = jx_object_set(v, "type", jx_string("string"));
  if (constant) bad |= jx_object_set(v, "const", jx_string(constant));
  else {
    bad |= jx_object_set(v, "minLength", jx_int(1));
    bad |= jx_object_set(v, "maxLength", jx_int((long long)max));
  }
  if (bad) { jx_free(v); return NULL; }
  return v;
}
static int field(jx_value *props, jx_value *required, const char *key, jx_value *v) {
  int bad = jx_object_set(props, key, v);
  bad |= jx_array_push(required, jx_string(key));
  return bad;
}

static jx_value *variant(const char *action, const char *tool,
                          const jx_value *args, size_t blobs, int draft) {
  jx_value *v = jx_object(), *props = jx_object(), *req = jx_array();
  int input = strcmp(action, "answer") != 0;
  int why = input && strcmp(action, "think") != 0;
  int recovery = !strcmp(action, "call") || !strcmp(action, "recall");
  int bad = field(props, req, "action", string_schema(0, action));
  if (why) bad |= field(props, req, "why", string_schema(ASNGN_STEP_META_MAX, NULL));
  if (tool) {
    bad |= field(props, req, "tool", string_schema(0, tool));
    bad |= field(props, req, draft ? "path" : "arguments",
                 draft ? string_schema(512, NULL) : jx_clone(args));
  } else if (input) {
    jx_value *s = string_schema(ASNGN_STEP_TEXT_MAX, NULL);
    if (!strcmp(action, "open")) {
      jx_value *handles = jx_array();
      for (size_t i = 1; i <= blobs; i++) {
        char handle[32]; snprintf(handle, sizeof handle, "B%zu", i);
        bad |= jx_array_push(handles, jx_string(handle));
      }
      bad |= jx_object_set(s, "enum", handles);
    }
    bad |= field(props, req, "input", s);
  }
  if (recovery) {
    bad |= field(props, req, "success", string_schema(ASNGN_STEP_META_MAX, NULL));
    bad |= field(props, req, "fallback", string_schema(ASNGN_STEP_META_MAX, NULL));
  }
  bad |= jx_object_set(v, "type", jx_string("object"));
  bad |= jx_object_set(v, "properties", props);
  bad |= jx_object_set(v, "required", req);
  bad |= jx_object_set(v, "additionalProperties", jx_bool(0));
  if (bad) { jx_free(v); return NULL; }
  return v;
}

asngn_err asngn_protocol_steps(asngn_ctx *c, bool call, bool recall,
    bool think, size_t blobs, bool draft, char **out) {
  jx_value *root = jx_object(), *variants = jx_array(), *commands = NULL;
  char *text = NULL;
  int bad = 0;
  if (!out || (call && !c)) { jx_free(root); jx_free(variants); return ASNGN_ERR_INVALID; }
  *out = NULL;
  if (call) {
    if (!c->astools || astools_command_schemas(c->astools, &text) != ASTOOLS_OK ||
        jx_parse(text, strlen(text), &commands)) bad = 1;
    for (size_t i = 0; !bad && i < jx_array_len(commands); i++) {
      const jx_value *cmd = jx_array_at(commands, i);
      const char *tool = jx_string_value(jx_object_get(cmd, "tool"));
      if (!tool) { bad = 1; break; }
      if (draft && (!strcmp(tool, "edit.replace") || !strcmp(tool, "edit.insert") ||
                    !strcmp(tool, "edit.patch"))) continue;
      bad |= jx_array_push(variants, variant("call", tool,
          jx_object_get(cmd, "arguments"), 0, draft && !strcmp(tool, "fs.write")));
    }
  }
  if (recall) bad |= jx_array_push(variants, variant("recall", NULL, NULL, 0, 0));
  if (blobs) bad |= jx_array_push(variants, variant("open", NULL, NULL, blobs, 0));
  if (think) bad |= jx_array_push(variants, variant("think", NULL, NULL, 0, 0));
  bad |= jx_array_push(variants, variant("clarify", NULL, NULL, 0, 0));
  bad |= jx_array_push(variants, variant("answer", NULL, NULL, 0, 0));
  bad |= jx_object_set(root, "oneOf", variants);
  if (!bad) *out = jx_write(root, 0);
  jx_free(root); jx_free(commands); astools_free(text);
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
