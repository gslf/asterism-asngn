#include "asngn_test.h"
#include "engine_fx.h"
#include "native.h"

static int queue_lookup(eng_fx *f, const char *calls) {
  return fake_model_push(&f->nano, "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n") &&
         fake_model_push_native(&f->stdm, calls);
}

TEST(native_calls_keep_correlated_results_and_use_the_generator) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", "routing: {native_actions:true}"));
  ASSERT_TRUE(f.c->cfg.native_actions);
  ASSERT_TRUE(queue_lookup(&f, "[{\"id\":\"read-1\",\"name\":\"fake.run\",\"arguments\":\"{"
                               "\\\"msg\\\":\\\"first observation\\\"}\"},"
                               "{\"id\":\"read-2\",\"name\":\"fake.run\",\"arguments\":\"{"
                               "\\\"msg\\\":\\\"second observation\\\"}\"}]"));
  ASSERT_TRUE(fake_model_push_native(
      &f.stdm, "[{\"id\":\"finish\",\"name\":\"asterism_finish\",\"arguments\":\"{}\"}]"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Both observations received.\n"));
  asngn_turn_result r = {0};
  ASSERT_OK(eng_turn(&f, "Inspect fake run twice", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Both observations received.\n");
  ASSERT_EQ_INT(f.stdm.native_calls, 2);
  ASSERT_EQ_INT(f.light.calls, 0);
  ASSERT_CONTAINS(f.stdm.last_native_results, "read-1:");
  ASSERT_CONTAINS(f.stdm.last_native_results, "first observation");
  ASSERT_CONTAINS(f.stdm.last_native_results, "read-2:");
  ASSERT_CONTAINS(f.stdm.last_native_results, "second observation");
  ASSERT_EQ_INT(f.stdm.reasoning_seen[0], ASMODEL_REASONING_DEFAULT);
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 2);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(native_batches_validate_before_the_first_effect) {
  const char *batches[] = {
      "[{\"id\":\"a\",\"name\":\"fake.run\",\"arguments\":\"{\\\"msg\\\":\\\"valid\\\"}\"},{\"id\":"
      "\"b\",\"name\":\"fake.run\",\"arguments\":\"{\\\"msg\\\":3}\"}]",
      "[{\"id\":\"a\",\"name\":\"fake.run\",\"arguments\":\"{\\\"msg\\\":\\\"valid\\\"}\"},{\"id\":"
      "\"b\",\"name\":\"fake.mut\",\"arguments\":\"{\\\"msg\\\":\\\"mutate\\\"}\"}]",
      "[{\"id\":\"a\",\"name\":\"asterism_clarify\",\"arguments\":\"{\\\"input\\\":\\\"ask\\\","
      "\\\"extra\\\":true}\"}]"};
  for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) {
    eng_fx f;
    ASSERT_TRUE(eng_setup(&f, "echo", "routing: {native_actions:true}"));
    ASSERT_TRUE(queue_lookup(&f, batches[i]));
    asngn_turn_result r = {0};
    ASSERT_ERR(eng_turn(&f, "Inspect fake tools", NULL, NULL, &r), ASNGN_ERR_PROTOCOL);
    asngn_stats stats;
    ASSERT_OK(asngn_get_stats(f.c, &stats));
    ASSERT_EQ_INT(stats.tool_calls, 0);
    asngn_turn_result_free(&r);
    eng_drop(&f);
  }
}

TEST(native_error_never_executes_partial_proposals) {
  asngn_err errors[] = {ASNGN_ERR_LIMIT, ASNGN_ERR_CANCELLED, ASNGN_ERR_TIMEOUT};
  for (size_t i = 0; i < 3; i++) {
    eng_fx f;
    ASSERT_TRUE(eng_setup(&f, "echo", "routing: {native_actions:true}"));
    f.stdm.native_error = errors[i];
    ASSERT_TRUE(queue_lookup(&f, "[{\"id\":\"a\",\"name\":\"fake.run\",\"arguments\":\"{"
                                 "\\\"msg\\\":\\\"no effect\\\"}\"}]"));
    asngn_turn_result r = {0};
    ASSERT_ERR(eng_turn(&f, "Inspect fake run", NULL, NULL, &r), errors[i]);
    asngn_stats stats;
    ASSERT_OK(asngn_get_stats(f.c, &stats));
    ASSERT_EQ_INT(stats.tool_calls, 0);
    asngn_turn_result_free(&r);
    eng_drop(&f);
  }
}

TEST(native_discovery_changes_the_available_contract) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", "routing: {native_actions:true}"));
  f.c->cfg.tool_limit = 1;
  ASSERT_TRUE(queue_lookup(&f, "[{\"id\":\"discover\",\"name\":\"asterism_discover\",\"arguments\":"
                               "\"{\\\"input\\\":\\\"fake run\\\"}\"}]"));
  ASSERT_TRUE(
      fake_model_push_native(&f.stdm, "[{\"id\":\"read\",\"name\":\"fake.run\",\"arguments\":\"{"
                                      "\\\"msg\\\":\\\"discovered\\\"}\"}]"));
  ASSERT_TRUE(fake_model_push_native(
      &f.stdm, "[{\"id\":\"finish\",\"name\":\"asterism_finish\",\"arguments\":\"{}\"}]"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Found the inspection tool.\n"));
  asngn_turn_result r = {0};
  ASSERT_OK(eng_turn(&f, "fake mut", NULL, NULL, &r));
  ASSERT_CONTAINS(f.stdm.last_native_results, "discovered");
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(native_contracts_preserve_profiles_and_artifact_gates) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_turn_state t = {
      .s = f.s, .gen_slot = 2, .security_profile = ASNGN_SECURITY_CODING_READONLY};
  ASSERT_OK(asngn_tools_select(f.c, &t, "fake mut"));
  asngn_native_contract *contract = calloc(1, sizeof *contract);
  ASSERT_TRUE(contract != NULL);
  ASSERT_OK(asngn_native_contract_build(f.c, &t, true, contract));
  for (size_t i = 0; i < contract->count; i++)
    ASSERT_NOT_CONTAINS(contract->schemas[i].description, "fake.mut:");
  t.security_profile = ASNGN_SECURITY_CODING_SANDBOXED;
  t.prof.task = ASNGN_RTASK_GENERATE;
  ASSERT_OK(asngn_native_contract_build(f.c, &t, true, contract));
  for (size_t i = 0; i < contract->count; i++)
    ASSERT_TRUE(strcmp(contract->schemas[i].name, "asterism_finish"));
  asngn_step finish = {.kind = ASNGN_STEP_ANSWER};
  bool done = false;
  ASSERT_ERR(asngn_action_apply(f.c, &t, &finish, false, false, false, &done), ASNGN_ERR_PROTOCOL);
  ASSERT_TRUE(!done);
  t.cancel = 1;
  ASSERT_ERR(asngn_action_apply(f.c, &t, &finish, false, false, false, &done), ASNGN_ERR_CANCELLED);
  free(contract);
  asngn_turn_state_free(&t);
  eng_drop(&f);
}

TEST_LIST = {TEST_ENTRY(native_calls_keep_correlated_results_and_use_the_generator),
             TEST_ENTRY(native_batches_validate_before_the_first_effect),
             TEST_ENTRY(native_error_never_executes_partial_proposals),
             TEST_ENTRY(native_discovery_changes_the_available_contract),
             TEST_ENTRY(native_contracts_preserve_profiles_and_artifact_gates)};
RUN_ALL_TESTS()
