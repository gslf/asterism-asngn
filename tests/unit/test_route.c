/*
 * test_route.c — the evidence-scored classification table and the
 * tier ladder over the default pool (nano, light, std, embed).
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

/* Bare context with default configuration: the tier ladder reads
 * c->cfg.pool / c->cfg.pool_n (verified against src/route.c), so the
 * model slots need not be populated. */
static asngn_ctx *ctx_defaults(void) {
  asngn_ctx *c = (asngn_ctx *)calloc(1, sizeof(asngn_ctx));
  if (c == NULL) return NULL;
  os_mutex_init(&c->err_mu);
  asngn_config_defaults(&c->cfg);
  return c;
}

static void ctx_free(asngn_ctx *c) {
  if (c == NULL) return;
  asngn_config_free(&c->cfg);
  os_mutex_destroy(&c->err_mu);
  free(c);
}

static asngn_route_evidence ev_tools(void) {
  asngn_route_evidence ev;
  memset(&ev, 0, sizeof ev);
  ev.tools_available = true;
  return ev;
}

/* ── message-only classification ─────────────────────────────────────── */

TEST(heuristic_short_greeting) {
  asngn_route_profile p;
  asngn_route_heuristic("hi", NULL, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_CHAT);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_TERSE);  /* len < 80 */
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_DIRECT);     /* no tools */
}

/* The motivating case: a hard problem stated in one short line. Byte
 * length alone called this SIMPLE; the task kind (generate) plus the
 * systems language now call it COMPLEX with no session evidence at all. */
TEST(heuristic_short_but_hard_generates_complex) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic(
      "write a C function that reverses a string in place, with tests",
      &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_GENERATE);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH); /* code output is bulky */
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN);
}

TEST(heuristic_debug_task_moderate_plan) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic("fix the bug in src/main.c", &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_DEBUG);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN); /* imperative verb + file path */
  ASSERT_TRUE((p.toolmask & ASNGN_TOOLF_EDIT) != 0);
  ASSERT_TRUE((p.toolmask & ASNGN_TOOLF_FS) != 0);
}

/* Length is one weak signal now, not a verdict: a long prose message
 * with no task evidence is MODERATE, no longer COMPLEX. */
TEST(heuristic_long_prose_moderate) {
  char msg[951];
  asngn_route_profile p;
  memset(msg, 'a', sizeof msg - 1);
  msg[sizeof msg - 1] = '\0';
  asngn_route_heuristic(msg, NULL, &p); /* len 950 > 900 */
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH); /* len > 600 */
}

TEST(heuristic_fence_moderate_rich) {
  asngn_route_profile p;
  asngn_route_heuristic("look:\n```\ncode\n```", NULL, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH);
}

TEST(heuristic_depth_ask_rich) {
  asngn_route_profile p;
  asngn_route_heuristic("explain X in detail", NULL, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_EXPLAIN);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH); /* depth ask beats len<80 */
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);
}

TEST(heuristic_question_only_direct) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic("what time is it?", &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_LOOKUP);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_DIRECT); /* tools, but nothing to plan */
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);
}

TEST(heuristic_conceptual_repo_question_direct) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic("what is a repository?", &ev, &p);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_DIRECT);
  asngn_route_heuristic("what is a file?", &ev, &p);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_DIRECT);
}

TEST(heuristic_local_repo_question_plans) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic("what files are in this repo?", &ev, &p);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN);
  asngn_route_heuristic("which files are in the current directory?", &ev,
                        &p);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN);
}

TEST(heuristic_explicit_tool_request_plans) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic(
      "Use the available tools to inspect the current repository", &ev, &p);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN);
}

/* ── evidence axes ───────────────────────────────────────────────────── */

/* Repository scale raises CLASS on tool-work turns only. */
TEST(evidence_repo_size_raises_plan_class) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  ev.repo_files = 5000;
  asngn_route_heuristic("fix the bug in src/main.c", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX); /* debug 3 + large repo 2 */
  asngn_route_heuristic("what time is it?", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);  /* DIRECT: size ignored */
}

/* The repository's dominant language stands in when the message names
 * none; systems languages weigh one point on code tasks. */
TEST(evidence_repo_language_raises_code_class) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  snprintf(ev.repo_language, sizeof ev.repo_language, "c");
  asngn_route_heuristic("fix the bug in src/main.c", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX); /* debug 3 + systems 1 */
  snprintf(ev.repo_language, sizeof ev.repo_language, "python");
  asngn_route_heuristic("fix the bug in src/main.c", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE);
}

/* Recent escalations and unreliable turns raise CLASS one step each,
 * instead of the old blanket "anything escalated recently ⇒ COMPLEX". */
TEST(evidence_history_raises_class_gradually) {
  asngn_route_profile p;
  asngn_route_evidence ev;
  memset(&ev, 0, sizeof ev);
  ev.window = 8;
  ev.escalated = 2;
  ev.unreliable = 2;
  asngn_route_heuristic("hi", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE); /* chat 0 + 1 + 1 */
  ev.unreliable = 0;
  asngn_route_heuristic("fix the bug in src/main.c", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX);  /* debug 3 + history 1 */
}

/* A recorded eval-suite failure rate below one half biases tool-work
 * turns one step up; a healthy record leaves the verdict alone. */
TEST(evidence_eval_calibration_biases_plan_class) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  ev.has_eval = true;
  ev.eval_success = 0.3;
  asngn_route_heuristic("fix the bug in src/main.c", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX); /* debug 3 + eval 1 */
  ev.eval_success = 1.0;
  asngn_route_heuristic("fix the bug in src/main.c", &ev, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE);
}

/* A language named in the message beats the repository census. */
TEST(evidence_message_language_beats_repo) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  snprintf(ev.repo_language, sizeof ev.repo_language, "c");
  asngn_route_heuristic("implement a python function for the parsing",
                        &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_GENERATE);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE); /* generate 3, no bump */
}

/* A generation verb with a named language routes GENERATE even when the
 * object is no code noun: "calculator" is in no keyword list, but "in
 * c++" says the deliverable is code (the bug that showed this: the turn
 * routed chat/TERSE and the source never reached the workspace). */
TEST(heuristic_generate_by_named_language) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic("write a calculator in c++", &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_GENERATE);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX); /* generate 3 + systems 1 */
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH);  /* code output is bulky */
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN);
  ASSERT_TRUE((p.toolmask & ASNGN_TOOLF_EDIT) != 0);
  ASSERT_TRUE((p.toolmask & ASNGN_TOOLF_FS) != 0);
  asngn_route_heuristic("create a calculator in python", &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_GENERATE);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_MODERATE); /* generate 3, no bump */
  /* the verb alone stays chat: "write a poem" names no code evidence */
  asngn_route_heuristic("write a poem about the sea", &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_CHAT);
}

/* Build/run asks route as BUILD and imply the proc family. */
TEST(heuristic_build_task_tools) {
  asngn_route_profile p;
  asngn_route_evidence ev = ev_tools();
  asngn_route_heuristic("run all the tests in the suite", &ev, &p);
  ASSERT_EQ_INT(p.task, ASNGN_RTASK_BUILD);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN);
  ASSERT_TRUE((p.toolmask & ASNGN_TOOLF_PROC) != 0);
}

/* ── tier ladder ─────────────────────────────────────────────────────── */

TEST(tier_ladder_up) {
  asngn_ctx *c = ctx_defaults();
  ASSERT_TRUE(c != NULL);
  /* default pool: 0 nano, 1 light, 2 std, 3 embed (embedding) */
  ASSERT_EQ_INT(asngn_route_tier_up(c, 0), 1);  /* nano -> light  */
  ASSERT_EQ_INT(asngn_route_tier_up(c, 1), 2);  /* light -> std   */
  ASSERT_EQ_INT(asngn_route_tier_up(c, 2), -1); /* embed is skipped */
  ASSERT_EQ_INT(asngn_route_tier_up(c, 3), -1);
  ASSERT_EQ_INT(asngn_route_tier_up(c, -1), -1);
  ASSERT_EQ_INT(asngn_route_tier_up(c, 99), -1);
  ASSERT_EQ_INT(asngn_route_tier_up(NULL, 0), -1);
  ctx_free(c);
}

TEST(tier_ladder_down) {
  asngn_ctx *c = ctx_defaults();
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asngn_route_tier_down(c, 0), -1); /* nano is the floor */
  ASSERT_EQ_INT(asngn_route_tier_down(c, 1), 0);  /* light -> nano    */
  ASSERT_EQ_INT(asngn_route_tier_down(c, 2), 1);  /* std -> light     */
  ASSERT_EQ_INT(asngn_route_tier_down(c, -1), -1);
  ASSERT_EQ_INT(asngn_route_tier_down(c, 99), -1);
  ASSERT_EQ_INT(asngn_route_tier_down(NULL, 2), -1);
  ctx_free(c);
}

TEST_LIST = {
  TEST_ENTRY(heuristic_short_greeting),
  TEST_ENTRY(heuristic_short_but_hard_generates_complex),
  TEST_ENTRY(heuristic_debug_task_moderate_plan),
  TEST_ENTRY(heuristic_long_prose_moderate),
  TEST_ENTRY(heuristic_fence_moderate_rich),
  TEST_ENTRY(heuristic_depth_ask_rich),
  TEST_ENTRY(heuristic_question_only_direct),
  TEST_ENTRY(heuristic_conceptual_repo_question_direct),
  TEST_ENTRY(heuristic_local_repo_question_plans),
  TEST_ENTRY(heuristic_explicit_tool_request_plans),
  TEST_ENTRY(evidence_repo_size_raises_plan_class),
  TEST_ENTRY(evidence_repo_language_raises_code_class),
  TEST_ENTRY(evidence_history_raises_class_gradually),
  TEST_ENTRY(evidence_eval_calibration_biases_plan_class),
  TEST_ENTRY(evidence_message_language_beats_repo),
  TEST_ENTRY(heuristic_generate_by_named_language),
  TEST_ENTRY(heuristic_build_task_tools),
  TEST_ENTRY(tier_ladder_up),
  TEST_ENTRY(tier_ladder_down),
};

RUN_ALL_TESTS()
