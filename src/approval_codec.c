/* Versioned approval records bind reviewed arguments to a concrete tool and snapshot. */
#include "approval.h"
#include <stdlib.h>
#include <string.h>

static bool field(const xcdn_value_t *v, const char *key, char *out, size_t cap) {
  const char *s = asngn_xstr(asngn_xfield(v, key));
  if (!s || !*s || strlen(s) >= cap) return false;
  memcpy(out, s, strlen(s) + 1);
  return true;
}
static bool hash_valid(const char *s) {
  if (strlen(s) != 64) return false;
  for (size_t i = 0; i < 64; i++)
    if (!strchr("0123456789abcdef", s[i])) return false;
  return true;
}

xcdn_value_t *asngn_approval_encode(const asngn_approval *a) {
  xcdn_value_t *v = xcdn_value_object();
  bool ok = v && asngn_xobj_put(v, "schema", xcdn_value_int(1)) &&
            asngn_xobj_put(v, "sequence", xcdn_value_int((int64_t)a->sequence)) &&
            asngn_xobj_put(v, "profile", xcdn_value_int(a->profile)) &&
            asngn_xobj_put(v, "status", xcdn_value_int(a->status)) &&
            asngn_xobj_put(v, "session_wide", xcdn_value_bool(a->session_wide != 0));
#define STRING(name) ok = ok && asngn_xobj_put(v, #name, xcdn_value_string(a->name))
  STRING(id);
  STRING(turn_id);
  STRING(tool_ref);
  STRING(command);
  STRING(arguments_sha256);
  STRING(package_sha256);
  STRING(snapshot);
  STRING(workspace);
  STRING(arguments);
#undef STRING
  if (!ok) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

asngn_err asngn_approval_decode(const xcdn_value_t *v, asngn_approval **out) {
  asngn_approval *a = calloc(1, sizeof *a);
  int64_t schema, sequence, profile, status;
  bool wide;
  *out = NULL;
  if (!a) return ASNGN_ERR_NOMEM;
  /* Replay also checks canonical bytes: xCDN parsing can replace duplicate keys. */
  bool ok = v && v->type == XCDN_VAL_OBJECT && v->data.object.len == 14 &&
            asngn_xint(asngn_xfield(v, "schema"), &schema) && schema == 1 &&
            asngn_xint(asngn_xfield(v, "sequence"), &sequence) && sequence > 0 &&
            asngn_xint(asngn_xfield(v, "profile"), &profile) && profile >= ASNGN_SECURITY_CHAT &&
            profile <= ASNGN_SECURITY_AUTOMATION_CI &&
            asngn_xint(asngn_xfield(v, "status"), &status) && status >= 0 &&
            status <= ASNGN_APPROVAL_INTERRUPTED &&
            asngn_xbool(asngn_xfield(v, "session_wide"), &wide);
#define STRING(name) ok = ok && field(v, #name, a->name, sizeof a->name)
  STRING(id);
  STRING(turn_id);
  STRING(tool_ref);
  STRING(command);
  STRING(arguments_sha256);
  STRING(package_sha256);
  STRING(snapshot);
  STRING(workspace);
#undef STRING
  const char *args = asngn_xstr(asngn_xfield(v, "arguments"));
  ok = ok && asngn_uuid_valid(a->id) && asngn_uuid_valid(a->turn_id) &&
       hash_valid(a->arguments_sha256) && hash_valid(a->package_sha256) &&
       hash_valid(a->snapshot) && args && strlen(args) <= ASNGN_APPROVAL_ARGS_MAX;
  if (!ok) {
    free(a);
    return ASNGN_ERR_PARSE;
  }
  a->sequence = (unsigned long long)sequence;
  a->profile = (asngn_security_profile)profile;
  a->status = (asngn_approval_status)status;
  a->session_wide = wide;
  a->arguments = asngn_strdup(args);
  if (!a->arguments) {
    free(a);
    return ASNGN_ERR_NOMEM;
  }
  *out = a;
  return ASNGN_OK;
}

bool asngn_approval_follows(const asngn_approval *before, const asngn_approval *next) {
  if (!before)
    return next->sequence == 1 && next->status == ASNGN_APPROVAL_PENDING && !next->session_wide;
  if (next->sequence != before->sequence + 1) return false;
  if (strcmp(before->id, next->id))
    return before->status >= ASNGN_APPROVAL_DENIED && next->status == ASNGN_APPROVAL_PENDING &&
           !next->session_wide;
  if (strcmp(before->turn_id, next->turn_id) || strcmp(before->tool_ref, next->tool_ref) ||
      strcmp(before->command, next->command) ||
      strcmp(before->arguments_sha256, next->arguments_sha256) ||
      strcmp(before->package_sha256, next->package_sha256) ||
      strcmp(before->snapshot, next->snapshot) || strcmp(before->workspace, next->workspace) ||
      strcmp(before->arguments, next->arguments) || before->profile != next->profile)
    return false;
  if (before->status == ASNGN_APPROVAL_PENDING)
    return (next->status == ASNGN_APPROVAL_APPROVED || next->status == ASNGN_APPROVAL_DENIED ||
            next->status == ASNGN_APPROVAL_INTERRUPTED) &&
           (!next->session_wide || next->status == ASNGN_APPROVAL_APPROVED);
  if (before->status == ASNGN_APPROVAL_APPROVED)
    return next->session_wide == before->session_wide &&
           (next->status == ASNGN_APPROVAL_CONSUMED || next->status == ASNGN_APPROVAL_INVALIDATED ||
            next->status == ASNGN_APPROVAL_INTERRUPTED);
  return false;
}
