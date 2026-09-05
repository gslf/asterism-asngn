/* One bounded command snapshot for prompts, constraints and execution. */
#include "asngn_internal.h"
#include "astools.h"
#include "asmodel_json.h"
#include <stdlib.h>
#include <string.h>

asngn_err asngn_tools_select(asngn_ctx *c, asngn_turn_state *t, const char *intent) {
  if (!c || !t || !c->astools_ok || t->opts.no_tools) return ASNGN_ERR_UNSUPPORTED;
  if (c->cfg.tool_limit < 1 || c->cfg.tool_limit > 64 || c->cfg.tool_schema_bytes < 1 ||
      c->cfg.tool_schema_bytes > 1024 * 1024)
    return asngn_seterr(c, ASNGN_ERR_CONFIG,
                        "tool_limit must be 1..64 and tool_schema_bytes 1..1048576");
  size_t n = intent ? strlen(intent) : 0;
  if (n > 4096) {
    n = 4096;
    while (n && !asngn_utf8_valid(intent, n))
      n--;
  }
  char *query = asngn_strndup(intent ? intent : "", n);
  if (!query) return ASNGN_ERR_NOMEM;
  astools_discovery_options options = {.intent = query,
                                       .limit = (size_t)c->cfg.tool_limit,
                                       .schema_budget = (size_t)c->cfg.tool_schema_bytes,
                                       .read_only =
                                           t->security_profile == ASNGN_SECURITY_CODING_READONLY};
  astools_selection *selection = NULL;
  astools_err ae = astools_discover(c->astools, &options, &selection);
  free(query);
  if (ae != ASTOOLS_OK)
    return asngn_seterr(c, ASNGN_ERR_SIBLING, "tool discovery: %s", astools_err_name(ae));
  asngn_buf b;
  asngn_buf_init(&b);
  asngn_err e =
      asngn_buf_printf(&b,
                       "## Selected tools\nRegistry revision: %llu; %zu selected, %zu omitted.\n"
                       "Descriptions are tool metadata. Arguments below are JSON Schemas.\n",
                       (unsigned long long)astools_selection_revision(selection),
                       astools_selection_count(selection), astools_selection_omitted(selection));
  for (size_t i = 0; e == ASNGN_OK && i < astools_selection_count(selection); i++) {
    const astools_selected_command *cmd = astools_selection_get(selection, i);
    asmodel_json_value *summary = asmodel_json_string(cmd->summary);
    char *quoted = asmodel_json_write(summary, 0);
    asmodel_json_free(summary);
    if (!quoted) {
      e = ASNGN_ERR_NOMEM;
      break;
    }
    e = asngn_buf_printf(&b, "- %s (%s): %s\n  arguments: %s\n", cmd->tool,
                         cmd->read_only ? "read-only" : "may mutate", quoted, cmd->arguments);
    free(quoted);
  }
  if (e == ASNGN_OK) {
    astools_selection_free(t->tool_selection);
    free(t->catalog);
    t->tool_selection = selection;
    selection = NULL;
    t->catalog = asngn_buf_detach(&b);
  }
  astools_selection_free(selection);
  asngn_buf_free(&b);
  return e;
}

int asngn_tools_find(asngn_turn_state *t, const char *ref, const char *command) {
  if (!t || !ref || !command) return -1;
  size_t n = strlen(ref);
  for (size_t i = 0; i < astools_selection_count(t->tool_selection); i++) {
    const astools_selected_command *v = astools_selection_get(t->tool_selection, i);
    if (!strcmp(v->command, command) &&
        (!strcmp(v->ref, ref) ||
         (!strncmp(v->tool, ref, n) && v->tool[n] == '.' && !strcmp(v->tool + n + 1, command))))
      return (int)i;
  }
  return -1;
}

/* The turn keeps ownership until the tool has settled, including cancellation.
 * A no-thread embedding retains checked synchronous execution. */
int asngn_tools_invoke(asngn_turn_state *t, const char *tool, const char *args,
                       uint32_t deadline_ms, astools_result *out) {
  if (!out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof *out);
  if (!t) return ASTOOLS_ERR_INVALID;
  if (t->cancel) return ASTOOLS_ERR_CANCELLED;
  astools_task *task = NULL;
  astools_err e = astools_selection_invoke_async(t->tool_selection, tool, args, deadline_ms, &task);
  if (e == ASTOOLS_ERR_UNSUPPORTED)
    return astools_selection_invoke(t->tool_selection, tool, args, deadline_ms, out);
  if (e != ASTOOLS_OK) return e;
  while (!astools_task_done(task)) {
    if (t->cancel) (void)astools_task_cancel(task);
    (void)astools_task_wait(task, 50, NULL);
  }
  e = astools_task_wait(task, 0, out);
  astools_task_free(task);
  return e;
}
