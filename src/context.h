/* Selection metadata contains hashes and costs, never source text. */
#ifndef ASNGN_CONTEXT_H
#define ASNGN_CONTEXT_H
#include "asngn_internal.h"
#include "asmodel_json.h"
#define ASNGN_CONTEXT_TRACE_ITEMS 128
asmodel_json_value *asngn_context_fingerprint(const char *text);
typedef struct {
  asmodel_json_value *root, *items;
  size_t seen;
} asngn_context_trace;
asngn_err asngn_context_trace_init(asngn_context_trace *trace, asngn_ctx *c, asngn_session *s,
                                   asngn_turn_state *t, int slot);
asngn_err asngn_context_trace_item(asngn_context_trace *trace, const char *zone, size_t index,
                                   const char *text, const char *decision, const char *reason);
asngn_err asngn_context_trace_finish(asngn_context_trace *trace, asngn_prompt *prompt);
void asngn_context_trace_free(asngn_context_trace *trace);
/* Best-effort metadata for input reaching model admission; no inference or
 * tokenization is performed. NULL means unavailable, never an execution error. */
char *asngn_request_trace(asngn_ctx *c, const asngn_turn_state *turn, int slot,
                           asngn_task_kind task, const asmodel_input *input,
                           const char *grammar, const asmodel_generate_params *params,
                           const asngn_context_diagnostics *budget, asngn_err admission);
void asngn_request_result(asngn_ctx *c, const asngn_turn_state *turn, const char *request,
                           int slot, asngn_task_kind task, const asmodel_generation_info *info,
                           asngn_err result, int64_t ms, bool attempted);
size_t asngn_context_tokens(asngn_ctx *c, int slot, const char *text);
asngn_err asngn_context_verbatim(asngn_ctx *c, asngn_session *s, const asngn_turn_state *t,
                                 int slot, char **text, size_t *tokens, asngn_context_trace *trace);
#endif
