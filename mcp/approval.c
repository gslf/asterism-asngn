/* Read-only inspection; an MCP tool client cannot approve its own actions. */
#include "approval.h"

asngn_err mcp_approval_read(asngn_session *s, asmodel_json_value **out) {
  static const char *const names[] = {"pending",  "approved",    "denied",
                                      "consumed", "invalidated", "interrupted"};
  asngn_approval *a = NULL;
  *out = NULL;
  asngn_err e = asngn_approval_get(s, &a);
  if (e != ASNGN_OK && e != ASNGN_ERR_NOT_FOUND) return e;
  asmodel_json_value *v = asmodel_json_object();
  int bad = !v || asmodel_json_object_set(v, "status",
                                          asmodel_json_string(a ? names[a->status] : "none"));
  if (a) {
    bad |= asmodel_json_object_set(v, "sequence", asmodel_json_int((long long)a->sequence));
    bad |= asmodel_json_object_set(v, "session_wide", asmodel_json_bool(a->session_wide));
    bad |= asmodel_json_object_set(v, "profile",
                                   asmodel_json_string(asngn_security_profile_name(a->profile)));
#define STRING(name) bad |= asmodel_json_object_set(v, #name, asmodel_json_string(a->name))
    STRING(id);
    STRING(turn_id);
    STRING(tool_ref);
    STRING(command);
    STRING(arguments);
    STRING(arguments_sha256);
    STRING(package_sha256);
    STRING(snapshot);
    STRING(workspace);
#undef STRING
  }
  asngn_approval_free(a);
  if (bad) {
    asmodel_json_free(v);
    return ASNGN_ERR_NOMEM;
  }
  *out = v;
  return ASNGN_OK;
}
