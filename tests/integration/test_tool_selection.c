#include "asngn_test.h"
#include "engine_fx.h"
#include "astools.h"

TEST(selection_binds_every_representation) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_turn_state t = {.s = f.s};
  f.c->cfg.tool_limit = 1;
  ASSERT_OK(asngn_tools_select(f.c, &t, "fake run"));
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake", "run"), 0);
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake@1.0.0", "run"), 0);
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake@2.0.0", "run"), -1);
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake", "mut"), -1);
  ASSERT_CONTAINS(t.catalog, "fake.run");
  ASSERT_NOT_CONTAINS(t.catalog, "fake.mut");
  char *schema = NULL, *grammar = NULL;
  ASSERT_OK(asngn_protocol_steps(astools_selection_schemas(t.tool_selection), true, false, false,
                                 true, 0, false, &schema));
  ASSERT_OK(asngn_grammar_steps(f.c, true, false, false, true, 0,
                                astools_selection_grammar(t.tool_selection), &grammar));
  ASSERT_CONTAINS(schema, "fake.run");
  ASSERT_NOT_CONTAINS(schema, "fake.mut");
  ASSERT_CONTAINS(grammar, "fake.run");
  ASSERT_NOT_CONTAINS(grammar, "fake.mut");
  char *decision =
      asngn_strdup("{\"action\":\"discover\",\"why\":\"find mutation\",\"input\":\"fake mut\"}");
  ASSERT_OK(asngn_protocol_decode(ASNGN_TASK_DECIDE, schema, &decision));
  asngn_step step;
  ASSERT_OK(asngn_step_parse(f.c, decision, &step));
  ASSERT_EQ_INT(step.kind, ASNGN_STEP_DISCOVER);
  ASSERT_OK(asngn_tools_select(f.c, &t, step.text));
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake", "mut"), 0);
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake", "run"), -1);
  asngn_step_free(&step);
  free(decision);
  free(schema);
  free(grammar);
  t.security_profile = ASNGN_SECURITY_CODING_READONLY;
  ASSERT_OK(asngn_tools_select(f.c, &t, "fake mut"));
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake", "mut"), -1);
  ASSERT_EQ_INT(asngn_tools_find(&t, "fake", "run"), 0);
  astools_result r = {0};
  ASSERT_EQ_INT(asngn_tools_invoke(&t, "fake.run", "{msg:\"ok\"}", 1000, &r), ASTOOLS_OK);
  ASSERT_TRUE(r.ok);
  astools_result_free(&r);
  ASSERT_EQ_INT(astools_tool_enable(f.c->astools, "fake", 0), ASTOOLS_OK);
  ASSERT_EQ_INT(asngn_tools_invoke(&t, "fake.run", "{msg:\"ok\"}", 1000, &r), ASTOOLS_ERR_DENIED);
  astools_result_free(&r);
  t.cancel = 1;
  ASSERT_EQ_INT(asngn_tools_invoke(&t, "fake.run", "{msg:\"ok\"}", 1000, &r),
                ASTOOLS_ERR_CANCELLED);
  t.cancel = 0;
  ASSERT_TRUE(asngn_fix_registry(f.reg_raw, "fs", eng_tool_path(), "echo"));
  ASSERT_EQ_INT(astools_registry_refresh(f.c->astools), ASTOOLS_OK);
  t.security_profile = ASNGN_SECURITY_CODING_SANDBOXED;
  ASSERT_OK(asngn_tools_select(f.c, &t, "fs write"));
  char args[512];
  snprintf(args, sizeof args, "{path:\"%s/blocked\",content:\"no\"}", f.root_raw);
  ASSERT_EQ_INT(astools_selection_validate(t.tool_selection, "fs.write", args), ASTOOLS_ERR_DENIED);
  asngn_turn_state_free(&t);
  eng_drop(&f);
}

TEST(discovery_reaches_a_previously_omitted_command) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  f.c->cfg.tool_limit = 1;
  ASSERT_TRUE(
      fake_model_push(&f.nano, "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n"));
  ASSERT_TRUE(fake_model_push(
      &f.light, "{action: \"discover\", why: \"find inspection\", input: \"fake run\"}\n"));
  ASSERT_TRUE(
      fake_model_push(&f.light, "{action: \"call\", why: \"inspect\", input: fake.run "
                                "{msg:\"seen\"}, success:\"echo\", fallback:\"report\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action:\"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Observed seen.\n"));
  asngn_turn_result r = {0};
  /* The initial intent selects mut; discovery must replace that snapshot. */
  ASSERT_OK(eng_turn(&f, "fake mut", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Observed seen.\n");
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 1);
  ASSERT_CONTAINS(f.light.last_system, "fake.run");
  ASSERT_NOT_CONTAINS(f.light.last_schema, "fake.mut");
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST_LIST = {TEST_ENTRY(selection_binds_every_representation),
             TEST_ENTRY(discovery_reaches_a_previously_omitted_command)};
RUN_ALL_TESTS()
