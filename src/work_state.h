/* Bounded acceptance state; callers of internal functions hold s->lock. */
#ifndef ASNGN_WORK_STATE_H
#define ASNGN_WORK_STATE_H
#include "asngn_internal.h"
#include "xcdn.h"
typedef struct asngn_work_store {
  asngn_work_state state;
  asngn_stream stream;
} asngn_work_store;
bool asngn_work_definition_valid(const asngn_work_definition *d);
bool asngn_work_criterion_equal(const asngn_work_criterion *a,
                                 const asngn_work_criterion *b);
xcdn_value_t *asngn_work_encode(const asngn_work_state *state);
asngn_err asngn_work_decode(const xcdn_value_t *value, asngn_work_state *state);
asngn_err asngn_work_load(asngn_session *s);
void asngn_work_free(asngn_work_store *w);
asngn_err asngn_work_save(asngn_session *s, asngn_work_state *next);
void asngn_work_refresh(asngn_work_state *w, const char *snapshot);
asngn_err asngn_work_revoke(asngn_session *s);
asngn_err asngn_work_observe(asngn_turn_state *t, const char *ref,
    const char *cmd, const char *args, bool tool_ok, const char *result,
    const char *before, const char *after);
asngn_err asngn_work_render(asngn_session *s, asngn_buf *out);
#endif
