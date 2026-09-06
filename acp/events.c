/* Copy bounded, typed host events; API calls happen only on the stdio thread. */
#include "server.h"
#include "xcdn.h"
#include <string.h>

static bool string(const xcdn_value_t *object, const char *key, char *out, size_t capacity) {
  const char *text = asngn_xstr(asngn_xfield(object, key));
  if (!text || strlen(text) >= capacity)
    return false;
  memcpy(out, text, strlen(text) + 1);
  return true;
}
void acp_events_receive(const char *text, void *userdata) {
  acp_server *s = userdata;
  acp_event event = {0};
  xcdn_error_t error;
  xcdn_document_t *doc = xcdn_parse(text, &error);
  const xcdn_value_t *root = doc && doc->values_len == 1 ? doc->values[0]->value : NULL;
  const char *kind = asngn_xstr(asngn_xfield(root, "kind"));
  if (kind && strcmp(kind, "confirm") && strcmp(kind, "action")) {
    xcdn_document_free(doc);
    return;
  }
  const xcdn_value_t *data = asngn_xfield(root, "data");
  bool valid = kind && string(root, "session", event.session, sizeof event.session) &&
               asngn_xuuid(asngn_xfield(root, "span"), event.turn) && asngn_uuid_valid(event.turn);
  if (valid && !strcmp(kind, "confirm")) {
    event.kind = ACP_CONFIRM;
    valid = asngn_xuuid(asngn_xfield(data, "confirm_id"), event.approval) &&
            asngn_uuid_valid(event.approval);
  } else if (valid) {
    char state[24], turn[37];
    valid = string(data, "state", state, sizeof state) &&
            string(data, "turn_id", turn, sizeof turn) && !strcmp(turn, event.turn) &&
            string(data, "action_id", event.action, sizeof event.action) &&
            asngn_uuid_valid(event.action) &&
            string(data, "approval_id", event.approval, sizeof event.approval) &&
            (!event.approval[0] || asngn_uuid_valid(event.approval)) &&
            string(data, "tool_ref", event.tool, sizeof event.tool) &&
            string(data, "command", event.command, sizeof event.command) &&
            asngn_xbool(asngn_xfield(data, "journaled"), &event.journaled);
    if (valid && !strcmp(state, "dispatching"))
      event.kind = ACP_DISPATCH;
    else if (valid && !strcmp(state, "observed")) {
      event.kind = ACP_OBSERVED;
      valid = asngn_xbool(asngn_xfield(data, "tool_ok"), &event.ok) &&
              string(data, "dispatch_error", event.error, sizeof event.error);
    } else
      valid = false;
  }
  xcdn_document_free(doc);
  os_mutex_lock(&s->events_mutex);
  if (!valid || s->event_count == ACP_EVENTS)
    s->event_error = valid ? ASNGN_ERR_LIMIT : ASNGN_ERR_PARSE;
  else {
    s->events[(s->event_head + s->event_count) % ACP_EVENTS] = event;
    s->event_count++;
  }
  os_mutex_unlock(&s->events_mutex);
}

static void action(acp_server *s, acp_session *session, const acp_event *event) {
  const char *id = event->approval[0] ? event->approval : event->action;
  bool known = session->tool_open && !strcmp(session->call_id, id);
  if (event->kind == ACP_OBSERVED && !known) {
    s->failure = ASNGN_ERR_PROTOCOL;
    return;
  }
  if (session->tool_open && !known)
    acp_tool_finish(s, session, false, "action_not_dispatched");
  acp_json *update = asmodel_json_object(), *meta = asmodel_json_object();
  acp_json *details = asmodel_json_object();
  int bad = asmodel_json_object_set(update, "sessionUpdate",
                                    asmodel_json_string(known ? "tool_call_update" : "tool_call"));
  bad |= asmodel_json_object_set(update, "toolCallId", asmodel_json_string(id));
  if (!known) {
    char title[196];
    snprintf(title, sizeof title, "%s.%s", event->tool, event->command);
    bad |= asmodel_json_object_set(update, "title", asmodel_json_string(title));
    bad |= asmodel_json_object_set(update, "kind", asmodel_json_string("other"));
  }
  bad |= asmodel_json_object_set(update, "status",
                                 asmodel_json_string(event->kind == ACP_DISPATCH ? "in_progress"
                                                     : event->ok                 ? "completed"
                                                                                 : "failed"));
  bad |= asmodel_json_object_set(details, "action_id", asmodel_json_string(event->action));
  bad |= asmodel_json_object_set(details, "journaled", asmodel_json_bool(event->journaled));
  if (event->kind == ACP_OBSERVED)
    bad |= asmodel_json_object_set(details, "dispatch_error", asmodel_json_string(event->error));
  bad |= asmodel_json_object_set(meta, "dev.asterism/asngn", details);
  bad |= asmodel_json_object_set(update, "_meta", meta);
  if (bad) {
    asmodel_json_free(update);
    update = NULL;
  }
  acp_update(s, session, update);
  memcpy(session->call_id, id, 37);
  session->tool_open = event->kind == ACP_DISPATCH;
}

/* Implemented separately to keep permission decisions apart from observations. */
void acp_permission_open(acp_server *, acp_session *, const acp_event *);
void acp_events_drain(acp_server *s) {
  for (size_t i = 0; i < ACP_EVENTS && s->failure == ASNGN_OK; i++) {
    acp_event event;
    os_mutex_lock(&s->events_mutex);
    s->failure = s->event_error;
    bool have = s->event_count != 0;
    if (have) {
      event = s->events[s->event_head];
      s->event_head = (s->event_head + 1) % ACP_EVENTS;
      s->event_count--;
    }
    os_mutex_unlock(&s->events_mutex);
    if (!have || s->failure != ASNGN_OK)
      return;
    acp_session *session = acp_session_find(s, event.session);
    if (!session || !session->job || strcmp(event.turn, mcp_job_id(session->job)))
      continue;
    if (event.kind == ACP_CONFIRM)
      acp_permission_open(s, session, &event);
    else
      action(s, session, &event);
  }
}
