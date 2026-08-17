/*
 * test_route.c — the heuristic classification table and the
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

TEST(heuristic_short_greeting) {
  asngn_route_profile p;
  asngn_route_heuristic("ciao", false, false, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_TERSE);  /* len < 80 */
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_DIRECT);     /* no tools */
}

TEST(heuristic_imperative_with_tools_plans) {
  asngn_route_profile p;
  asngn_route_heuristic("fix the bug in src/main.c", true, false, &p);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_PLAN); /* imperative verb + file path */
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);
}

TEST(heuristic_long_message_complex) {
  char msg[951];
  asngn_route_profile p;
  memset(msg, 'a', sizeof msg - 1);
  msg[sizeof msg - 1] = '\0';
  asngn_route_heuristic(msg, false, false, &p); /* len 950 > 900 */
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH); /* len > 600 */
}

TEST(heuristic_fence_complex_rich) {
  asngn_route_profile p;
  asngn_route_heuristic("look:\n```\ncode\n```", false, false, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH);
}

TEST(heuristic_recent_escalation_complex) {
  asngn_route_profile p;
  asngn_route_heuristic("hi", false, true, &p);
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_COMPLEX);
}

TEST(heuristic_depth_ask_rich) {
  asngn_route_profile p;
  asngn_route_heuristic("spiega in dettaglio X", false, false, &p);
  ASSERT_EQ_INT(p.detail, ASNGN_DETAIL_RICH); /* depth ask beats len<80 */
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);
}

TEST(heuristic_question_only_direct) {
  asngn_route_profile p;
  asngn_route_heuristic("what time is it?", true, false, &p);
  ASSERT_EQ_INT(p.mode, ASNGN_MODE_DIRECT); /* tools, but nothing to plan */
  ASSERT_EQ_INT(p.klass, ASNGN_CLASS_SIMPLE);
}

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
  TEST_ENTRY(heuristic_imperative_with_tools_plans),
  TEST_ENTRY(heuristic_long_message_complex),
  TEST_ENTRY(heuristic_fence_complex_rich),
  TEST_ENTRY(heuristic_recent_escalation_complex),
  TEST_ENTRY(heuristic_depth_ask_rich),
  TEST_ENTRY(heuristic_question_only_direct),
  TEST_ENTRY(tier_ladder_up),
  TEST_ENTRY(tier_ladder_down),
};

RUN_ALL_TESTS()
