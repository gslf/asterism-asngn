/* Only requests from the owning editor can create sessions or grant actions. */
#include "server.h"
#include <string.h>

static void initialize(acp_server *s, const acp_json *id, const acp_json *params) {
  const acp_json *version = asmodel_json_object_get(params, "protocolVersion");
  if (s->initialized || !asmodel_json_is_int(version) || asmodel_json_int_value(version) < 0 ||
      asmodel_json_int_value(version) > 65535) {
    acp_error(s, id, -32602, "Initialization requires a protocol version and runs once",
              ASNGN_ERR_INVALID);
    return;
  }
  acp_json *result = acp_literal(
      "{\"protocolVersion\":1,\"agentCapabilities\":{\"loadSession\":false,"
      "\"promptCapabilities\":{\"image\":false,\"audio\":false,\"embeddedContext\":false},"
      "\"mcpCapabilities\":{\"http\":false,\"sse\":false},"
      "\"sessionCapabilities\":{\"close\":{}}},\"authMethods\":[],"
      "\"agentInfo\":{\"name\":\"asngn-acp\",\"title\":\"asngn\"}}");
  if (asmodel_json_object_set(asmodel_json_object_get(result, "agentInfo"), "version",
                              asmodel_json_string(asngn_version()))) {
    asmodel_json_free(result);
    result = NULL;
  }
  acp_reply(s, id, result);
  s->initialized = s->failure == ASNGN_OK;
}

void acp_dispatch(acp_server *s, const acp_json *request) {
  const acp_json *id = asmodel_json_object_get(request, "id");
  const char *version = acp_string(request, "jsonrpc");
  const char *method = acp_string(request, "method");
  const acp_json *params = asmodel_json_object_get(request, "params");
  if (!version || strcmp(version, "2.0") || (id && !acp_id_valid(id))) {
    acp_error(s, NULL, -32600, "Invalid JSON-RPC envelope", ASNGN_ERR_INVALID);
    return;
  }
  if (!asmodel_json_object_get(request, "method")) {
    /* Responses belong only to outstanding permission requests. */
    acp_permission_reply(s, request);
    return;
  }
  if (!id) {
    if (method && s->initialized && !strcmp(method, "session/cancel") &&
        !asmodel_json_object_get(request, "result") && !asmodel_json_object_get(request, "error")) {
      acp_session *session = acp_session_find(s, acp_string(params, "sessionId"));
      if (session && session->job) {
        session->cancelled = true;
        (void)mcp_job_cancel(session->job);
      }
    }
    return;
  }
  if (!method || asmodel_json_object_get(request, "result") ||
      asmodel_json_object_get(request, "error")) {
    acp_error(s, id, -32600, "Invalid request", ASNGN_ERR_INVALID);
    return;
  }
  for (size_t i = 0; i < ACP_SESSIONS; i++) {
    if (acp_id_equal(id, s->sessions[i].prompt_id) || acp_id_equal(id, s->sessions[i].close_id)) {
      /* A second response with that ID would be ambiguous to the peer. */
      s->failure = ASNGN_ERR_INVALID;
      return;
    }
  }
  if (asmodel_json_typeof(params) != ASMODEL_JSON_OBJECT) {
    acp_error(s, id, -32602, "Parameters must be an object", ASNGN_ERR_INVALID);
    return;
  }
  if (!strcmp(method, "initialize")) {
    initialize(s, id, params);
    return;
  }
  if (!s->initialized) {
    acp_error(s, id, -32000, "Initialize before opening a session", ASNGN_ERR_INVALID);
    return;
  }
  if (!strcmp(method, "session/new")) {
    acp_session_new(s, id, params);
    return;
  }
  if (strcmp(method, "session/prompt") && strcmp(method, "session/close")) {
    acp_error(s, id, -32601, "Method is not supported by this host", ASNGN_ERR_INVALID);
    return;
  }
  acp_session *session = acp_session_find(s, acp_string(params, "sessionId"));
  if (!session) {
    acp_error(s, id, -32602, "Unknown session", ASNGN_ERR_NOT_FOUND);
    return;
  }
  if (session->close_id) {
    acp_error(s, id, -32000, "Session is closing", ASNGN_ERR_BUSY);
    return;
  }
  if (!strcmp(method, "session/prompt")) {
    acp_prompt(s, session, id, params);
    return;
  }
  if (!session->job) {
    acp_session_drop(session);
    acp_reply(s, id, asmodel_json_object());
  } else {
    session->close_id = asmodel_json_clone(id);
    if (!session->close_id)
      s->failure = ASNGN_ERR_NOMEM;
    session->cancelled = true;
    (void)mcp_job_cancel(session->job);
  }
}
