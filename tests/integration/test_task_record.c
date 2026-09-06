/* Durable outcomes, crash boundaries and read-only evidence retrieval. */
#include "asngn_test.h"
#include "engine_fx.h"
#include "mcp/recovery.h"

static int reopen(eng_fx *f) {
  asngn_session_close(f->s);
  f->s = NULL;
  return asngn_session_open(f->c, "s1", &f->s) == ASNGN_OK;
}
static asngn_err begin(eng_fx *f, asngn_turn_state *t) {
  memset(t, 0, sizeof *t);
  t->s = f->s;
  t->work_revision = 7;
  t->user_msg = "Repair café / 日本語";
  asngn_uuid_v4(t->span_root);
  return asngn_turn_journal(t, "started", t->user_msg);
}
static asngn_err commit(asngn_turn_state *t) {
  asngn_turn u = {0}, a = {0};
  u.n = 1;
  u.at = 1755168000;
  u.text = t->user_msg;
  strcpy(u.role, "user");
  memcpy(u.turn_id, t->span_root, 37);
  asngn_err e = asngn_session_stage_turn(t->s, &u);
  if (e != ASNGN_OK)
    return e;
  t->s->turns = 1;
  a.n = 2;
  a.at = u.at;
  a.text = "An exact durable answer.\n";
  strcpy(a.role, "assistant");
  memcpy(a.turn_id, t->span_root, 37);
  t->led.turn = 2;
  t->led.at = u.at;
  memcpy(t->led.turn_id, t->span_root, 37);
  return asngn_turn_commit(t, &a);
}
static int fail_at(void *ud, const char *point) { return !strcmp(ud, point); }

TEST(finished_task_survives_release_and_reopen) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_work_definition d = {.count = 1};
  strcpy(d.goal, "Repair");
  strcpy(d.criteria[0].id, "tests");
  strcpy(d.criteria[0].requirement, "Run tests");
  strcpy(d.criteria[0].command, "test");
  strcpy(d.criteria[0].path, ".");
  strcpy(d.criteria[0].adapter, "cmake");
  ASSERT_OK(asngn_session_work_define(f.s, 0, &d));
  ASSERT_TRUE(fake_model_push(&f.nano, "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "Hello, 日本語.\n"));
  asngn_task *task = NULL;
  asngn_turn_result result = {0};
  char id[37];
  ASSERT_OK(asngn_submit(f.s, "hi", NULL, NULL, NULL, &task));
  memcpy(id, asngn_task_id(task), 37);
  ASSERT_OK(asngn_task_wait(task, 60000, &result));
  asngn_task_free(task);
  int calls = f.light.calls + f.nano.calls;
  for (int i = 0; i < 2; i++) {
    asngn_task_record *r = NULL;
    ASSERT_OK(asngn_session_task_read(f.s, id, &r));
    ASSERT_EQ_INT(r->state, ASNGN_TASK_FINISHED);
    ASSERT_EQ_INT(r->outcome, ASNGN_OK);
    ASSERT_EQ_INT(r->turn_committed, 1);
    ASSERT_EQ_INT(r->action_uncertain, 0);
    ASSERT_EQ_INT(r->work_revision, 1);
    ASSERT_EQ_STR(r->input, "hi");
    ASSERT_EQ_STR(r->answer, result.answer);
    ASSERT_EQ_STR(r->answer, "Hello, 日本語.\n");
    asngn_task_record_free(r);
    ASSERT_TRUE(reopen(&f));
  }
  ASSERT_OK(asngn_session_work_invalidate(f.s, 1));
  asmodel_json_value *o = NULL;
  ASSERT_OK(mcp_task_recover(f.s, id, &o));
  ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_object_get(o, "task_state")), "superseded");
  ASSERT_EQ_INT(asmodel_json_int_value(asmodel_json_object_get(o, "admitted_work_revision")), 1);
  ASSERT_EQ_INT(asmodel_json_int_value(asmodel_json_object_get(o, "work_revision")), 2);
  ASSERT_EQ_INT(f.light.calls + f.nano.calls, calls);
  asmodel_json_free(o);
  asngn_turn_result_free(&result);
  eng_drop(&f);
}

TEST(failed_and_cancelled_outcomes_are_terminal) {
  const asngn_err failures[] = {ASNGN_ERR_CANCELLED, ASNGN_ERR_TIMEOUT};
  for (size_t i = 0; i < sizeof failures / sizeof *failures; i++) {
    eng_fx f;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    ASSERT_TRUE(fake_model_push(&f.nano, "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
    ASSERT_TRUE(fake_model_push_error(&f.light, failures[i]));
    asngn_task *task = NULL;
    asngn_turn_result result = {0};
    char id[37];
    ASSERT_OK(asngn_submit(f.s, "hi", NULL, NULL, NULL, &task));
    memcpy(id, asngn_task_id(task), 37);
    ASSERT_ERR(asngn_task_wait(task, 60000, &result), failures[i]);
    asngn_task_free(task);
    ASSERT_TRUE(reopen(&f));
    asngn_task_record *r = NULL;
    ASSERT_OK(asngn_session_task_read(f.s, id, &r));
    ASSERT_EQ_INT(r->state, ASNGN_TASK_FINISHED);
    ASSERT_EQ_INT(r->outcome, failures[i]);
    ASSERT_EQ_INT(r->turn_committed, 0);
    ASSERT_EQ_INT(f.s->interrupted_turns, 0);
    ASSERT_EQ_INT(f.s->log_n, 0);
    asngn_consumption usage;
    ASSERT_OK(asngn_get_consumption(f.c,&usage));
    ASSERT_TRUE(usage.lifetime.calls >= 2);
    ASSERT_EQ_INT(usage.lifetime.unsettled_calls,0);
    ASSERT_TRUE(usage.lifetime.charged_tokens > 0);
    ASSERT_EQ_INT(usage.lifetime.cancelled_calls,failures[i] == ASNGN_ERR_CANCELLED);
    ASSERT_EQ_INT(usage.lifetime.failed_calls,failures[i] == ASNGN_ERR_TIMEOUT);
    ASSERT_EQ_INT(f.s->spent_tokens,0); /* The conversation rolled back; inference did not. */
    asngn_task_record_free(r);
    asngn_turn_result_free(&result);
    eng_drop(&f);
  }
}

TEST(action_observation_and_partial_output) {
  for (int observed = 0; observed < 2; observed++) {
    eng_fx f;
    asngn_turn_state t;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    ASSERT_OK(begin(&f, &t));
    asngn_uuid_v4(t.action_id);
    t.action_mutates = true;
    ASSERT_OK(asngn_turn_journal(&t, "action", "edit.apply {patch: pending}"));
    if (observed)
      ASSERT_OK(asngn_turn_journal(&t, "observed", "{ok:false,detail:\"α\"}"));
    ASSERT_TRUE(reopen(&f));
    t.s = f.s;
    asngn_task_record *r = NULL;
    ASSERT_OK(asngn_session_task_read(f.s, t.span_root, &r));
    ASSERT_EQ_INT(r->state, ASNGN_TASK_INTERRUPTED);
    ASSERT_EQ_INT(r->work_revision, 7);
    ASSERT_EQ_INT(r->action_uncertain, !observed);
    ASSERT_EQ_STR(r->action_id, t.action_id);
    ASSERT_EQ_STR(r->last_action, "edit.apply {patch: pending}");
    if (observed)
      ASSERT_EQ_STR(r->last_observation, "{ok:false,detail:\"α\"}");
    else
      ASSERT_TRUE(r->last_observation == NULL);
    asngn_task_record_free(r);
    t.answer = "Partial response α";
    ASSERT_OK(asngn_turn_finished(&t, ASNGN_ERR_CANCELLED));
    ASSERT_TRUE(reopen(&f));
    ASSERT_OK(asngn_session_task_read(f.s, t.span_root, &r));
    ASSERT_EQ_INT(r->state, ASNGN_TASK_FINISHED);
    ASSERT_EQ_STR(r->answer, t.answer);
    ASSERT_EQ_INT(r->action_uncertain, !observed);
    ASSERT_EQ_INT(f.s->uncertain_actions, !observed);
    ASSERT_EQ_INT(f.s->interrupted_turns, 0);
    ASSERT_EQ_INT(f.light.calls + f.nano.calls, 0);
    asngn_task_record_free(r);
    eng_drop(&f);
  }
}

TEST(read_only_errors_and_torn_tail) {
  eng_fx f;
  asngn_turn_state t;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_OK(begin(&f, &t));
  asngn_task_record *r = NULL;
  char absent[37];
  asngn_uuid_v4(absent);
  ASSERT_ERR(asngn_session_task_read(f.s, "../x", &r), ASNGN_ERR_INVALID);
  ASSERT_ERR(asngn_session_task_read(f.s, absent, &r), ASNGN_ERR_NOT_FOUND);
  f.s->busy = true;
  ASSERT_ERR(asngn_session_task_read(f.s, t.span_root, &r), ASNGN_ERR_BUSY);
  f.s->busy = false;
  uint64_t before, after;
  ASSERT_OK(os_file_size(f.s->journal_st.path, &before));
  FILE *file = os_fopen(f.s->journal_st.path, "ab");
  ASSERT_TRUE(file != NULL);
  ASSERT_EQ_INT(fwrite("// torn", 1, 7, file), 7);
  ASSERT_EQ_INT(fclose(file), 0);
  ASSERT_ERR(asngn_session_task_read(f.s, t.span_root, &r), ASNGN_ERR_PARSE);
  ASSERT_TRUE(r == NULL);
  ASSERT_OK(os_file_size(f.s->journal_st.path, &after));
  ASSERT_EQ_INT(after, before + 7);
  ASSERT_TRUE(reopen(&f));
  ASSERT_OK(os_file_size(f.s->journal_st.path, &after));
  ASSERT_EQ_INT(after, before);
  ASSERT_OK(asngn_session_task_read(f.s, t.span_root, &r));
  asngn_task_record_free(r);
  eng_drop(&f);
}

TEST(uncertain_writes_block_further_frames) {
  const char *points[] = {"started", "action", "observed", "before_finished", "finished"};
  for (size_t i = 0; i < sizeof points / sizeof *points; i++) {
    eng_fx f;
    asngn_turn_state t;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    f.c->fault = fail_at;
    f.c->fault_ud = (void *)points[i];
    asngn_err e = begin(&f, &t);
    if (i > 0) {
      ASSERT_OK(e);
      asngn_uuid_v4(t.action_id);
      e = asngn_turn_journal(&t, "action", "intent");
    }
    if (i > 1) {
      ASSERT_OK(e);
      e = asngn_turn_journal(&t, "observed", "result");
    }
    if (i > 2) {
      ASSERT_OK(e);
      e = asngn_turn_finished(&t, ASNGN_ERR_CANCELLED);
    }
    ASSERT_ERR(e, ASNGN_ERR_IO);
    ASSERT_TRUE(f.s->recovery_required);
    uint64_t before, after;
    ASSERT_OK(os_file_size(f.s->journal_st.path, &before));
    ASSERT_ERR(asngn_turn_finished(&t, ASNGN_ERR_IO), ASNGN_ERR_IO);
    ASSERT_OK(os_file_size(f.s->journal_st.path, &after));
    ASSERT_EQ_INT(after, before);
    f.c->fault = NULL;
    ASSERT_TRUE(reopen(&f));
    asngn_task_record *r = NULL;
    ASSERT_OK(asngn_session_task_read(f.s, t.span_root, &r));
    ASSERT_EQ_INT(r->state, i == 4 ? ASNGN_TASK_FINISHED : ASNGN_TASK_INTERRUPTED);
    asngn_task_record_free(r);
    eng_drop(&f);
  }
}

TEST(corrupt_terminal_frames_fail_closed) {
  const char *tails[] = {"schema:1,outcome:\"ASNGN_OK\",committed:false,answer:\"\"",
                         "schema:1,outcome:\"ASNGN_ERR_IO\",committed:true,answer:\"\"",
                         "schema:1,outcome:\"success\",committed:false,answer:\"\"",
                         "schema:2,outcome:\"ASNGN_ERR_IO\",committed:false,answer:\"\"",
                         "schema:1,outcome:\"ASNGN_ERR_IO\",committed:false,answer:[]"};
  for (size_t i = 0; i < sizeof tails / sizeof *tails; i++) {
    eng_fx f;
    asngn_turn_state t;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    ASSERT_OK(begin(&f, &t));
    char frame[512];
    snprintf(frame, sizeof frame, "{id:\"%s\",state:\"finished\",%s}", t.span_root, tails[i]);
    ASSERT_OK(asngn_wal_append(f.c, &f.s->journal_st, frame, strlen(frame)));
    asngn_task_record *r = NULL;
    ASSERT_ERR(asngn_session_task_read(f.s, t.span_root, &r), ASNGN_ERR_PARSE);
    asngn_session_close(f.s);
    f.s = NULL;
    ASSERT_ERR(asngn_session_open(f.c, "s1", &f.s), ASNGN_ERR_PARSE);
    eng_drop(&f);
  }
}

TEST(oversized_mcp_evidence_is_not_truncated) {
  eng_fx f;
  asngn_turn_state t;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_OK(begin(&f, &t));
  char *large = malloc(3u * 1024u * 1024u + 1);
  ASSERT_TRUE(large != NULL);
  memset(large, 'x', 3u * 1024u * 1024u);
  large[3u * 1024u * 1024u] = 0;
  t.answer = large;
  ASSERT_OK(asngn_turn_finished(&t, ASNGN_ERR_LIMIT));
  asngn_task_record *r = NULL;
  ASSERT_OK(asngn_session_task_read(f.s, t.span_root, &r));
  ASSERT_EQ_STR(r->answer, large);
  asngn_task_record_free(r);
  asmodel_json_value *o = NULL;
  ASSERT_ERR(mcp_task_recover(f.s, t.span_root, &o), ASNGN_ERR_LIMIT);
  ASSERT_TRUE(o == NULL);
  free(large);
  eng_drop(&f);
}

#ifndef _WIN32
#include <sys/wait.h>
static int crash_at(void *ud, const char *point) {
  if (!strcmp(ud, point))
    _exit(77);
  return 0;
}
static int open_context(eng_fx *f) {
  asngn_open_params p = {.engine_root = f->root_raw, .config_path = f->engine_cfg};
  const char *ids[] = {"nano", "light", "std", "embed"};
  asngn_model_iface ifs[] = {fake_model_iface(&f->nano), fake_model_iface(&f->light),
                             fake_model_iface(&f->stdm), fake_model_iface(&f->embed)};
  asngn_clock ck = fake_clock_make(&f->clk);
  return asngn_open_with(&p, ifs, 4, ids, &ck, &f->c) == ASNGN_OK;
}
TEST(crash_before_and_after_terminal_sync) {
  for (int after = 0; after < 2; after++) {
    eng_fx f;
    asngn_turn_state t;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    ASSERT_OK(begin(&f, &t));
    ASSERT_OK(commit(&t));
    char id[37];
    memcpy(id, t.span_root, 37);
    asngn_session_close(f.s);
    f.s = NULL;
    asngn_close(f.c);
    f.c = NULL;
    pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (!child) {
      if (!open_context(&f) || asngn_session_open(f.c, "s1", &f.s) != ASNGN_OK)
        _exit(2);
      t.s = f.s;
      f.c->fault = crash_at;
      f.c->fault_ud = after ? "finished" : "before_finished";
      (void)asngn_turn_finished(&t, ASNGN_OK);
      _exit(3);
    }
    int status;
    ASSERT_EQ_INT(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ_INT(WEXITSTATUS(status), 77);
    ASSERT_TRUE(open_context(&f));
    ASSERT_OK(asngn_session_open(f.c, "s1", &f.s));
    asngn_task_record *r = NULL;
    ASSERT_OK(asngn_session_task_read(f.s, id, &r));
    ASSERT_EQ_INT(r->state, after ? ASNGN_TASK_FINISHED : ASNGN_TASK_COMMITTED);
    ASSERT_EQ_STR(r->answer, "An exact durable answer.\n");
    ASSERT_EQ_INT(r->turn_committed, 1);
    ASSERT_EQ_INT(f.s->log_n, 2);
    ASSERT_EQ_INT(f.s->led_n, 1);
    asngn_task_record_free(r);
    eng_drop(&f);
  }
}
#endif

TEST_LIST = {
    TEST_ENTRY(finished_task_survives_release_and_reopen),
    TEST_ENTRY(failed_and_cancelled_outcomes_are_terminal),
    TEST_ENTRY(action_observation_and_partial_output),
    TEST_ENTRY(read_only_errors_and_torn_tail),
    TEST_ENTRY(uncertain_writes_block_further_frames),
    TEST_ENTRY(corrupt_terminal_frames_fail_closed),
    TEST_ENTRY(oversized_mcp_evidence_is_not_truncated),
#ifndef _WIN32
    TEST_ENTRY(crash_before_and_after_terminal_sync),
#endif
};
RUN_ALL_TESTS()
