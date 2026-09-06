/* Shared execution boundaries. Both decision protocols use these same gates. */
#ifndef ASNGN_EXECUTION_H
#define ASNGN_EXECUTION_H
#include "asngn_internal.h"
#include "astools.h"
#include "work_state.h"

void asngn_ledger_zones(asngn_ledger_entry *led, const asngn_prompt *p);
int64_t asngn_daily_spend(asngn_ctx *c);
void asngn_output_stream(const char *piece, void *ud);
asngn_err asngn_work_data(asngn_ctx *c, asngn_turn_state *t, const char *payload);
char *asngn_context_text(asngn_session *s, const char *text);
asngn_err asngn_generate_watched(asngn_ctx *c, asngn_turn_state *t, int slot, asngn_task_kind task,
                                 const char *sys, const char *usr, const char *gbnf,
                                 const char *schema, int max_tokens, asngn_token_fn cb, void *cb_ud,
                                 char **out_text, int *out_in, int *out_out);
bool asngn_turn_expired(asngn_ctx *c, asngn_turn_state *t);
bool asngn_tool_ref_is(const char *ref, const char *want);
bool asngn_artifact_command(const char *ref, const char *cmd);
bool asngn_generation_needs_artifact(asngn_ctx *c, const asngn_turn_state *t);
bool asngn_coding_task(asngn_route_task task);
bool asngn_coding_verification_unresolved(asngn_turn_state *t);
void asngn_call_state_key(const uint8_t intent[32], const uint8_t workspace[32],
                          uint64_t world_epoch, uint8_t out[32]);
asngn_err asngn_expand_write_draft(asngn_ctx *c, asngn_turn_state *t, const char *ref,
                                   const char *cmd, const char *args, char **out);
char *asngn_call_args(const char *args);
asngn_err asngn_call_outcome(asngn_ctx *c, asngn_turn_state *t, const char *ref, const char *cmd,
                             const char *args, const char *line);
asngn_err asngn_call_error(asngn_ctx *c, asngn_turn_state *t, const char *ref, const char *cmd,
                           const char *args, const char *code, const char *message);
void asngn_call_fallback(asngn_ctx *c, asngn_turn_state *t, const char *fallback);
void asngn_action_event(asngn_turn_state *t, const char *ref, const char *command,
                        const char *args, const astools_result *result,
                        astools_err error, bool journaled);
asngn_err asngn_call_confirm(asngn_ctx *c, asngn_turn_state *t,
    const astools_selected_command *tool, const char *args, const asngn_tool_note *note,
    bool *allowed, const char **deny_code);
asngn_err asngn_call_execute(asngn_ctx *c, asngn_turn_state *t, const char *line,
                             const char *fallback);
asngn_err asngn_step_instruction(asngn_ctx *c, asngn_turn_state *t, bool call_ok, bool call_muted,
                                 bool think_ok, bool think_muted, char **out);
asngn_err asngn_actions_run(asngn_ctx *c, asngn_turn_state *t);
asngn_err asngn_native_run(asngn_ctx *c, asngn_turn_state *t);
bool asngn_response_has_tool_protocol(asngn_ctx *c, const char *text);
asngn_err asngn_answer_run(asngn_ctx *c, asngn_turn_state *t, size_t *aux_tokens);
bool asngn_native_answer_eligible(asngn_ctx *c, asngn_turn_state *t);
asngn_err asngn_generate_input(asngn_ctx *c, asngn_turn_state *t, int slot, asngn_task_kind task,
                               const asmodel_input *input, const char *gbnf, const char *schema,
                               const asmodel_tools *tools, int max_tokens, asngn_token_fn cb,
                               void *cb_ud, char **out_text, int *out_in, int *out_out);
asngn_err asngn_action_apply(asngn_ctx *c, asngn_turn_state *t, asngn_step *st, bool call_now,
                             bool think_now, bool discover_now, bool *done);
#endif
