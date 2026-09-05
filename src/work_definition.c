/* Acceptance definitions are host input, bounded before copying or persisting. */
#include "work_state.h"
#include <string.h>

static bool text(const char *s, size_t cap, bool required) {
  const char *end = memchr(s, 0, cap);
  return end && (!required || end != s) && asngn_utf8_valid(s, (size_t)(end-s));
}
static bool member(const char *s, const char *const *values, size_t n) {
  for (size_t i = 0; i < n; i++) if (!strcmp(s, values[i])) return true;
  return false;
}
static bool path_valid(const char *s) {
  if (!strcmp(s, ".")) return true;
  if (os_path_is_abs(s) || strchr(s, '\\') || strchr(s, ':')) return false;
  for (const char *p = s; ; ) {
    size_t n = strcspn(p, "/");
    if (!n || (n == 1 && p[0] == '.') || (n == 2 && !memcmp(p,"..",2))) return false;
    p += n;
    if (!*p) return true;
    p++;
  }
}
bool asngn_work_definition_valid(const asngn_work_definition *d) {
  static const char *const commands[] = {"build", "test", "lint", "diagnostics"};
  static const char *const adapters[] = {"auto", "cmake", "cargo", "npm", "python"};
  if (!d || !d->count || d->count > ASNGN_WORK_CRITERIA_MAX ||
      !text(d->goal,sizeof d->goal,true) ||
      !text(d->constraints,sizeof d->constraints,false)) return false;
  for (size_t i = 0; i < d->count; i++) {
    const asngn_work_criterion *c = &d->criteria[i];
    if (!text(c->id,sizeof c->id,true) || !asngn_slug_valid(c->id) ||
        !text(c->requirement,sizeof c->requirement,true) ||
        !text(c->command,sizeof c->command,true) || !member(c->command,commands,4) ||
        !text(c->adapter,sizeof c->adapter,true) || !member(c->adapter,adapters,5) ||
        !text(c->path,sizeof c->path,true) || !path_valid(c->path) ||
        c->depends_on >= (1u << i)) return false;
    for (size_t j = 0; j < i; j++)
      if (!strcmp(c->id,d->criteria[j].id)) return false;
  }
  return true;
}
bool asngn_work_criterion_equal(const asngn_work_criterion *a,
                                 const asngn_work_criterion *b) {
  return !strcmp(a->id,b->id) && !strcmp(a->requirement,b->requirement) &&
         !strcmp(a->command,b->command) && !strcmp(a->adapter,b->adapter) &&
         !strcmp(a->path,b->path) && a->depends_on == b->depends_on;
}
const char *asngn_proof_status_name(asngn_proof_status status) {
  static const char *const names[] = {"not_run", "passed", "failed", "inconclusive",
      "infrastructure_error", "stale", "blocked"};
  return (unsigned)status < sizeof names / sizeof names[0] ? names[status] : "unknown";
}
void asngn_work_refresh(asngn_work_state *w, const char *snapshot) {
  uint32_t passed = 0;
  w->succeeded = w->definition.count != 0;
  for (size_t i = 0; i < w->definition.count; i++) {
    asngn_work_proof *p = &w->proofs[i];
    if (p->status != ASNGN_PROOF_NOT_RUN && p->status != ASNGN_PROOF_STALE &&
        (!snapshot || !*snapshot || strcmp(p->snapshot,snapshot)))
      p->status = ASNGN_PROOF_STALE;
    if (p->status == ASNGN_PROOF_PASSED &&
        (w->definition.criteria[i].depends_on & passed) != w->definition.criteria[i].depends_on)
      p->status = ASNGN_PROOF_BLOCKED;
    if (p->status == ASNGN_PROOF_PASSED) passed |= 1u << i;
    else w->succeeded = 0;
  }
}
