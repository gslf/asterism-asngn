/* Host action observations must join the actual tool dispatch and durable record. */
#include "asngn_test.h"
#include "engine_fx.h"
#include "xcdn.h"

static const char *string(const xcdn_value_t *v, const char *key) {
  return asngn_xstr(asngn_xfield(v, key));
}
static bool bool_field(const xcdn_value_t *v, const char *key) {
  bool value = false;
  return asngn_xbool(asngn_xfield(v, key), &value) && value;
}
static int fail_observed(void *ud, const char *point) {
  (void)ud;
  return !strcmp(point, "observed");
}
TEST(action_events_bind_actual_dispatch_without_claiming_verification) {
  for (int mode = 0; mode < 3; mode++) {
    eng_fx f;
    ASSERT_TRUE(eng_setup(&f, mode == 1 ? "crash" : "echo", NULL));
    ASSERT_TRUE(
        fake_model_push(&f.nano, "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n"));
    ASSERT_TRUE(fake_model_push(
        &f.light, "{action:\"call\",why:\"observe\",input:fake.run {msg:\"private α payload\"},"
                  "success:\"observed\",fallback:\"report\"}\n"));
    ASSERT_TRUE(fake_model_push(&f.light, "{action:\"answer\"}\n"));
    ASSERT_TRUE(fake_model_push(&f.stdm, "The runtime result is available.\n"));
    if (mode == 2)
      f.c->fault = fail_observed;
    asngn_task *task = NULL;
    ASSERT_OK(asngn_submit(f.s, "Run fake.run", NULL, NULL, NULL, &task));
    char task_id[37];
    snprintf(task_id, sizeof task_id, "%s", asngn_task_id(task));
    asngn_turn_result result = {0};
    asngn_err outcome = asngn_task_wait(task, 3000, &result);
    ASSERT_EQ_INT(outcome, mode == 2 ? ASNGN_ERR_IO : ASNGN_OK);
    asngn_task_free(task);
    char **events = NULL;
    size_t count = 0, actions = 0;
    ASSERT_OK(asngn_tele_tail(f.c, 256, &events, &count));
    char action_id[37] = "", observation_hash[65] = "";
    for (size_t i = 0; i < count; i++) {
      xcdn_error_t error;
      xcdn_document_t *doc = xcdn_parse(events[i], &error);
      ASSERT_TRUE(doc != NULL);
      const xcdn_value_t *event = doc->values[0]->value;
      const char *kind = string(event, "kind");
      if (!kind || strcmp(kind, "action")) {
        xcdn_document_free(doc);
        continue;
      }
      const xcdn_value_t *data = asngn_xfield(event, "data");
      ASSERT_TRUE(data != NULL);
      ASSERT_EQ_STR(string(data, "turn_id"), task_id);
      ASSERT_EQ_STR(string(data, "tool_ref"), "fake");
      ASSERT_EQ_STR(string(data, "command"), "run");
      ASSERT_TRUE(asngn_uuid_valid(string(data, "action_id")));
      ASSERT_TRUE(strlen(string(data, "arguments_sha256")) == 64);
      ASSERT_TRUE(strstr(events[i], "private") == NULL);
      ASSERT_TRUE(asngn_xfield(data, "verification_ok") == NULL);
      if (actions++ == 0) {
        ASSERT_EQ_STR(string(data, "state"), "dispatching");
        ASSERT_TRUE(bool_field(data, "journaled"));
        ASSERT_TRUE(asngn_xfield(data, "tool_ok") == NULL);
        snprintf(action_id, sizeof action_id, "%s", string(data, "action_id"));
      } else {
        ASSERT_EQ_STR(string(data, "state"), "observed");
        ASSERT_EQ_STR(string(data, "action_id"), action_id);
        ASSERT_EQ_INT(bool_field(data, "journaled"), mode != 2);
        ASSERT_EQ_INT(bool_field(data, "tool_ok"), mode != 1);
        ASSERT_EQ_INT(!strcmp(string(data, "dispatch_error"), "ASTOOLS_OK"), mode != 1);
        snprintf(observation_hash, sizeof observation_hash, "%s",
                 string(data, "observation_sha256"));
      }
      xcdn_document_free(doc);
    }
    ASSERT_EQ_INT(actions, 2);
    if (mode != 2) {
      asngn_task_record *record = NULL;
      ASSERT_OK(asngn_session_task_read(f.s, task_id, &record));
      ASSERT_EQ_STR(record->action_id, action_id);
      uint8_t hash[32];
      char hex[65];
      asngn_sha256(record->last_observation, strlen(record->last_observation), hash);
      asngn_sha256_hex(hash, 32, hex);
      ASSERT_EQ_STR(observation_hash, hex);
      asngn_task_record_free(record);
    } else
      ASSERT_TRUE(f.s->recovery_required);
    asngn_strings_free(events, count);
    asngn_turn_result_free(&result);
    eng_drop(&f);
  }
}
TEST_LIST = {TEST_ENTRY(action_events_bind_actual_dispatch_without_claiming_verification)};
RUN_ALL_TESTS()
