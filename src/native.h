/* Private native-action adapter. Execution authority stays in action_apply. */
#ifndef ASNGN_NATIVE_H
#define ASNGN_NATIVE_H
#include "execution.h"

#define ASNGN_NATIVE_HISTORY 64
#define ASNGN_NATIVE_BYTES (1024 * 1024)
typedef struct {
  asmodel_tool_schema schemas[64];
  const astools_selected_command *commands[64]; /* borrowed from this selection */
  asngn_step_kind kinds[64];
  char names[59][63], descriptions[59][1200];
  size_t count;
} asngn_native_contract;
typedef struct {
  asmodel_tool_call call;
  char *result;
} asngn_native_entry;
typedef struct {
  asngn_native_entry entries[ASNGN_NATIVE_HISTORY];
  size_t count, bytes;
} asngn_native_history;

asngn_err asngn_native_contract_build(asngn_ctx *c, asngn_turn_state *t, bool call_now,
                                      asngn_native_contract *out);
asngn_err asngn_native_steps(asngn_ctx *c, asngn_turn_state *t,
                             const asngn_native_contract *contract, const asmodel_tool_calls *calls,
                             asngn_step steps[32]);
void asngn_native_history_free(asngn_native_history *h);
/* Copies a redacted, bounded observation of this action's working items. */
asngn_err asngn_native_record(asngn_turn_state *t, asngn_native_history *h,
                              const asmodel_tool_call *call, size_t work_before);
asngn_err asngn_native_input(asngn_ctx *c, asngn_turn_state *t, const asngn_prompt *seed,
                             const asngn_native_history *h, asngn_buf *status,
                             asmodel_message messages[131], asmodel_block blocks[131],
                             asmodel_input *out);
#endif
