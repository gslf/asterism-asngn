/*
 * test_engine_guards.c — the loop/resource guards and the input
 * gate, end to end over the same sibling fixture as test_engine_turns
 * (engine_fx.h): scripted fake models + the real astools registry running
 * the scripted asngn_fake_tool binary.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "engine_fx.h"

/* ── a. THINK limit preserves analysis and steers the next pass ───────── */

TEST(think_limit) {
  eng_fx f;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"think\", input: \"inspect the "
                              "available evidence\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"think\", input: \"choose the "
                              "next useful action\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Final answer.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool calmly", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Final answer.\n");

  /* Both notes remain in context, then THINK is removed for exactly the next
   * constrained pass.  The guard steers action; it does not discard analysis
   * or force the response phase. */
  ASSERT_CONTAINS(f.light.last_user, "THINK: inspect the available evidence");
  ASSERT_CONTAINS(f.light.last_user, "THINK: choose the next useful action");
  ASSERT_CONTAINS(f.light.last_user, "[notice] thinking budget complete");
  ASSERT_NOT_CONTAINS(f.light.last_grammar, "think     ::=");
  ASSERT_CONTAINS(f.light.last_user, "Use the analysis already recorded");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* A compile/test retry is a new observation after a source mutation, even
 * when its tool, command, and canonical arguments are byte-identical. */
TEST(identical_call_after_workspace_change) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo",
                        "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN "
                              "| TASK EDIT\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"compile the server\", input: fake.run "
      "{msg: \"gcc server.c -o server\"}, success: \"exit zero\", "
      "fallback: \"create the missing source\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"create the missing source\", input: "
      "fake.mut {msg: \"write server.c\"}, success: \"source exists\", "
      "fallback: \"report the write failure\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"compile the rewritten server\", input: "
      "fake.run {msg: \"gcc server.c -o server\"}, success: \"exit zero\", "
      "fallback: \"report compiler diagnostics\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "The rewritten server compiles.\n"));

  ASSERT_OK(eng_turn(&f, "fix the server and compile it", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "The rewritten server compiles.\n");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 3);
  ASSERT_NOT_CONTAINS(f.stdm.last_user, "asngn/repeat");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── b. identical CALL blocked with asngn/repeat ──────────────────────── */

TEST(identical_call) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"using the tool\", "
                              "input: fake.run {msg: \"a\"}, success: "
                              "\"RESULT with a\", fallback: \"answer\"}"
                              "\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"using the tool\", "
                              "input: fake.run {msg: \"a\"}, success: "
                              "\"RESULT with a\", fallback: \"answer\"}"
                              "\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Already done.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool twice", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Already done.\n");

  /* the second, identical call was blocked without dispatch */
  ASSERT_CONTAINS(f.light.last_user, "asngn/repeat");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 1);

  /* the pass after the blocked repeat had CALL muted: no call action
   * template in the instruction, and the instruction explains why */
  ASSERT_NOT_CONTAINS(f.light.last_user, "<tool>.<command>");
  ASSERT_CONTAINS(f.light.last_user, "Take a different step");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── b2. futile steps after a good result force the answer ────────────── */

TEST(futile_steps) {
  /* One successful CALL, then the model spins: an identical RECALL
   * repeated twice is two consecutive guard-blocked steps with a good
   * result in hand — the loop forces the answer pass instead of burning
   * the step budget (asper is disabled in the fixture, so the fresh
   * RECALL yields nothing but still counts as progress). */
  eng_fx f;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"using the tool\", "
                              "input: fake.run {msg: \"a\"}, success: "
                              "\"RESULT with a\", fallback: \"answer\"}"
                              "\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"recall\", why: \"context on "
                              "a\", input: \"what do I know of a?\", success: "
                              "\"notes on a\", fallback: \"proceed\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"recall\", why: \"context on "
                              "a\", input: \"what do I know of a?\", success: "
                              "\"notes on a\", fallback: \"proceed\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"recall\", why: \"context on "
                              "a\", input: \"what do I know of a?\", success: "
                              "\"notes on a\", fallback: \"proceed\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"think\", input: \"never "
                              "reached\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Answer from the results.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool and answer", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Answer from the results.\n");

  /* CALL + RECALL + 2 blocked RECALLs = 4 steps; the 5th decision never
   * ran (the futile guard ended the loop first) */
  ASSERT_EQ_INT(f.s->log[1].steps, 4);
  ASSERT_EQ_INT(f.light.calls, 4);
  ASSERT_CONTAINS(f.stdm.last_user, "no further progress");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(futile_steps_do_not_skip_code_verification) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo",
                        "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN "
                              "| TASK EDIT\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"rewrite source\", input: fake.mut "
      "{msg: \"rewrite server.c\"}, success: \"source changed\", "
      "fallback: \"report failure\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"recall\", why: \"look for build notes\", input: "
      "\"server build notes\", success: \"build command\", fallback: "
      "\"use gcc\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"recall\", why: \"look for build notes\", input: "
      "\"server build notes\", success: \"build command\", fallback: "
      "\"use gcc\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"recall\", why: \"look for build notes\", input: "
      "\"server build notes\", success: \"build command\", fallback: "
      "\"use gcc\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"verify the rewrite\", input: fake.run "
      "{msg: \"gcc server.c -o server\"}, success: \"exit zero\", "
      "fallback: \"report diagnostics\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "The rewrite was verified.\n"));

  ASSERT_OK(eng_turn(&f, "rewrite and verify server.c", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "The rewrite was verified.\n");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 2);
  ASSERT_CONTAINS(f.stdm.last_user, "gcc server.c -o server");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── c. step budget forces the answer pass ────────────────────────────── */

TEST(step_budget) {
  eng_fx f;
  asngn_turn_result r;
  int i, seen;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", "safety: { max_steps: 3 },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  for (i = 0; i < 5; i++)
    ASSERT_TRUE(fake_model_push(&f.light,
                                "{action: \"think\", input: \"x\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Here is what I know.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool without stopping", NULL, NULL,
                     &r));

  /* the user still gets an answer built from what was gathered */
  ASSERT_EQ_STR(r.answer, "Here is what I know.\n");

  /* the exhaustion notice reached a model-visible zone: the forced
   * answer pass (std) always sees it; a late decision pass may too */
  seen = (f.stdm.last_user != NULL &&
          strstr(f.stdm.last_user, "[notice] step budget exhausted") !=
              NULL) ||
         (f.light.last_user != NULL &&
          strstr(f.light.last_user, "[notice] step budget exhausted") !=
              NULL);
  ASSERT_TRUE(seen);

  /* the turn still ledgers as a plan-mode route */
  ASSERT_EQ_INT((long long)f.s->led_n, 1);
  ASSERT_EQ_STR(f.s->led[0].mode, "plan");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── d. tool-call cap blocks the second dispatch ──────────────────────── */

TEST(tool_cap) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", "safety: { max_tool_calls: 1 },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"first call\", "
                              "input: fake.run {msg: \"a\"}, success: "
                              "\"RESULT with a\", fallback: \"answer\"}"
                              "\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"second "
                              "call\", input: fake.run {msg: \"b\"}, "
                              "success: \"RESULT with b\", fallback: "
                              "\"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Budget exhausted.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool twice", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Budget exhausted.\n");

  /* the second (different) call hit the cap instead of dispatching */
  ASSERT_CONTAINS(f.light.last_user, "asngn/tool-cap");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 1);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── e. input gate: empty, oversized, control characters ─────────────── */

TEST(input_gate) {
  eng_fx f;
  asngn_turn_result r;
  asngn_task *t = NULL;
  char *big;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));

  /* empty message: rejected before any task exists */
  ASSERT_ERR(asngn_submit(f.s, "", NULL, NULL, NULL, &t),
             ASNGN_ERR_INVALID);
  ASSERT_TRUE(t == NULL);

  /* 70000 bytes: over the 64 KiB cap */
  big = (char *)malloc(70001);
  ASSERT_TRUE(big != NULL);
  memset(big, 'a', 70000);
  big[70000] = '\0';
  ASSERT_ERR(asngn_submit(f.s, big, NULL, NULL, NULL, &t),
             ASNGN_ERR_INVALID);
  free(big);
  ASSERT_TRUE(t == NULL);

  /* control bytes pass the gate but are stripped from the message */
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "ok.\n"));
  ASSERT_OK(eng_turn(&f, "ok\x01" "ok", NULL, NULL, &r));
  ASSERT_EQ_INT((long long)f.s->log_n, 2);
  ASSERT_EQ_STR(f.s->log[0].text, "okok");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── f. cancel: liveness only ─────────────────────────────────────────── */

/* Cancellation races the turn by design: the fake models reply instantly
 * and pop a default "ANSWER\n" once their queue is empty, so the turn may
 * legitimately COMPLETE before the cancel flag is observed, or abort at
 * any of the loop's cancellation points. The contract under test is
 * liveness and memory safety — the task finishes promptly with either
 * verdict, and teardown (close over a cancelled/finished turn) does not
 * hang or crash — not which of the two races wins. */
TEST(cancel_turn) {
  eng_fx f;
  asngn_turn_result r;
  asngn_task *t = NULL;
  asngn_err e;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"think\", input: \"pause\"}\n"));

  ASSERT_OK(asngn_submit(f.s, "use the fake tool with a pause", NULL, NULL,
                         NULL, &t));
  ASSERT_OK(asngn_task_cancel(t));
  e = asngn_task_wait(t, 60000, &r);
  ASSERT_TRUE(e == ASNGN_OK || e == ASNGN_ERR_CANCELLED);
  asngn_task_free(t);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(cancel_reaches_inflight_provider) {
  eng_fx f;
  asngn_task task;
  asngn_turn_state turn;

  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  memset(&task, 0, sizeof task);
  memset(&turn, 0, sizeof turn);
  task.ctx = f.c;
  task.turn = &turn;

  os_mutex_lock(&f.c->q_mu);
  f.c->call_active = 1;
  f.c->call_turn = &turn;
  f.c->call_cancel = 0;
  os_mutex_unlock(&f.c->q_mu);

  ASSERT_OK(asngn_task_cancel(&task));
  ASSERT_EQ_INT(turn.cancel, 1);
  ASSERT_EQ_INT(f.c->call_cancel, 1);

  os_mutex_lock(&f.c->q_mu);
  f.c->call_active = 0;
  f.c->call_turn = NULL;
  os_mutex_unlock(&f.c->q_mu);
  eng_drop(&f);
}

/* ── runner ───────────────────────────────────────────────────────────── */

TEST_LIST = {
  TEST_ENTRY(think_limit),
  TEST_ENTRY(identical_call),
  TEST_ENTRY(identical_call_after_workspace_change),
  TEST_ENTRY(futile_steps),
  TEST_ENTRY(futile_steps_do_not_skip_code_verification),
  TEST_ENTRY(step_budget),
  TEST_ENTRY(tool_cap),
  TEST_ENTRY(input_gate),
  TEST_ENTRY(cancel_turn),
  TEST_ENTRY(cancel_reaches_inflight_provider),
};

RUN_ALL_TESTS()
