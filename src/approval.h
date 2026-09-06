/* Checked approval transitions. Callers of store functions hold s->lock. */
#ifndef ASNGN_APPROVAL_H
#define ASNGN_APPROVAL_H
#include "execution.h"
#include "xcdn.h"
#define ASNGN_APPROVAL_ARGS_MAX (256 * 1024)
/* Includes worst-case escaping of all bounded review fields. */
#define ASNGN_APPROVAL_FRAME_MAX (2u * 1024u * 1024u)
typedef struct asngn_approval_store {
  asngn_approval *current;
  asngn_stream stream;
} asngn_approval_store;
asngn_err asngn_approval_load(asngn_session *s);
void asngn_approval_store_free(asngn_approval_store *store);
asngn_err asngn_approval_save(asngn_session *s, asngn_approval *next);
asngn_err asngn_approval_transition(asngn_session *s, asngn_approval_status status,
                                    int session_wide);
xcdn_value_t *asngn_approval_encode(const asngn_approval *a);
asngn_err asngn_approval_decode(const xcdn_value_t *value, asngn_approval **out);
bool asngn_approval_follows(const asngn_approval *before, const asngn_approval *next);
asngn_err asngn_approval_prepare(asngn_turn_state *t, const astools_selected_command *tool,
                                 const char *args, char id[37], char snapshot[65]);
asngn_err asngn_approval_snapshot(asngn_session *s, char out[65]);
void asngn_approval_grant_key(asngn_turn_state *t, const astools_selected_command *tool,
                              char out[65]);
#endif
