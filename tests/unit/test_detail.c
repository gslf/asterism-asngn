/*
 * test_detail.c — detail caps, directives, in-message cues, and the
 * selection order with pressure levers.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

/* Bare context with default configuration (caps, warn_at 0.80). */
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

TEST(caps_defaults) {
  asngn_ctx *c = ctx_defaults();
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asngn_detail_cap(c, ASNGN_DETAIL_TERSE), 128);
  ASSERT_EQ_INT(asngn_detail_cap(c, ASNGN_DETAIL_NORMAL), 384);
  ASSERT_EQ_INT(asngn_detail_cap(c, ASNGN_DETAIL_RICH), 1024);
  ASSERT_EQ_INT(asngn_detail_cap(c, ASNGN_DETAIL_AUTO), 384); /* defensive */
  ctx_free(c);
}

TEST(directives_exact) {
  ASSERT_EQ_STR(asngn_detail_directive(ASNGN_DETAIL_TERSE),
                "Answer directly. No preamble, no recap, no closing summary.");
  ASSERT_EQ_STR(asngn_detail_directive(ASNGN_DETAIL_NORMAL),
                "Answer completely but economically; expand only what the "
                "question needs.");
  ASSERT_EQ_STR(asngn_detail_directive(ASNGN_DETAIL_RICH),
                "Answer thoroughly, with structure and examples where "
                "useful.");
  ASSERT_EQ_STR(asngn_detail_directive(ASNGN_DETAIL_AUTO),
                asngn_detail_directive(ASNGN_DETAIL_NORMAL));
}

TEST(names) {
  ASSERT_EQ_STR(asngn_detail_name(ASNGN_DETAIL_TERSE), "terse");
  ASSERT_EQ_STR(asngn_detail_name(ASNGN_DETAIL_NORMAL), "normal");
  ASSERT_EQ_STR(asngn_detail_name(ASNGN_DETAIL_RICH), "rich");
  ASSERT_EQ_STR(asngn_detail_name(ASNGN_DETAIL_AUTO), "auto");
}

TEST(cues) {
  ASSERT_EQ_INT(asngn_detail_cue("briefly please"), ASNGN_DETAIL_TERSE);
  ASSERT_EQ_INT(asngn_detail_cue("spiegamelo in dettaglio"),
                ASNGN_DETAIL_RICH);
  ASSERT_EQ_INT(asngn_detail_cue("hello"), ASNGN_DETAIL_AUTO);
  ASSERT_EQ_INT(asngn_detail_cue(NULL), ASNGN_DETAIL_AUTO);
}

TEST(effective_user_override_wins_under_pressure) {
  asngn_ctx *c = ctx_defaults();
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asngn_detail_effective(c, ASNGN_DETAIL_RICH,
                                       ASNGN_DETAIL_TERSE,
                                       ASNGN_CLASS_SIMPLE, 1.0),
                ASNGN_DETAIL_RICH);
  ctx_free(c);
}

TEST(effective_warn_pressure_steps_rich_down) {
  asngn_ctx *c = ctx_defaults(); /* warn_at 0.80 */
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asngn_detail_effective(c, ASNGN_DETAIL_AUTO,
                                       ASNGN_DETAIL_RICH,
                                       ASNGN_CLASS_MODERATE, 0.9),
                ASNGN_DETAIL_NORMAL);
  ctx_free(c);
}

TEST(effective_normal_to_terse_only_when_simple) {
  asngn_ctx *c = ctx_defaults();
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asngn_detail_effective(c, ASNGN_DETAIL_AUTO,
                                       ASNGN_DETAIL_NORMAL,
                                       ASNGN_CLASS_SIMPLE, 0.9),
                ASNGN_DETAIL_TERSE);
  ASSERT_EQ_INT(asngn_detail_effective(c, ASNGN_DETAIL_AUTO,
                                       ASNGN_DETAIL_NORMAL,
                                       ASNGN_CLASS_MODERATE, 0.9),
                ASNGN_DETAIL_NORMAL);
  ctx_free(c);
}

TEST(effective_full_pressure_forces_terse) {
  asngn_ctx *c = ctx_defaults();
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asngn_detail_effective(c, ASNGN_DETAIL_AUTO,
                                       ASNGN_DETAIL_RICH,
                                       ASNGN_CLASS_COMPLEX, 1.0),
                ASNGN_DETAIL_TERSE);
  ctx_free(c);
}

TEST(effective_default_auto_takes_classifier_vote) {
  asngn_ctx *c = ctx_defaults(); /* detail_default = AUTO */
  ASSERT_TRUE(c != NULL);
  ASSERT_EQ_INT(asngn_detail_effective(c, ASNGN_DETAIL_AUTO,
                                       ASNGN_DETAIL_NORMAL,
                                       ASNGN_CLASS_MODERATE, 0.0),
                ASNGN_DETAIL_NORMAL);
  ctx_free(c);
}

TEST_LIST = {
  TEST_ENTRY(caps_defaults),
  TEST_ENTRY(directives_exact),
  TEST_ENTRY(names),
  TEST_ENTRY(cues),
  TEST_ENTRY(effective_user_override_wins_under_pressure),
  TEST_ENTRY(effective_warn_pressure_steps_rich_down),
  TEST_ENTRY(effective_normal_to_terse_only_when_simple),
  TEST_ENTRY(effective_full_pressure_forces_terse),
  TEST_ENTRY(effective_default_auto_takes_classifier_vote),
};

RUN_ALL_TESTS()
