/* Editor decisions bind to an immutable runtime request, never to model metadata. */
#include "server.h"
#include <string.h>

void acp_tool_finish(acp_server *s, acp_session *session, bool ok, const char *reason) {
  if (!session->tool_open)
    return;
  acp_json *update = acp_literal("{\"sessionUpdate\":\"tool_call_update\"}");
  int bad = asmodel_json_object_set(update, "toolCallId", asmodel_json_string(session->call_id));
  bad |=
      asmodel_json_object_set(update, "status", asmodel_json_string(ok ? "completed" : "failed"));
  acp_json *output = asmodel_json_object();
  bad |= asmodel_json_object_set(output, "reason", asmodel_json_string(reason));
  bad |= asmodel_json_object_set(update, "rawOutput", output);
  if (bad) {
    asmodel_json_free(update);
    update = NULL;
  }
  acp_update(s, session, update);
  session->tool_open = false;
}
void acp_permission_open(acp_server *s, acp_session *session, const acp_event *event) {
  asngn_approval *a = NULL;
  asngn_err e = asngn_approval_get(session->session, &a);
  if (e != ASNGN_OK) {
    s->failure = e;
    return;
  }
  if (a->status != ASNGN_APPROVAL_PENDING || session->cancelled) {
    asngn_approval_free(a);
    return;
  }
  if (strcmp(a->id, event->approval) || strcmp(a->turn_id, event->turn) ||
      session->awaiting_permission) {
    asngn_approval_free(a);
    s->failure = ASNGN_ERR_PROTOCOL;
    return;
  }
  acp_tool_finish(s, session, false, "action_not_dispatched");
  acp_json *call =
      acp_literal("{\"sessionUpdate\":\"tool_call\",\"kind\":\"other\",\"status\":\"pending\"}");
  acp_json *input = asmodel_json_object();
  char title[196];
  snprintf(title, sizeof title, "%s.%s", a->tool_ref, a->command);
  int bad = asmodel_json_object_set(call, "title", asmodel_json_string(title));
  bad |= asmodel_json_object_set(call, "toolCallId", asmodel_json_string(a->id));
#define FIELD(key) bad |= asmodel_json_object_set(input, #key, asmodel_json_string(a->key))
  FIELD(arguments);
  FIELD(arguments_sha256);
  FIELD(package_sha256);
  FIELD(snapshot);
  FIELD(workspace);
  FIELD(turn_id);
#undef FIELD
  bad |= asmodel_json_object_set(call, "rawInput", input);
  acp_json *params =
      acp_literal("{\"options\":["
                  "{\"optionId\":\"allow-once\",\"name\":\"Allow once\",\"kind\":\"allow_once\"},"
                  "{\"optionId\":\"reject-once\",\"name\":\"Reject\",\"kind\":\"reject_once\"}]}");
  bad |= asmodel_json_object_set(params, "sessionId",
                                 asmodel_json_string(asngn_session_slug(session->session)));
  /* Permission toolCall uses the update shape; sessionUpdate is only for notifications. */
  acp_json *preview = asmodel_json_object();
  const char *keys[] = {"toolCallId", "title", "kind", "status", "rawInput"};
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
    bad |= asmodel_json_object_set(preview, keys[i],
                                   asmodel_json_clone(asmodel_json_object_get(call, keys[i])));
  bad |= asmodel_json_object_set(params, "toolCall", preview);
  if (bad) {
    asmodel_json_free(call);
    asmodel_json_free(params);
    s->failure = ASNGN_ERR_NOMEM;
  } else {
    memcpy(session->permission, a->id, 37);
    memcpy(session->call_id, a->id, 37);
    session->awaiting_permission = session->tool_open = true;
    acp_update(s, session, call);
    acp_permission_request(s, a->id, params);
  }
  asngn_approval_free(a);
}
void acp_permission_reply(acp_server *s, const acp_json *response) {
  const char *id = acp_string(response, "id");
  if (!id)
    return;
  for (size_t i = 0; i < ACP_SESSIONS; i++) {
    acp_session *session = &s->sessions[i];
    if (!session->job || !session->awaiting_permission || strcmp(id, session->permission))
      continue;
    const acp_json *result = asmodel_json_object_get(response, "result");
    const acp_json *outcome = asmodel_json_object_get(result, "outcome");
    const char *kind = acp_string(outcome, "outcome"), *option = acp_string(outcome, "optionId");
    bool selected = kind && !strcmp(kind, "selected") && option &&
                    (!strcmp(option, "allow-once") || !strcmp(option, "reject-once"));
    bool valid = !asmodel_json_object_get(response, "error") &&
                 (selected || (kind && !strcmp(kind, "cancelled") && !option));
    bool allow = valid && selected && !strcmp(option, "allow-once") && !session->cancelled;
    bool abandoned = valid && kind && !strcmp(kind, "cancelled");
    session->awaiting_permission = false;
    asngn_err e = asngn_confirm(s->engine, session->permission, allow, 0);
    if (!allow || e != ASNGN_OK)
      acp_tool_finish(s, session, false,
                      valid ? "permission_denied_or_expired" : "invalid_permission_response");
    if (abandoned || !valid || (e != ASNGN_OK && e != ASNGN_ERR_NOT_FOUND)) {
      session->cancelled = true;
      (void)mcp_job_cancel(session->job);
    }
    return;
  }
}
