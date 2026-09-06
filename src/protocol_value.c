/* Decode provider JSON at the application boundary. Runtime code stays generic. */
#include "asngn_internal.h"
#include "blob.h"
#include "asmodel_json.h"
#include <stdlib.h>
#include <string.h>

static const char *string(const asmodel_json_value *o, const char *key) {
  const asmodel_json_value *v = asmodel_json_object_get(o, key);
  const char *s = asmodel_json_string_value(v);
  return s && strlen(s) == asmodel_json_string_length(v) && *s ? s : NULL;
}
static int quoted(asngn_buf *b, const asmodel_json_value *o, const char *key) {
  if (!string(o, key)) return -1;
  char *text = asmodel_json_write(asmodel_json_object_get(o, key), 0);
  int bad = !text || asngn_buf_appends(b, text) != ASNGN_OK;
  free(text); return bad;
}
static int field(asngn_buf *b, const asmodel_json_value *o, const char *key) {
  return asngn_buf_printf(b, ", %s: ", key) != ASNGN_OK || quoted(b, o, key);
}
static int tool_valid(const char *tool) {
  if (!tool || tool[0] < 'a' || tool[0] > 'z' || !strchr(tool, '.')) return 0;
  for (const unsigned char *p = (const unsigned char *)tool; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || strchr("@.+-", *p))) return 0;
  return 1;
}
static int strings_intact(const asmodel_json_value *v) {
  if (asmodel_json_typeof(v) == ASMODEL_JSON_STRING)
    return strlen(asmodel_json_string_value(v)) == asmodel_json_string_length(v);
  if (asmodel_json_typeof(v) == ASMODEL_JSON_ARRAY) {
    for (size_t i = 0; i < asmodel_json_array_len(v); i++)
      if (!strings_intact(asmodel_json_array_at(v,i))) return 0;
  } else if (asmodel_json_typeof(v) == ASMODEL_JSON_OBJECT) {
    for (size_t i = 0; i < asmodel_json_object_count(v); i++)
      if (!strings_intact(asmodel_json_object_value_at(v,i))) return 0;
  }
  return 1;
}

/* Application state narrows action/tool/handle choices. Tool invocation still
 * validates argument types and host policy against the selected manifest. */
static int admitted(const asmodel_json_value *o, const char *schema) {
  asmodel_json_value *root = NULL;
  if (!schema || asmodel_json_parse(schema,strlen(schema),&root)) return 0;
  const asmodel_json_value *variants = asmodel_json_object_get(root,"oneOf");
  int ok = 0;
  for (size_t i = 0; !ok && i < asmodel_json_array_len(variants); i++) {
    const asmodel_json_value *props = asmodel_json_object_get(asmodel_json_array_at(variants,i),"properties");
    const char *action = string(o,"action");
    const char *allowed = string(asmodel_json_object_get(props,"action"),"const");
    if (!action || !allowed || strcmp(action,allowed)) continue;
    if (!strcmp(action,"call")) {
      const char *tool=string(o,"tool"), *expected=string(asmodel_json_object_get(props,"tool"),"const");
      if (!tool || !expected || strcmp(tool,expected)) continue;
      if ((asmodel_json_object_get(props,"path") != NULL) != (asmodel_json_object_get(o,"path") != NULL)) continue;
    } else if (!strcmp(action,"open")) {
      const asmodel_json_value *input = asmodel_json_object_get(o, "input");
      const asmodel_json_value *input_props =
          asmodel_json_object_get(asmodel_json_object_get(props, "input"), "properties");
      const asmodel_json_value *handles =
          asmodel_json_object_get(asmodel_json_object_get(input_props, "blob"), "enum");
      asngn_step range = {0};
      if (asngn_blob_parse(input, &range) == ASNGN_OK)
        for (size_t j = 0; j < asmodel_json_array_len(handles); j++)
          if (asmodel_json_int_value(asmodel_json_array_at(handles, j)) == range.blob_n) {
            ok = 1;
            break;
          }
      continue;
    }
    ok = 1;
  }
  asmodel_json_free(root);
  return ok;
}

static int step(asngn_buf *b, const asmodel_json_value *o) {
  const char *action = string(o, "action");
  size_t fields = 1;
  if (!action) return -1;
  int call = !strcmp(action, "call");
  int input = strcmp(action, "answer") != 0;
  int why = input && strcmp(action, "think") != 0;
  int recovery = call || !strcmp(action, "recall");
  if (strcmp(action,"answer") && strcmp(action,"think") && strcmp(action,"open") &&
      strcmp(action,"clarify") && strcmp(action,"recall") && strcmp(action,"discover") && !call) return -1;
  if (asngn_buf_appends(b, "{action: ") != ASNGN_OK || quoted(b, o, "action")) return -1;
  if (why) { fields++; if (field(b, o, "why")) return -1; }
  if (input) {
    fields++;
    if (asngn_buf_appends(b, ", input: ") != ASNGN_OK) return -1;
    if (call) {
      const char *tool = string(o, "tool");
      const asmodel_json_value *args = asmodel_json_object_get(o, "arguments");
      const char *path = string(o, "path");
      fields++;
      if (!tool_valid(tool) || asngn_buf_printf(b, "%s ", tool) != ASNGN_OK) return -1;
      if (path && !strcmp(tool, "fs.write") && !args) {
        if (asngn_buf_appends(b, "{path: ") != ASNGN_OK || quoted(b, o, "path") ||
            asngn_buf_appends(b, ", content: \"@asngn:draft\"}") != ASNGN_OK) return -1;
      } else {
        if (path || asmodel_json_typeof(args) != ASMODEL_JSON_OBJECT || !strings_intact(args)) return -1;
        char *text = asmodel_json_write(args, 0);
        int bad = !text || asngn_buf_appends(b, text) != ASNGN_OK;
        free(text); if (bad) return -1;
      }
    } else if (!strcmp(action, "open")) {
      const asmodel_json_value *input = asmodel_json_object_get(o, "input");
      asngn_step range = {0};
      if (asngn_blob_parse(input, &range) != ASNGN_OK) return -1;
      char *text = asmodel_json_write(input, 0);
      int bad = !text || asngn_buf_appends(b, text) != ASNGN_OK;
      free(text);
      if (bad) return -1;
    } else if (quoted(b, o, "input")) return -1;
  }
  if (recovery) {
    fields += 2;
    if (field(b, o, "success") || field(b, o, "fallback")) return -1;
  }
  return asmodel_json_object_count(o) != fields || asngn_buf_appends(b, "}\n") != ASNGN_OK;
}

static int choice(const char *s, const char *values) {
  if (!s) return 0;
  size_t n = strlen(s);
  for (const char *p = values; *p;) {
    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end-p) : strlen(p);
    if (len == n && !memcmp(p,s,n)) return 1;
    p += len; if (*p) p++;
  }
  return 0;
}

asngn_err asngn_protocol_decode(asngn_task_kind kind, const char *schema, char **text) {
  asmodel_json_value *o = NULL;
  asngn_buf b;
  int bad = !text || !*text;
  if (bad || asmodel_json_parse(*text, strlen(*text), &o)) return ASNGN_ERR_PROTOCOL;
  asngn_buf_init(&b);
  if (asmodel_json_typeof(o) != ASMODEL_JSON_OBJECT) bad = 1;
  else if (kind == ASNGN_TASK_DECIDE) bad = !admitted(o,schema) || step(&b,o);
  else if (kind == ASNGN_TASK_CLASSIFY) {
    const char *a=string(o,"class"), *d=string(o,"detail");
    const char *m=string(o,"mode"), *t=string(o,"task");
    bad = !choice(a,"SIMPLE|MODERATE|COMPLEX") || !choice(d,"TERSE|NORMAL|RICH") ||
        !choice(m,"DIRECT|PLAN") ||
        !choice(t,"CHAT|LOOKUP|EXPLAIN|EDIT|BUILD|GENERATE|REFACTOR|DEBUG") ||
        asmodel_json_object_count(o) != 4;
    if (!bad) bad = asngn_buf_printf(&b, "CLASS %s | DETAIL %s | MODE %s | TASK %s\n",
                                     a,d,m,t) != ASNGN_OK;
  } else if (kind == ASNGN_TASK_JUDGE) {
    const asmodel_json_value *score = asmodel_json_object_get(o, "score");
    const char *critique = string(o, "critique");
    long long n = asmodel_json_int_value(score);
    bad = !asmodel_json_is_int(score) || n < 0 || n > 10 || !critique ||
        asmodel_json_object_count(o) != 2 || strlen(critique) > ASNGN_STEP_META_MAX ||
        strpbrk(critique, "|\r\n") != NULL;
    if (!bad) bad = asngn_buf_printf(&b, "SCORE %lld | %s\n", n, critique) != ASNGN_OK;
  } else bad = 1;
  asmodel_json_free(o);
  if (!bad) { free(*text); *text = asngn_buf_detach(&b); }
  asngn_buf_free(&b);
  return bad ? ASNGN_ERR_PROTOCOL : ASNGN_OK;
}
