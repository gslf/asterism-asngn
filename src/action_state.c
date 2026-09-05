/* Artifact and completion gates derived from runtime observations. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

bool asngn_turn_expired(asngn_ctx *c, asngn_turn_state *t) {
  return t->deadline_mono > 0 && asngn_clock_mono_ms(&c->clock) >= t->deadline_mono;
}

/* ── phase-separated artifact drafting ───────────────────────────────── */

bool asngn_tool_ref_is(const char *ref, const char *want) {
  size_t n;
  if (ref == NULL || want == NULL) return false;
  n = strlen(want);
  return strncmp(ref, want, n) == 0 && (ref[n] == '\0' || ref[n] == '@');
}

bool asngn_artifact_command(const char *ref, const char *cmd) {
  if (asngn_tool_ref_is(ref, "fs") && strcmp(cmd, "write") == 0) return true;
  if (asngn_tool_ref_is(ref, "fs")) return false; /* mkdir/move is not content */
  if (asngn_tool_ref_is(ref, "edit") &&
      (strcmp(cmd, "replace") == 0 || strcmp(cmd, "insert") == 0 || strcmp(cmd, "patch") == 0))
    return true;
  if (asngn_tool_ref_is(ref, "edit")) return false;
  /* Extensible mutating tools (proc generators, custom scaffolds, test
   * fixtures) may create artifacts; the caller already proved success and
   * !read_only before consulting this helper. */
  return true;
}

bool asngn_generation_needs_artifact(asngn_ctx *c, const asngn_turn_state *t) {
  return t->prof.task == ASNGN_RTASK_GENERATE && c->astools_ok && !t->opts.no_tools &&
         !t->artifact_written && !t->authorization_blocked &&
         t->security_profile != ASNGN_SECURITY_CODING_READONLY;
}

bool asngn_coding_task(asngn_route_task task) {
  return task == ASNGN_RTASK_GENERATE || task == ASNGN_RTASK_EDIT || task == ASNGN_RTASK_REFACTOR ||
         task == ASNGN_RTASK_DEBUG || task == ASNGN_RTASK_BUILD;
}

bool asngn_coding_verification_unresolved(asngn_turn_state *t) {
  return asngn_coding_task(t->prof.task) && t->artifact_written &&
         (!t->verification_attempted || !asngn_verification_current(t));
}
