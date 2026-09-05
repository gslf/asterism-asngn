/* Verification admission: fail closed on ambiguous tool results. */
#include "asngn_internal.h"
#include "xcdn.h"
#include <string.h>

/* Only closed project workflows can satisfy the coding verification gate.
 * Arbitrary process output and filenames are not verification contracts. */
bool asngn_verification_command(const char *ref, const char *cmd,
                                 const char *args) {
  (void)args;
  return cmd != NULL && ref != NULL &&
         (strcmp(ref, "project") == 0 || strncmp(ref, "project@", 8) == 0) &&
         (strcmp(cmd, "build") == 0 || strcmp(cmd, "test") == 0 ||
          strcmp(cmd, "lint") == 0 || strcmp(cmd, "diagnostics") == 0);
}

bool asngn_verification_result_ok(const char *text) {
  xcdn_error_t error;
  xcdn_document_t *doc;
  int64_t code, schema;
  bool ok = false;
  if (text == NULL) return false;
  doc = xcdn_parse(text, &error);
  if (doc == NULL) return false;
  if (doc->values_len == 1) {
    const xcdn_value_t *v = doc->values[0]->value;
    const char *status = asngn_xstr(asngn_xfield(v, "verification_status"));
    ok = asngn_xint(asngn_xfield(v, "verification_schema"), &schema) && schema == 1 &&
         status && !strcmp(status, "passed") &&
         asngn_xint(asngn_xfield(v, "exit_code"), &code) && code == 0;
  }
  xcdn_document_free(doc);
  return ok;
}

bool asngn_verification_current(asngn_turn_state *t) {
  if (!t->verification_ok) return false;
  if (asngn_workspace_refresh(t->s->ctx) != ASNGN_OK ||
      !t->verification_snapshot[0] ||
      strcmp(t->verification_snapshot, t->s->ctx->workspace.fingerprint)) {
    t->verification_ok = false;
    return false;
  }
  return true;
}
