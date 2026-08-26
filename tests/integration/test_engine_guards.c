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

/* ── a. THINK limit injects the "enough thinking" notice ──────────────── */

TEST(think_limit) {
  eng_fx f;
  asngn_turn_result r;
  int i;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  for (i = 0; i < 6; i++)
    ASSERT_TRUE(fake_model_push(&f.light,
                                "THINK | penso ancora un momento\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Risposta finale.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake con calma", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Risposta finale.\n");

  /* think_limit (2 in a row / 4 per turn) tripped and the notice landed
   * in the working zone the later decision passes saw */
  ASSERT_CONTAINS(f.light.last_user, "[notice] enough thinking");

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
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.run {msg: \"a\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.run {msg: \"a\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Gia fatto.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake due volte", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Gia fatto.\n");

  /* the second, identical call was blocked without dispatch */
  ASSERT_CONTAINS(f.light.last_user, "asngn/repeat");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 1);

  /* the pass after the blocked repeat had CALL muted: no CALL protocol
   * line, and the instruction explains why */
  ASSERT_NOT_CONTAINS(f.light.last_user, "CALL <tool>.<command>");
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
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.run {msg: \"a\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "RECALL | cosa so di a?\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "RECALL | cosa so di a?\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "RECALL | cosa so di a?\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "THINK | mai raggiunto\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Risposta dai risultati.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake e rispondi", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Risposta dai risultati.\n");

  /* CALL + RECALL + 2 blocked RECALLs = 4 steps; the 5th decision never
   * ran (the futile guard ended the loop first) */
  ASSERT_EQ_INT(f.s->log[1].steps, 4);
  ASSERT_EQ_INT(f.light.calls, 4);
  ASSERT_CONTAINS(f.stdm.last_user, "no further progress");

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
    ASSERT_TRUE(fake_model_push(&f.light, "THINK | x\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Ecco cosa so.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake senza fermarti", NULL, NULL,
                     &r));

  /* the user still gets an answer built from what was gathered */
  ASSERT_EQ_STR(r.answer, "Ecco cosa so.\n");

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
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.run {msg: \"a\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.run {msg: \"b\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Budget finito.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake due volte", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Budget finito.\n");

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
  ASSERT_TRUE(fake_model_push(&f.light, "THINK | pausa\n"));

  ASSERT_OK(asngn_submit(f.s, "usa il tool fake con pausa", NULL, NULL,
                         NULL, &t));
  ASSERT_OK(asngn_task_cancel(t));
  e = asngn_task_wait(t, 60000, &r);
  ASSERT_TRUE(e == ASNGN_OK || e == ASNGN_ERR_CANCELLED);
  asngn_task_free(t);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── runner ───────────────────────────────────────────────────────────── */

TEST_LIST = {
  TEST_ENTRY(think_limit),
  TEST_ENTRY(identical_call),
  TEST_ENTRY(futile_steps),
  TEST_ENTRY(step_budget),
  TEST_ENTRY(tool_cap),
  TEST_ENTRY(input_gate),
  TEST_ENTRY(cancel_turn),
};

RUN_ALL_TESTS()
