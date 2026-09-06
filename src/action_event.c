/* Runtime observations for hosts; these events are never verification receipts. */
#include "asmodel_json.h"
#include "execution.h"
#include <stdlib.h>
#include <string.h>

void asngn_action_event(asngn_turn_state *t, const char *ref, const char *command, const char *args,
                        const astools_result *result, astools_err error, bool journaled) {
  asmodel_json_value *v = asmodel_json_object();
  uint8_t digest[32];
  char hash[65];
  const char *input = args ? args : "{}";
  asngn_sha256(input, strlen(input), digest);
  asngn_sha256_hex(digest, 32, hash);
  int bad = !v;
#define STRING(key, value) bad |= asmodel_json_object_set(v, key, asmodel_json_string(value))
#define BOOL(key, value) bad |= asmodel_json_object_set(v, key, asmodel_json_bool(value))
  bad |= asmodel_json_object_set(v, "schema", asmodel_json_int(1));
  STRING("action_id", t->action_id);
  STRING("turn_id", t->span_root);
  STRING("approval_id", t->approval_id);
  STRING("tool_ref", ref);
  STRING("command", command);
  STRING("arguments_sha256", hash);
  STRING("state", result ? "observed" : "dispatching");
  BOOL("journaled", journaled);
  if (result) {
    STRING("dispatch_error", astools_err_name(error));
    BOOL("tool_ok", error == ASTOOLS_OK && result->ok);
    /* The payload remains reopenable through the action journal, not telemetry. */
    const char *body = result->result_xcdn  ? result->result_xcdn
                       : result->error_code ? result->error_code
                                            : "unknown outcome";
    asngn_sha256(body, strlen(body), digest);
    asngn_sha256_hex(digest, 32, hash);
    STRING("observation_sha256", hash);
    bad |=
        asmodel_json_object_set(v, "observation_bytes", asmodel_json_int((long long)strlen(body)));
  }
#undef BOOL
#undef STRING
  char *text = bad ? NULL : asmodel_json_write(v, 0);
  asmodel_json_free(v);
  if (text)
    asngn_tele_emit(t->s->ctx, "action", t->span_root, NULL, t->s->slug, t->led.turn, text);
  free(text);
}
