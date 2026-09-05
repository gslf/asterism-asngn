#include "asngn_test.h"
#include "engine_fx.h"
#include "astools.h"
#include "execution.h"

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
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"inspect\", input: fake.run "
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

TEST(structured_generation_preserves_roles_and_admission) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_turn_state turn = {.s = f.s, .gen_slot = 2};
  asmodel_block blocks[] = {
      {.kind = ASMODEL_BLOCK_TEXT, .text = "system"},
      {.kind = ASMODEL_BLOCK_TEXT, .text = "inspect the result"},
      {.kind = ASMODEL_BLOCK_TOOL_CALL, .id = "observed-1", .name = "inspect", .text = "{}"},
      {.kind = ASMODEL_BLOCK_TOOL_RESULT, .id = "observed-1", .text = "actual evidence"},
      {.kind = ASMODEL_BLOCK_TEXT, .text = "explain"}};
  asmodel_message messages[] = {{ASMODEL_ROLE_SYSTEM, &blocks[0], 1},
                                {ASMODEL_ROLE_USER, &blocks[1], 1},
                                {ASMODEL_ROLE_ASSISTANT, &blocks[2], 1},
                                {ASMODEL_ROLE_TOOL, &blocks[3], 1},
                                {ASMODEL_ROLE_USER, &blocks[4], 1}};
  asmodel_input input = {messages, 5};
  char *text = NULL;
  int ti = 0, to = 0;
  ASSERT_TRUE(fake_model_push(&f.stdm, "Evidence explained."));
  ASSERT_OK(asngn_generate_input(f.c, &turn, 2, ASNGN_TASK_ANSWER, &input, NULL, NULL, NULL, 128,
                                 NULL, NULL, &text, &ti, &to));
  ASSERT_EQ_STR(text, "Evidence explained.");
  free(text);
  text = NULL;
  ASSERT_EQ_INT(f.stdm.input_messages, 5);
  ASSERT_EQ_INT(f.stdm.input_roles[2], ASMODEL_ROLE_ASSISTANT);
  ASSERT_EQ_INT(f.stdm.input_roles[3], ASMODEL_ROLE_TOOL);
  ASSERT_EQ_INT(f.stdm.input_roles[4], ASMODEL_ROLE_USER);
  int calls = f.stdm.calls;
  asmodel_tool_calls proposed = {0};
  asmodel_tool_schema schema = {"inspect", "inspect evidence", "{\"type\":\"object\"}"};
  asmodel_tools tools = {
      .schemas = &schema, .count = 1, .choice = ASMODEL_TOOLS_AUTO, .output = &proposed};
  int occupied = asngn_models_count_input(f.c, 2, &input);
  f.c->models[2].cfg.ctx = occupied + 128 + f.c->cfg.safety_margin;
  /* Plain input fits exactly; the native schema must also be reserved. */
  ASSERT_ERR(asngn_generate_input(f.c, &turn, 2, ASNGN_TASK_DECIDE, &input, NULL, NULL, &tools, 128,
                                  NULL, NULL, &text, &ti, &to),
             ASNGN_ERR_CONTEXT);
  ASSERT_TRUE(text == NULL && proposed.count == 0);
  ASSERT_EQ_INT(f.stdm.calls, calls);
  turn.deadline_mono = f.clk.mono_ms - 1;
  ASSERT_ERR(asngn_generate_input(f.c, &turn, 2, ASNGN_TASK_DECIDE, &input, NULL, NULL, &tools, 128,
                                  NULL, NULL, &text, &ti, &to),
             ASNGN_ERR_TIMEOUT);
  ASSERT_TRUE(text == NULL && proposed.count == 0 && ti == 0 && to == 0);
  asmodel_tool_calls_clear(&proposed);
  eng_drop(&f);
}

TEST_LIST = {TEST_ENTRY(structured_generation_preserves_roles_and_admission),
             TEST_ENTRY(selection_binds_every_representation),
             TEST_ENTRY(discovery_reaches_a_previously_omitted_command)};
RUN_ALL_TESTS()
