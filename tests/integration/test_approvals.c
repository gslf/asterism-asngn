#include "approval.h"
#include "asngn_test.h"
#include "engine_fx.h"

static int scripted_mutation(eng_fx *f) {
  return fake_model_push(&f->nano, "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n") &&
         fake_model_push(&f->light,
                         "{action:\"call\",why:\"change\",input:fake.mut {msg:\"review this "
                         "payload\"},success:\"observed\",fallback:\"report\"}\n") &&
         fake_model_push(&f->light, "{action:\"answer\"}\n") &&
         fake_model_push(&f->stdm, "Action result reported.\n");
}

static int fail_write(void *ud, const char *point) {
  (void)ud;
  return !strcmp(point, "stream_short_write");
}

#ifndef ASNGN_NO_THREADS
static asngn_approval *wait_pending(eng_fx *f, asngn_task *task) {
  for (int i = 0; i < 2000; i++) {
    asngn_approval *a = NULL;
    if (asngn_approval_get(f->s, &a) == ASNGN_OK && a->status == ASNGN_APPROVAL_PENDING) return a;
    asngn_approval_free(a);
    os_sleep_ms(1);
  }
  asngn_turn_result result = {0};
  asngn_err e = asngn_task_wait(task, 0, &result);
  fprintf(stderr, "missing approval: %s; %s; answer=%s; planner=%s\n", asngn_err_name(e),
          asngn_last_error(f->c), result.answer ? result.answer : "",
          f->light.last_user ? f->light.last_user : "");
  asngn_task_cancel(task);
  asngn_task_free(task);
  asngn_turn_result_free(&result);
  eng_drop(f);
  return NULL;
}

/* The durable record can become visible just before its wakeup slot is armed. */
static asngn_err decide(eng_fx *f, const char *id, int allow, int wide) {
  asngn_err e = ASNGN_ERR_NOT_FOUND;
  for (int i = 0; i < 1000 && e == ASNGN_ERR_NOT_FOUND; i++) {
    e = asngn_confirm(f->c, id, allow, wide);
    if (e == ASNGN_ERR_NOT_FOUND) os_sleep_ms(1);
  }
  return e;
}

TEST(approval_binds_payload_and_is_durable_before_execution) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(scripted_mutation(&f));
  asngn_task *task = NULL;
  ASSERT_OK(asngn_submit(f.s, "Run fake.mut", NULL, NULL, NULL, &task));
  asngn_approval *a = wait_pending(&f, task);
  ASSERT_TRUE(a != NULL);
  ASSERT_EQ_STR(a->tool_ref, "fake@1.0.0");
  ASSERT_CONTAINS(a->arguments, "review this payload");
  uint8_t hash[32];
  char hex[65];
  asngn_sha256(a->arguments, strlen(a->arguments), hash);
  asngn_sha256_hex(hash, 32, hex);
  ASSERT_EQ_STR(a->arguments_sha256, hex);
  ASSERT_TRUE(strlen(a->snapshot) == 64);
  ASSERT_OK(decide(&f, a->id, 1, 0));
  ASSERT_TRUE(asngn_confirm(f.c, a->id, 0, 0) != ASNGN_OK);
  asngn_turn_result r = {0};
  ASSERT_OK(asngn_task_wait(task, 3000, &r));
  asngn_task_free(task);
  asngn_approval_free(a);
  ASSERT_OK(asngn_approval_get(f.s, &a));
  ASSERT_EQ_INT(a->status, ASNGN_APPROVAL_CONSUMED);
  ASSERT_EQ_INT(a->sequence, 3);
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 1);
  asngn_session_close(f.s);
  f.s = NULL;
  ASSERT_OK(asngn_session_open(f.c, "s1", &f.s));
  asngn_approval *reopened = NULL;
  ASSERT_OK(asngn_approval_get(f.s, &reopened));
  ASSERT_EQ_STR(reopened->id, a->id);
  ASSERT_EQ_INT(reopened->status, ASNGN_APPROVAL_CONSUMED);
  asngn_approval_free(a);
  asngn_approval_free(reopened);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(changed_workspace_invalidates_an_approved_request) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(scripted_mutation(&f));
  asngn_task *task = NULL;
  ASSERT_OK(asngn_submit(f.s, "Run fake.mut", NULL, NULL, NULL, &task));
  asngn_approval *a = wait_pending(&f, task);
  ASSERT_TRUE(a != NULL);
  char path[512];
  snprintf(path, sizeof path, "%s/editor-change.txt", f.ws_raw);
  ASSERT_OK(os_write_file(path, "external edit", 13));
  ASSERT_OK(decide(&f, a->id, 1, 0));
  asngn_approval_free(a);
  asngn_turn_result r = {0};
  ASSERT_OK(asngn_task_wait(task, 3000, &r));
  asngn_task_free(task);
  ASSERT_OK(asngn_approval_get(f.s, &a));
  ASSERT_EQ_INT(a->status, ASNGN_APPROVAL_INVALIDATED);
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 0);
  ASSERT_CONTAINS(f.light.last_user, "asngn/approval-stale");
  asngn_approval_free(a);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}


TEST(failed_approval_write_never_releases_the_action) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(scripted_mutation(&f));
  asngn_task *task = NULL;
  ASSERT_OK(asngn_submit(f.s, "Run fake.mut", NULL, NULL, NULL, &task));
  asngn_approval *a = wait_pending(&f, task);
  ASSERT_TRUE(a != NULL);
  f.c->fault = fail_write;
  ASSERT_ERR(decide(&f, a->id, 1, 0), ASNGN_ERR_IO);
  asngn_turn_result r = {0};
  ASSERT_ERR(asngn_task_wait(task, 3000, &r), ASNGN_ERR_IO);
  asngn_task_free(task);
  f.c->fault = NULL;
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 0);
  asngn_approval_free(a);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

#endif

TEST(interrupted_approvals_reopen_without_replaying_tools) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_turn_state t = {
      .s = f.s, .gen_slot = 2, .security_profile = ASNGN_SECURITY_CODING_SANDBOXED};
  asngn_uuid_v4(t.span_root);
  ASSERT_OK(asngn_tools_select(f.c, &t, "fake mut"));
  int i = asngn_tools_find(&t, "fake", "mut");
  ASSERT_TRUE(i >= 0);
  char id[37], snapshot[65];
  ASSERT_OK(asngn_approval_prepare(&t, astools_selection_get(t.tool_selection, (size_t)i),
                                   "{msg:\"pending\"}", id, snapshot));
  asngn_turn_state_free(&t);
  asngn_session_close(f.s);
  f.s = NULL;
  ASSERT_OK(asngn_session_open(f.c, "s1", &f.s));
  asngn_approval *a = NULL;
  ASSERT_OK(asngn_approval_get(f.s, &a));
  ASSERT_EQ_STR(a->id, id);
  ASSERT_EQ_INT(a->status, ASNGN_APPROVAL_INTERRUPTED);
  ASSERT_ERR(asngn_confirm(f.c, id, 1, 0), ASNGN_ERR_NOT_FOUND);
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 0);
  asngn_approval_free(a);
  eng_drop(&f);
}

#ifndef ASNGN_NO_THREADS
TEST(approval_contains_the_expanded_artifact) {
  eng_fx f;
  ASSERT_TRUE(eng_setup_tool(&f, "fs", "echo", NULL));
  ASSERT_TRUE(
      fake_model_push(&f.nano, "CLASS COMPLEX | DETAIL NORMAL | MODE PLAN | TASK GENERATE\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action:\"call\",why:\"write\",input:fs.write "
      "{path:\"main.c\",content:\"@asngn:draft\"},success:\"written\",fallback:\"report\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "int main(void) { return 0; }\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action:\"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Write result received; tests have not run.\n"));
  asngn_task *task = NULL;
  ASSERT_OK(asngn_submit(f.s, "write a C program", NULL, NULL, NULL, &task));
  asngn_approval *a = wait_pending(&f, task);
  ASSERT_TRUE(a != NULL);
  ASSERT_CONTAINS(a->arguments, "int main");
  ASSERT_NOT_CONTAINS(a->arguments, "@asngn:draft");
  ASSERT_OK(decide(&f, a->id, 1, 0));
  asngn_turn_result r = {0};
  ASSERT_OK(asngn_task_wait(task, 3000, &r));
  asngn_task_free(task);
  asngn_approval_free(a);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(session_grants_cannot_authorize_a_replaced_package) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(scripted_mutation(&f));
  asngn_task *task = NULL;
  ASSERT_OK(asngn_submit(f.s, "Run fake.mut", NULL, NULL, NULL, &task));
  asngn_approval *a = wait_pending(&f, task);
  ASSERT_TRUE(a != NULL);
  ASSERT_OK(decide(&f, a->id, 1, 1));
  asngn_turn_result r = {0};
  ASSERT_OK(asngn_task_wait(task, 3000, &r));
  asngn_task_free(task);
  asngn_turn_result_free(&r);
  ASSERT_TRUE(scripted_mutation(&f));
  ASSERT_OK(eng_turn(&f, "Run fake.mut", NULL, NULL, &r));
  asngn_turn_result_free(&r);
  ASSERT_TRUE(asngn_fix_registry(f.reg_raw, "fake", eng_tool_path(), "sleep"));
  ASSERT_EQ_INT(astools_registry_refresh(f.c->astools), ASTOOLS_OK);
  ASSERT_TRUE(scripted_mutation(&f));
  ASSERT_OK(asngn_submit(f.s, "Run fake.mut", NULL, NULL, NULL, &task));
  asngn_approval *changed = wait_pending(&f, task);
  ASSERT_TRUE(changed != NULL);
  ASSERT_TRUE(strcmp(a->package_sha256, changed->package_sha256));
  ASSERT_OK(decide(&f, changed->id, 0, 0));
  ASSERT_OK(asngn_task_wait(task, 3000, &r));
  asngn_task_free(task);
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 2);
  asngn_turn_result_free(&r);
  asngn_approval_free(a);
  asngn_approval_free(changed);
  eng_drop(&f);
}

#endif

TEST(reviewed_fields_cannot_change_during_a_decision) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_turn_state t = {
      .s = f.s, .gen_slot = 2, .security_profile = ASNGN_SECURITY_CODING_SANDBOXED};
  asngn_uuid_v4(t.span_root);
  ASSERT_OK(asngn_tools_select(f.c, &t, "fake mut"));
  int i = asngn_tools_find(&t, "fake", "mut");
  ASSERT_TRUE(i >= 0);
  char id[37], snapshot[65];
  ASSERT_OK(asngn_approval_prepare(&t, astools_selection_get(t.tool_selection, (size_t)i),
                                   "{msg:\"fixed request\"}", id, snapshot));
  asngn_approval *copy = NULL;
  ASSERT_OK(asngn_approval_get(f.s, &copy));
  copy->status = ASNGN_APPROVAL_APPROVED;
  copy->snapshot[0] = copy->snapshot[0] == 'a' ? 'b' : 'a';
  os_rwlock_wrlock(&f.s->lock);
  asngn_err e = asngn_approval_save(f.s, copy);
  os_rwlock_wrunlock(&f.s->lock);
  ASSERT_ERR(e, ASNGN_ERR_INVALID);
  asngn_approval_free(copy);
  ASSERT_OK(asngn_approval_get(f.s, &copy));
  ASSERT_EQ_INT(copy->sequence, 1);
  ASSERT_EQ_STR(copy->snapshot, snapshot);
  ASSERT_EQ_INT(copy->status, ASNGN_APPROVAL_PENDING);
  asngn_approval_free(copy);
  asngn_turn_state_free(&t);
  eng_drop(&f);
}

#ifdef ASNGN_NO_THREADS
typedef struct { eng_fx *f; int mode, calls; asngn_err decision; } sync_review;
static void review_now(const char *event, void *ud) {
  sync_review *r = ud;
  xcdn_document_t *doc = xcdn_parse_str(event,strlen(event),NULL);
  const xcdn_value_t *v = doc && doc->values_len == 1 ? doc->values[0]->value : NULL;
  const char *kind = asngn_xstr(asngn_xfield(v,"kind"));
  if (kind && !strcmp(kind,"confirm")) {
    asngn_approval *a = NULL;
    r->calls++;
    r->decision = asngn_approval_get(r->f->s,&a);
    if (r->decision == ASNGN_OK) {
      if (r->mode == 2) {
        char path[512]; snprintf(path,sizeof path,"%s/external.txt",r->f->ws_raw);
        r->decision = os_write_file(path,"change",6);
      }
      if (r->mode == 3) r->f->c->fault = fail_write;
      if (r->decision == ASNGN_OK) r->decision = asngn_confirm(r->f->c,a->id,r->mode != 1,0);
      asngn_approval_free(a);
    }
  }
  xcdn_document_free(doc);
}
TEST(synchronous_review_checks_decisions_before_dispatch) {
  const asngn_approval_status states[] = {ASNGN_APPROVAL_CONSUMED,ASNGN_APPROVAL_DENIED,
                                        ASNGN_APPROVAL_INVALIDATED};
  for (int mode = 0; mode < 4; mode++) {
    eng_fx f; ASSERT_TRUE(eng_setup(&f,"echo",NULL)); ASSERT_TRUE(scripted_mutation(&f));
    sync_review review = {.f=&f,.mode=mode}; asngn_set_event_sink(f.c,review_now,&review);
    asngn_task *task = NULL; asngn_turn_result result = {0};
    ASSERT_OK(asngn_submit(f.s,"Run fake.mut",NULL,NULL,NULL,&task));
    asngn_err expected = mode == 3 ? ASNGN_ERR_IO : ASNGN_OK;
    ASSERT_EQ_INT(asngn_task_wait(task,1,&result),expected);
    ASSERT_EQ_INT(review.calls,1); ASSERT_EQ_INT(review.decision,expected);
    asngn_task_free(task); f.c->fault = NULL;
    asngn_stats stats; ASSERT_OK(asngn_get_stats(f.c,&stats));
    ASSERT_EQ_INT(stats.tool_calls,mode == 0 ? 1 : 0);
    if (mode != 3) {
      asngn_approval *a = NULL; ASSERT_OK(asngn_approval_get(f.s,&a));
      ASSERT_EQ_INT(a->status,states[mode]); asngn_approval_free(a);
    }
    asngn_turn_result_free(&result); eng_drop(&f);
  }
}
TEST(synchronous_missing_decision_finishes_without_execution) {
  eng_fx f; ASSERT_TRUE(eng_setup(&f,"echo",NULL)); ASSERT_TRUE(scripted_mutation(&f));
  asngn_task *task = NULL; asngn_turn_result result = {0}; char id[37];
  ASSERT_OK(asngn_submit(f.s,"Run fake.mut",NULL,NULL,NULL,&task));
  memcpy(id,asngn_task_id(task),37);
  ASSERT_ERR(asngn_task_wait(task,1,&result),ASNGN_ERR_DENIED); asngn_task_free(task);
  asngn_approval *a = NULL; ASSERT_OK(asngn_approval_get(f.s,&a));
  ASSERT_EQ_INT(a->status,ASNGN_APPROVAL_INTERRUPTED); asngn_approval_free(a);
  asngn_stats stats; ASSERT_OK(asngn_get_stats(f.c,&stats)); ASSERT_EQ_INT(stats.tool_calls,0);
  asngn_task_record *record = NULL; ASSERT_OK(asngn_session_task_read(f.s,id,&record));
  ASSERT_EQ_INT(record->state,ASNGN_TASK_FINISHED); ASSERT_EQ_INT(record->outcome,ASNGN_ERR_DENIED);
  asngn_task_record_free(record); asngn_turn_result_free(&result); eng_drop(&f);
}
#endif

TEST_LIST = {
#ifndef ASNGN_NO_THREADS
  TEST_ENTRY(approval_binds_payload_and_is_durable_before_execution),
  TEST_ENTRY(session_grants_cannot_authorize_a_replaced_package),
  TEST_ENTRY(changed_workspace_invalidates_an_approved_request),
  TEST_ENTRY(failed_approval_write_never_releases_the_action),
  TEST_ENTRY(approval_contains_the_expanded_artifact),
#else
  TEST_ENTRY(synchronous_review_checks_decisions_before_dispatch),
  TEST_ENTRY(synchronous_missing_decision_finishes_without_execution),
#endif
  TEST_ENTRY(reviewed_fields_cannot_change_during_a_decision),
  TEST_ENTRY(interrupted_approvals_reopen_without_replaying_tools)
};
RUN_ALL_TESTS()
