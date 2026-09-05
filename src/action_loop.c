/* Bounded action controller, recovery and executable completion gates. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

asngn_err asngn_actions_run(asngn_ctx *c, asngn_turn_state *t) {
  asngn_session *s = t->s;
  int plan_slot = asngn_models_slot_for_role(c, ASNGN_ROLE_PLANNER);
  /* Routine tool lookup can use the cheap planner.  Coding orchestration
   * and other complex work need the same capable tier that owns the final
   * implementation; waiting for malformed output before escalating wastes
   * tokens and lets a small model choose a low-quality workflow. */
  bool decide_on_generator =
      asngn_coding_task(t->prof.task) || t->prof.klass == ASNGN_CLASS_COMPLEX;
  asngn_err e = ASNGN_OK;

  if (t->phase != ASNGN_PHASE_ACTION)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step loop entered outside action phase");
  if (plan_slot < 0) plan_slot = t->gen_slot;

  while (!t->cancel) {
    char *instr = NULL, *gbnf = NULL, *line = NULL, *schema = NULL;
    asngn_prompt prompt;
    asngn_step st;
    int slot = decide_on_generator ? t->gen_slot : plan_slot;
    int tin = 0, tout = 0;
    int decision_cap = c->cfg.s_decide.max_tokens > 0 ? c->cfg.s_decide.max_tokens : 1024;
    bool call_muted, call_now, think_muted, think_now;

    if (t->steps >= c->cfg.max_steps || asngn_turn_expired(c, t) || t->osc_cycles > 2) {
      const char *why = t->osc_cycles > 2 ? "oscillation" : "step_budget";
      char data[64];
      snprintf(data, sizeof data, "{guard: \"%s\"}", why);
      asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn, data);
      os_rwlock_wrlock(&c->lock);
      c->stats.guard_trips++;
      os_rwlock_wrunlock(&c->lock);
      if (asngn_generation_needs_artifact(c, t))
        return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                            "action phase ended before an artifact was "
                            "written");
      asngn_work_push(c, t,
                      "[notice] step budget exhausted \xE2\x80\x94 answer "
                      "with what you have");
      t->forced_answer = true;
      return ASNGN_OK;
    }

    call_muted = t->call_mute;
    t->call_mute = false; /* one pass only */
    bool discover_now = c->astools_ok && !t->opts.no_tools;
    call_now = discover_now && astools_selection_count(t->tool_selection) > 0 && !call_muted;
    think_muted = t->think_mute;
    t->think_mute = false; /* one pass only */
    think_now = c->cfg.think_limit > 0 && !think_muted;
    e = asngn_step_instruction(c, t, call_now, call_muted, think_now, think_muted, &instr);
    if (e != ASNGN_OK) return e;
    e = asngn_grammar_steps(c, call_now, c->asper_ok, think_now, discover_now, s->blobs_n,
                            astools_selection_grammar(t->tool_selection), &gbnf);
    if (e != ASNGN_OK) {
      free(instr);
      return e;
    }

    memset(&prompt, 0, sizeof prompt);
    e = asngn_context_assemble(c, s, t, NULL, instr, slot, &prompt);
    if (e != ASNGN_OK) {
      free(instr);
      free(gbnf);
      return e;
    }
    asngn_ledger_zones(&t->led, &prompt);
    e = asngn_context_validate(c, slot, &prompt, decision_cap);
    if (e != ASNGN_OK) {
      asngn_prompt_free(&prompt);
      free(instr);
      free(gbnf);
      return e;
    }
    e = asngn_protocol_steps(astools_selection_schemas(t->tool_selection), call_now, c->asper_ok,
                             think_now, discover_now, s->blobs_n,
                             asngn_generation_needs_artifact(c, t), &schema);
    if (e != ASNGN_OK) {
      asngn_prompt_free(&prompt);
      free(instr);
      free(gbnf);
      return e;
    }
    e = asngn_generate_watched(c, t, slot, ASNGN_TASK_DECIDE, prompt.system_text, prompt.user_text,
                               gbnf, schema, decision_cap, NULL, NULL, &line, &tin, &tout);
    free(schema);
    asngn_prompt_free(&prompt);
    t->led.gt_decision += (size_t)(tout > 0 ? tout : 0);
    if (e != ASNGN_OK) {
      free(line);
      line = NULL;
      free(instr);
      free(gbnf);
      return e;
    }

    memset(&st, 0, sizeof st);
    /* The step grammar completes only through the trailing newline
     * (root ::= step "\n"), so a line without one is a generation the
     * decide token cap cut off mid-ramble, never a finished decision.
     * Parsing the fragment would ship truncated CLARIFY/THINK text as
     * if the model had meant it; treat it as a malformed pass instead. */
    {
      bool missing_newline = strchr(line, '\n') == NULL;
      asngn_err parse_err = missing_newline ? ASNGN_ERR_PROTOCOL : asngn_step_parse(c, line, &st);
      if (missing_newline || parse_err != ASNGN_OK) {
        char parse_detail[256];
        snprintf(parse_detail, sizeof parse_detail, "%s",
                 missing_newline ? "constrained output had no final newline" : asngn_last_error(c));
        free(line);
        free(instr);
        free(gbnf);
        return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "decision protocol failed: %s", parse_detail);
      }
    }
    free(instr);
    free(gbnf);
    if (c->asper_ok) {
      e = asngn_siblings_event_append(c, s->slug, ASNGN_MEM_DECISION, line, NULL, false, NULL);
      if (e != ASNGN_OK) {
        asngn_step_free(&st);
        free(line);
        return e;
      }
    }
    bool done = false;
    e = asngn_action_apply(c, t, &st, call_now, think_now, discover_now, &done);
    if (e == ASNGN_ERR_PROTOCOL && st.kind == ASNGN_STEP_CALL) {
      if (!decide_on_generator) {
        decide_on_generator = true;
        asngn_log(c, ASNGN_LOG_WARN, "loop",
                  "planner emitted an invalid tool protocol; escalating decisions: %s",
                  asngn_last_error(c));
      }
      asngn_work_push(c, t,
                      "[notice] the previous tool action was invalid; issue a corrected "
                      "workspace-relative call");
      t->futile_row++;
      e = ASNGN_OK;
    }
    if (e != ASNGN_OK || done) {
      asngn_step_free(&st);
      free(line);
      return e;
    }
    if (st.kind == ASNGN_STEP_CALL && t->repeat_calls >= 2 && !decide_on_generator) {
      decide_on_generator = true;
      t->call_mute = false;
      asngn_log(c, ASNGN_LOG_WARN, "loop",
                "identical call repeated; escalating decisions to the generator tier");
    }
    asngn_step_free(&st);
    free(line);
    /* Two consecutive blocked steps can end a completed lookup, but a tool's
     * transport-level `ok` is not proof that edited code works.  Keep the
     * action phase available while verification is pending or failed; the
     * ordinary step/token caps still bound an uncooperative model; an
     * explicitly configured deadline remains an additional opt-in guard. */
    if (t->futile_row >= 2 && t->tool_ok_seen && !asngn_coding_verification_unresolved(t)) {
      asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn,
                      "{guard: \"futile_steps\"}");
      os_rwlock_wrlock(&c->lock);
      c->stats.guard_trips++;
      os_rwlock_wrunlock(&c->lock);
      asngn_work_push(c, t,
                      "[notice] no further progress \xE2\x80\x94 "
                      "answering with what you have");
      t->forced_answer = true;
      return ASNGN_OK;
    }
  }
  return t->cancel ? ASNGN_ERR_CANCELLED : ASNGN_OK;
}
