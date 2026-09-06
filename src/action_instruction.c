/* Phase-scoped instructions for constrained decisions. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

/* ── decision passes and the step loop ───────────────────────────────── */

/* call_ok already folds in the one-pass mute; call_muted says the mute
 * is why CALL is missing, so the instruction can explain it. */
asngn_err asngn_step_instruction(asngn_ctx *c, asngn_turn_state *t, bool call_ok, bool call_muted,
                                 bool think_ok, bool think_muted, char **out) {
  asngn_buf b;
  asngn_err e;
  bool recall_ok = c->asper_ok;
  asngn_buf_init(&b);
  e = asngn_buf_printf(&b,
                       "Decide your next step. Emit exactly one action object on one line. "
                       "Hard completion budget: %d tokens; finish the object before that "
                       "limit. Remaining action steps: %d; remaining tool calls: %d. These "
                       "are ceilings, not targets: use only what advances the task.\n",
                       c->cfg.s_decide.max_tokens > 0 ? c->cfg.s_decide.max_tokens : 1024,
                       c->cfg.max_steps > t->steps ? c->cfg.max_steps - t->steps : 0,
                       c->cfg.max_tool_calls > t->tool_calls ? c->cfg.max_tool_calls - t->tool_calls
                                                             : 0);
  if (e == ASNGN_OK && c->astools_ok && !t->opts.no_tools)
    e = asngn_buf_appends(
        &b, "{action: \"discover\", why: \"<short reason>\", input: \"<tool name or purpose>\"} "
            "# replace the shortlist by searching all authorized tools. "
            "Use when a needed command is absent or its package changed.\n");
  if (e == ASNGN_OK && call_ok)
    e = asngn_buf_appends(&b, "{action: \"call\", why: \"<short reason>\", "
                              "input: <tool>.<command> {<args>}, success: "
                              "\"<what a good result shows>\", fallback: "
                              "\"<your plan if it fails>\"}  # run a tool\n");
  if (e == ASNGN_OK && call_ok)
    e = asngn_buf_appends(&b, "Tool paths are relative to the bound workspace. "
                              "Never invent /workspace or C:/workspace prefixes.\n");
  if (e == ASNGN_OK && t->security_profile == ASNGN_SECURITY_CODING_READONLY)
    e = asngn_buf_appends(&b,
                          "Security profile coding-readonly permits inspection but no mutation. "
                          "Use only commands marked read-only; if a write is required, explain "
                          "that the user can change the profile.\n");
  if (e == ASNGN_OK && t->usage_mode == ASNGN_USAGE_AUTOMATE)
    e = asngn_buf_appends(&b, "Automation mode favors completing the requested workflow and "
                              "validating its outcome before answering.\n");
  if (e == ASNGN_OK && recall_ok)
    e = asngn_buf_appends(&b, "{action: \"recall\", why: \"<short reason>\", "
                              "input: \"<question>\", success: \"<what memory "
                              "should return>\", fallback: \"<your plan if "
                              "nothing>\"}  # ask long-term memory\n");
  if (e == ASNGN_OK && t->s->blobs_n > 0)
    e = asngn_buf_printf(&b,
                         "{action: \"open\", why: \"<short reason>\", "
                         "input: {\"blob\": <1-%zu>, \"offset\": <byte-offset>}}  # read evidence "
                         "at an exact offset\n",
                         t->s->blobs_n);
  if (e == ASNGN_OK && think_ok)
    e = asngn_buf_appends(&b, "{action: \"think\", input: \"<one-line "
                              "note>\"}  # note to yourself\n");
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b, "{action: \"clarify\", why: \"<why you are "
                              "blocked>\", input: \"<question>\"}  # only if "
                              "blocked: ask the user and stop\n"
                              "{action: \"answer\"}  # write the final "
                              "answer now\n");
  if (e == ASNGN_OK && call_muted)
    e = asngn_buf_appends(&b, "The last call repeated one already run; its "
                              "outcome is shown above. Take a different "
                              "step.\n");
  if (e == ASNGN_OK && think_muted)
    e = asngn_buf_appends(&b, "The consecutive-thinking budget is complete. "
                              "Use the analysis already recorded: act, answer, "
                              "or clarify instead of adding another note.\n");
  /* the closing lines carry the most weight with greedy decoders: show
   * the worked call example only before any tool has run; once results
   * exist push toward answer — but only when something actually
   * succeeded, else push toward fixing the call instead. A generation
   * ask overrides all of that until something lands on disk: its
   * deliverable is source files in the workspace, and a planner left
   * with the generic example pastes the code into the answer instead. */
  if (e == ASNGN_OK && call_ok && t->prof.task == ASNGN_RTASK_GENERATE && !t->artifact_written &&
      asngn_tools_find(t, "fs", "write") >= 0)
    e = asngn_buf_appends(&b, "This is the private action phase. Treat the "
                              "request as professional software work: inspect "
                              "relevant existing context when present, choose a "
                              "coherent structure, and create every file needed "
                              "for the requested outcome. Do not stop after a "
                              "minimal demo or the first file. For fs.write, "
                              "never put "
                              "source code in this short action; set content to "
                              "the exact marker @asngn:draft. The engine will "
                              "generate that payload in a separate private "
                              "draft phase, compose the real call, and invoke it. "
                              "@asngn:draft-mode "
                              "Example first step:\n"
                              "{action: \"call\", why: \"create the requested "
                              "source file\", input: fs.write {path: "
                              "\"<file>\", content: \"@asngn:draft\"}, "
                              "success: \"a RESULT with bytes_written\", "
                              "fallback: \"report that the write failed\"}\n");
  else if (e == ASNGN_OK && call_ok && !t->tools_used)
    e = asngn_buf_appends(&b, "Choose a selected tool that obtains evidence needed by the task. "
                              "If none fits, discover a tool by name or purpose.\n");
  else if (e == ASNGN_OK && asngn_coding_task(t->prof.task) && t->artifact_written &&
           !t->verification_attempted && call_ok)
    e = asngn_buf_appends(&b, "A source mutation succeeded, but a write is not proof of a correct "
                              "program. Check that all requested artifacts are present and run the "
                              "most applicable build, compile, test, or smoke command now. If the "
                              "environment genuinely cannot verify it, answer only after stating "
                              "that limitation.\n");
  else if (e == ASNGN_OK && t->tools_used && t->tool_ok_seen)
    e = asngn_buf_appends(&b, "Tool results are above. If they already "
                              "answer the user, emit {action: \"answer\"} "
                              "now.\n");
  else if (e == ASNGN_OK && t->tools_used && call_ok)
    e = asngn_buf_appends(&b, "Every call so far failed. Re-read the user "
                              "message and issue one corrected call.\n");
  if (e != ASNGN_OK) {
    asngn_buf_free(&b);
    return e;
  }
  *out = asngn_buf_detach(&b);
  asngn_buf_free(&b);
  return *out != NULL ? ASNGN_OK : ASNGN_ERR_NOMEM;
}
