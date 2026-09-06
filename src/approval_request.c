/* Construct immutable review data before publishing a confirmation event. */
#include "approval.h"
#include <stdlib.h>
#include <string.h>

asngn_err asngn_approval_snapshot(asngn_session *s, char out[65]) {
  asngn_workspace_info workspace = s->workspace;
  asngn_err e = asngn_workspace_snapshot(&workspace, NULL);
  if (e == ASNGN_OK) memcpy(out, workspace.fingerprint, 65);
  return e;
}

void asngn_approval_grant_key(asngn_turn_state *t, const astools_selected_command *tool,
                              char out[65]) {
  asngn_sha256_ctx h;
  uint8_t hash[32];
  char profile[24];
  snprintf(profile, sizeof profile, "%d", (int)t->security_profile);
  const char *fields[] = {
      "asngn-tool-grant-v1",          tool->ref, tool->command, tool->content_sha256,
      t->s->workspace.canonical_root, profile};
  asngn_sha256_init(&h);
  for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
    asngn_sha256_update(&h, fields[i], strlen(fields[i]) + 1);
  asngn_sha256_final(&h, hash);
  asngn_sha256_hex(hash, 32, out);
}

asngn_err asngn_approval_prepare(asngn_turn_state *t, const astools_selected_command *tool,
                                 const char *args, char id[37], char snapshot[65]) {
  if (!args || strlen(args) > ASNGN_APPROVAL_ARGS_MAX) return ASNGN_ERR_LIMIT;
  asngn_approval *a = calloc(1, sizeof *a);
  if (!a) return ASNGN_ERR_NOMEM;
  asngn_uuid_v4(a->id);
  memcpy(a->turn_id, t->span_root, 37);
  snprintf(a->tool_ref, sizeof a->tool_ref, "%s", tool->ref);
  snprintf(a->command, sizeof a->command, "%s", tool->command);
  snprintf(a->package_sha256, sizeof a->package_sha256, "%s", tool->content_sha256);
  snprintf(a->workspace, sizeof a->workspace, "%s", t->s->workspace.canonical_root);
  a->profile = t->security_profile;
  uint8_t hash[32];
  asngn_sha256(args, strlen(args), hash);
  asngn_sha256_hex(hash, 32, a->arguments_sha256);
  size_t masked_bytes = 0;
  asngn_err e = asngn_redact(args, strlen(args), &a->arguments, &masked_bytes);
  if (e == ASNGN_OK && !a->arguments) a->arguments = asngn_strdup(args);
  if (e == ASNGN_OK && !a->arguments) e = ASNGN_ERR_NOMEM;
  if (e == ASNGN_OK && strlen(a->arguments) > ASNGN_APPROVAL_ARGS_MAX) e = ASNGN_ERR_LIMIT;
  if (e == ASNGN_OK) e = asngn_approval_snapshot(t->s, a->snapshot);
  if (e == ASNGN_OK) {
    memcpy(id, a->id, 37);
    memcpy(snapshot, a->snapshot, 65);
    os_rwlock_wrlock(&t->s->lock);
    e = asngn_approval_save(t->s, a);
    os_rwlock_wrunlock(&t->s->lock);
  }
  if (e != ASNGN_OK) asngn_approval_free(a);
  return e;
}
