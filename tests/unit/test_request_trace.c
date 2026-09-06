/* Trace content, structure and omissions without retaining raw model input. */
#include "asngn_test.h"
#include "context.h"

static asmodel_json_value *render(const asmodel_input *input, const char *grammar,
                                  const asmodel_generate_params *params, asngn_err admission) {
  asngn_ctx *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->models_n = 1;
  strcpy(c->models[0].cfg.id, "test-model");
  asngn_context_diagnostics budget = {.prompt_total=900, .prompt_budget=1500,
                                      .output_reserve=100, .safety_margin=50};
  char *json = asngn_request_trace(c, NULL, 0, ASNGN_TASK_DECIDE, input, grammar, params,
                                   &budget, admission);
  free(c);
  asmodel_json_value *value = NULL;
  if (json) (void)asmodel_json_parse(json, strlen(json), &value);
  free(json);
  return value;
}
static const char *digest(const asmodel_json_value *value) {
  return asmodel_json_string_value(asmodel_json_object_get(value, "input_contract_sha256"));
}
TEST(native_roles_and_correlations_are_observable_without_payloads) {
  asmodel_block blocks[] = {
    {.kind=ASMODEL_BLOCK_TEXT, .text="PRIVATE_USER_TEXT"},
    {.kind=ASMODEL_BLOCK_TOOL_CALL, .id="private_call", .name="lookup", .text="{\"secret\":true}"},
    {.kind=ASMODEL_BLOCK_TOOL_RESULT, .id="private_call", .text="PRIVATE_TOOL_RESULT"},
  };
  asmodel_message messages[] = {{ASMODEL_ROLE_USER, blocks, 1},
    {ASMODEL_ROLE_ASSISTANT, blocks+1, 1}, {ASMODEL_ROLE_TOOL, blocks+2, 1}};
  asmodel_input input = {messages, 3};
  ASSERT_EQ_INT(asmodel_input_validate(&input), ASMODEL_OK);
  asmodel_tool_schema schema = {"lookup", "PRIVATE_DESCRIPTION", "{\"type\":\"object\"}"};
  asmodel_tools tools = {.schemas=&schema, .count=1};
  asmodel_generate_params params = {.tools=&tools, .max_tokens=100};
  asmodel_json_value *value = render(&input, NULL, &params, ASNGN_OK);
  ASSERT_TRUE(value && digest(value));
  const asmodel_json_value *items = asmodel_json_object_get(value, "items");
  ASSERT_EQ_INT(asmodel_json_array_len(items), 4);
  const asmodel_json_value *call = asmodel_json_array_at(items, 2), *result = asmodel_json_array_at(items, 3);
  ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_object_get(call, "role")), "assistant");
  ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_object_get(result, "role")), "tool");
  ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_object_get(asmodel_json_object_get(call, "correlation"), "sha256")),
                asmodel_json_string_value(asmodel_json_object_get(asmodel_json_object_get(result, "correlation"), "sha256")));
  char *json = asmodel_json_write(value, 0);
  ASSERT_TRUE(json && !strstr(json, "PRIVATE_") && !strstr(json, "secret") && !strstr(json, "private_call"));
  ASSERT_EQ_INT(asmodel_json_int_value(asmodel_json_object_get(value, "prompt_total")), 900);
  free(json);
  asmodel_json_free(value);
}
TEST(omitted_detail_still_changes_the_complete_input_hash) {
  asmodel_message messages[256];
  asmodel_block blocks[256];
  asmodel_tool_schema schemas[64];
  char names[64][16];
  for (size_t i = 0; i < 256; i++) {
    blocks[i] = (asmodel_block){.kind=ASMODEL_BLOCK_TEXT, .text="private repeated history"};
    messages[i] = (asmodel_message){ASMODEL_ROLE_USER, &blocks[i], 1};
  }
  for (size_t i = 0; i < 64; i++) {
    snprintf(names[i], sizeof names[i], "tool_%zu", i);
    schemas[i] = (asmodel_tool_schema){names[i], "private description", "{}"};
  }
  asmodel_input input = {messages, 256};
  asmodel_tools tools = {.schemas=schemas, .count=64};
  asmodel_generate_params params = {.tools=&tools};
  asmodel_json_value *before = render(&input, NULL, &params, ASNGN_OK);
  ASSERT_TRUE(before && digest(before));
  blocks[255].text = "changed excluded detail";
  asmodel_json_value *after = render(&input, NULL, &params, ASNGN_OK);
  ASSERT_TRUE(after && digest(after));
  ASSERT_TRUE(strcmp(digest(before), digest(after)));
  ASSERT_EQ_INT(asmodel_json_int_value(asmodel_json_object_get(after, "items_total")), 320);
  ASSERT_EQ_INT(asmodel_json_int_value(asmodel_json_object_get(after, "items_omitted")), 192);
  char *a = asmodel_json_write(asmodel_json_object_get(before, "items"), 0);
  char *b = asmodel_json_write(asmodel_json_object_get(after, "items"), 0);
  ASSERT_EQ_STR(a, b);
  free(a); free(b);
  a = asmodel_json_write(after, 0);
  ASSERT_TRUE(a && strlen(a) < 60000);
  free(a);
  asmodel_json_free(before); asmodel_json_free(after);
}
TEST(boundaries_roles_and_output_contracts_have_distinct_identities) {
  asmodel_block blocks[] = {{.text="ab"}, {.text="c"}};
  asmodel_message messages[] = {{ASMODEL_ROLE_USER, blocks, 2}};
  asmodel_input input = {messages, 1};
  asmodel_generate_params params = {0};
  asmodel_json_value *before = render(&input, NULL, &params, ASNGN_OK);
  ASSERT_TRUE(before && digest(before));
  for (int change = 0; change < 4; change++) {
    blocks[0].text = change == 0 ? "a" : "ab";
    blocks[1].text = change == 0 ? "bc" : "c";
    messages[0].role = change == 1 ? ASMODEL_ROLE_ASSISTANT : ASMODEL_ROLE_USER;
    params.output_schema = change == 2 ? "{\"type\":\"string\"}" : NULL;
    asmodel_json_value *after = render(&input, change == 3 ? "root ::= \"ok\"" : NULL, &params, ASNGN_OK);
    ASSERT_TRUE(after && digest(after) && strcmp(digest(before), digest(after)));
    asmodel_json_free(after);
  }
  asmodel_json_free(before);
}
TEST(invalid_input_is_not_traversed_for_diagnostics) {
  asmodel_input invalid = {NULL, 256};
  asmodel_generate_params params = {0};
  asmodel_json_value *value = render(&invalid, NULL, &params, ASNGN_ERR_INVALID);
  ASSERT_TRUE(value);
  ASSERT_TRUE(!asmodel_json_object_get(value, "items") && !digest(value));
  ASSERT_EQ_INT(asmodel_json_typeof(asmodel_json_object_get(value, "prompt_total")), ASMODEL_JSON_NULL);
  ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_object_get(value, "admission")), "ASNGN_ERR_INVALID");
  asmodel_json_free(value);
}
TEST_LIST = {TEST_ENTRY(native_roles_and_correlations_are_observable_without_payloads),
  TEST_ENTRY(omitted_detail_still_changes_the_complete_input_hash),
  TEST_ENTRY(boundaries_roles_and_output_contracts_have_distinct_identities),
  TEST_ENTRY(invalid_input_is_not_traversed_for_diagnostics)};
RUN_ALL_TESTS()
