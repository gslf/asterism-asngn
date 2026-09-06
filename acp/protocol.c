/* Strict JSON-RPC envelopes; helpers consume their result/params/update value. */
#include "server.h"
#include <stdlib.h>
#include <string.h>

const char *acp_string(const acp_json *object, const char *key) {
  const acp_json *v = asmodel_json_object_get(object, key);
  const char *text = asmodel_json_string_value(v);
  return text && strlen(text) == asmodel_json_string_length(v) ? text : NULL;
}
bool acp_id_valid(const acp_json *id) {
  switch (asmodel_json_typeof(id)) {
  case ASMODEL_JSON_NULL:
    return id != NULL;
  case ASMODEL_JSON_STRING: {
    const char *s = asmodel_json_string_value(id);
    return asmodel_json_string_length(id) <= 128 && strlen(s) == asmodel_json_string_length(id);
  }
  case ASMODEL_JSON_NUMBER:
    return asmodel_json_is_int(id) != 0;
  default:
    return false;
  }
}
bool acp_id_equal(const acp_json *a, const acp_json *b) {
  if (!a || !b || asmodel_json_typeof(a) != asmodel_json_typeof(b))
    return false;
  switch (asmodel_json_typeof(a)) {
  case ASMODEL_JSON_NULL:
    return true;
  case ASMODEL_JSON_NUMBER:
    return asmodel_json_int_value(a) == asmodel_json_int_value(b);
  case ASMODEL_JSON_STRING:
    return asmodel_json_string_length(a) == asmodel_json_string_length(b) &&
           !strcmp(asmodel_json_string_value(a), asmodel_json_string_value(b));
  default:
    return false;
  }
}
acp_json *acp_literal(const char *text) {
  acp_json *v = NULL;
  (void)asmodel_json_parse(text, strlen(text), &v);
  return v;
}
static void send(acp_server *s, acp_json *packet, int bad) {
  char *wire = bad ? NULL : asmodel_json_write(packet, 0);
  asmodel_json_free(packet);
  if (s->failure == ASNGN_OK)
    s->failure = wire ? acp_io_queue(&s->io, wire, strlen(wire)) : ASNGN_ERR_NOMEM;
  free(wire);
}
static void response(acp_server *s, const acp_json *id, const char *kind, acp_json *value) {
  acp_json *packet = asmodel_json_object();
  int bad = !packet;
  bad |= asmodel_json_object_set(packet, "jsonrpc", asmodel_json_string("2.0"));
  bad |= asmodel_json_object_set(packet, "id", id ? asmodel_json_clone(id) : asmodel_json_null());
  bad |= asmodel_json_object_set(packet, kind, value);
  send(s, packet, bad);
}
void acp_reply(acp_server *s, const acp_json *id, acp_json *result) {
  response(s, id, "result", result);
}
void acp_error(acp_server *s, const acp_json *id, int code, const char *message,
               asngn_err outcome) {
  acp_json *error = asmodel_json_object(), *data = asmodel_json_object();
  int bad = !error || !data;
  bad |= asmodel_json_object_set(error, "code", asmodel_json_int(code));
  bad |= asmodel_json_object_set(error, "message", asmodel_json_string(message));
  bad |= asmodel_json_object_set(data, "outcome", asmodel_json_string(asngn_err_name(outcome)));
  bad |= asmodel_json_object_set(error, "data", data);
  if (bad) {
    asmodel_json_free(error);
    error = NULL;
  }
  response(s, id, "error", error);
}
static void request(acp_server *s, const char *method, const char *id, acp_json *params) {
  acp_json *packet = asmodel_json_object();
  int bad = !packet;
  bad |= asmodel_json_object_set(packet, "jsonrpc", asmodel_json_string("2.0"));
  bad |= asmodel_json_object_set(packet, "method", asmodel_json_string(method));
  if (id)
    bad |= asmodel_json_object_set(packet, "id", asmodel_json_string(id));
  bad |= asmodel_json_object_set(packet, "params", params);
  send(s, packet, bad);
}
void acp_update(acp_server *s, acp_session *session, acp_json *update) {
  acp_json *params = asmodel_json_object();
  int bad = !params;
  bad |= asmodel_json_object_set(params, "sessionId",
                                 asmodel_json_string(asngn_session_slug(session->session)));
  bad |= asmodel_json_object_set(params, "update", update);
  if (bad) {
    asmodel_json_free(params);
    params = NULL;
  }
  request(s, "session/update", NULL, params);
}
void acp_permission_request(acp_server *s, const char *id, acp_json *params) {
  request(s, "session/request_permission", id, params);
}
