/* Acceptance replay cannot normalize ambiguous records into executable criteria. */
#include "asngn_test.h"
#include "work_state.h"

typedef struct {
  char dir[256], workspace[256];
  asngn_ctx *ctx;
  asngn_session session;
} fixture;
static bool setup(fixture *f) {
  memset(f, 0, sizeof *f);
  if (!asngn_test_tmpdir(f->dir) || !asngn_test_tmpdir(f->workspace))
    return false;
  f->ctx = calloc(1, sizeof *f->ctx);
  if (!f->ctx)
    return false;
  os_mutex_init(&f->ctx->err_mu);
  os_mutex_init(&f->ctx->log_mu);
  f->ctx->clock = asngn_clock_system();
  f->session.ctx = f->ctx;
  f->session.dir = f->dir;
  os_rwlock_init(&f->session.lock);
  snprintf(f->session.workspace.canonical_root, ASNGN_WORKSPACE_PATH_MAX, "%s", f->workspace);
  asngn_work_definition d = {0};
  strcpy(d.goal, "Fix the observed defect");
  d.count = 1;
  strcpy(d.criteria[0].id, "check");
  strcpy(d.criteria[0].requirement, "before-after");
  strcpy(d.criteria[0].command, "test");
  strcpy(d.criteria[0].adapter, "cmake");
  strcpy(d.criteria[0].path, ".");
  return asngn_work_load(&f->session) == ASNGN_OK &&
         asngn_session_work_define(&f->session, 0, &d) == ASNGN_OK;
}
static void drop(fixture *f) {
  asngn_work_free(f->session.work);
  os_rwlock_destroy(&f->session.lock);
  os_mutex_destroy(&f->ctx->err_mu);
  os_mutex_destroy(&f->ctx->log_mu);
  free(f->ctx);
  asngn_test_rmtree(f->dir);
  asngn_test_rmtree(f->workspace);
}
static asngn_err encode(xcdn_value_t *value, asngn_buf *b) {
  xcdn_node_t *node = value ? xcdn_node_new(value) : NULL;
  if (!node) {
    xcdn_value_free(value);
    return ASNGN_ERR_NOMEM;
  }
  asngn_err e = asngn_xnode_write(node, false, b);
  xcdn_node_free(node);
  return e;
}
static asngn_err replace(fixture *f, const char *path, const asngn_buf *b) {
  asngn_work_free(f->session.work);
  f->session.work = NULL;
  asngn_err e = os_write_file(path, "", 0);
  asngn_stream stream = {0};
  if (e == ASNGN_OK)
    e = asngn_stream_open(f->ctx, &stream, path, true);
  if (e == ASNGN_OK)
    e = asngn_wal_append(f->ctx, &stream, b->data, b->len);
  asngn_stream_close(&stream);
  return e;
}
TEST(acceptance_replay_rejects_duplicate_lossy_and_multivalue_frames) {
  for (int mode = 0; mode < 4; mode++) {
    fixture f;
    ASSERT_TRUE(setup(&f));
    asngn_work_state state = f.session.work->state;
    char *path = os_path_join(f.dir, "work.xcdn");
    ASSERT_TRUE(path != NULL);
    xcdn_value_t *v = asngn_work_encode(&state);
    ASSERT_TRUE(v != NULL);
    if (mode == 0)
      ASSERT_TRUE(asngn_xobj_put(v, "schema", xcdn_value_int(1)));
    if (mode == 1) {
      xcdn_value_t *row = asngn_xfield(v, "criteria")->data.array.items[0]->value;
      ASSERT_TRUE(asngn_xobj_put(row, "status", xcdn_value_string("not_run")));
    }
    asngn_buf record = {0};
    ASSERT_OK(encode(v, &record));
    if (mode == 2) {
      state.sequence++;
      ASSERT_OK(asngn_buf_appendc(&record, '\n'));
      ASSERT_OK(encode(asngn_work_encode(&state), &record));
    }
    if (mode == 3) {
      char *at = strstr(record.data, "before-after");
      ASSERT_TRUE(at != NULL);
      at += strlen("before");
      asngn_buf changed = {0};
      ASSERT_OK(asngn_buf_append(&changed, record.data, (size_t)(at - record.data)));
      ASSERT_OK(asngn_buf_appends(&changed, "\\u0000"));
      ASSERT_OK(asngn_buf_appends(&changed, at + 1));
      asngn_buf_free(&record);
      record = changed;
    }
    ASSERT_OK(replace(&f, path, &record));
    asngn_err e = asngn_work_load(&f.session);
    bool hidden = f.session.work == NULL;
    free(path);
    asngn_buf_free(&record);
    drop(&f);
    ASSERT_EQ_INT(e, ASNGN_ERR_PARSE);
    ASSERT_TRUE(hidden);
  }
}
TEST(invalid_acceptance_transition_leaves_later_torn_bytes_intact) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  asngn_work_state next = f.session.work->state;
  next.sequence++;
  strcpy(next.definition.goal, "Changed without a new revision");
  asngn_buf record = {0};
  ASSERT_OK(encode(asngn_work_encode(&next), &record));
  asngn_stream *stream = &f.session.work->stream;
  ASSERT_OK(asngn_wal_append(f.ctx, stream, record.data, record.len));
  const char tail[] = "// asngn-wal-v2 ";
  ASSERT_EQ_INT(fwrite(tail, 1, sizeof tail - 1, stream->fp), sizeof tail - 1);
  ASSERT_OK(os_fsync(stream->fp));
  char *path = os_path_join(f.dir, "work.xcdn");
  ASSERT_TRUE(path != NULL);
  asngn_work_free(f.session.work);
  f.session.work = NULL;
  size_t before_n, after_n;
  char *before = asngn_test_read_file(path, &before_n);
  ASSERT_TRUE(before != NULL);
  asngn_err e = asngn_work_load(&f.session);
  char *after = asngn_test_read_file(path, &after_n);
  ASSERT_TRUE(after != NULL);
  bool same = before_n == after_n && !memcmp(before, after, before_n);
  free(before);
  free(after);
  free(path);
  asngn_buf_free(&record);
  drop(&f);
  ASSERT_EQ_INT(e, ASNGN_ERR_PARSE);
  ASSERT_TRUE(same);
}
static asngn_work_definition maximum_definition(void) {
  asngn_work_definition d = {0};
  memset(d.goal, 1, sizeof d.goal - 1);
  memset(d.constraints, 1, sizeof d.constraints - 1);
  d.count = ASNGN_WORK_CRITERIA_MAX;
  for (size_t i = 0; i < d.count; i++) {
    asngn_work_criterion *c = &d.criteria[i];
    snprintf(c->id, sizeof c->id, "check-%zu", i);
    memset(c->requirement, 1, sizeof c->requirement - 1);
    strcpy(c->path, ".");
    strcpy(c->command, "test");
    strcpy(c->adapter, "cmake");
    c->depends_on = (1u << i) - 1;
  }
  return d;
}
TEST(maximum_criteria_and_long_history_keep_the_latest_exact_state) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  asngn_work_definition definition = maximum_definition();
  ASSERT_OK(asngn_session_work_define(&f.session, 1, &definition));
  asngn_work_state state = f.session.work->state;
  for (int i = 0; i < 255; i++)
    ASSERT_OK(asngn_work_save(&f.session, &state));
  char *path = os_path_join(f.dir, "work.xcdn");
  ASSERT_TRUE(path != NULL);
  uint64_t size;
  ASSERT_OK(os_file_size(path, &size));
  ASSERT_TRUE(size > 8u * 1024u * 1024u);
  asngn_work_free(f.session.work);
  f.session.work = NULL;
  ASSERT_OK(asngn_work_load(&f.session));
  ASSERT_EQ_INT(f.session.work->state.sequence, 257);
  ASSERT_EQ_INT(f.session.work->state.revision, 2);
  ASSERT_EQ_INT(f.session.work->state.definition.count, ASNGN_WORK_CRITERIA_MAX);
  ASSERT_EQ_STR(f.session.work->state.definition.goal, definition.goal);
  for (size_t i = 0; i < definition.count; i++) {
    ASSERT_TRUE(asngn_work_criterion_equal(&f.session.work->state.definition.criteria[i],
                                           &definition.criteria[i]));
    ASSERT_EQ_INT(f.session.work->state.proofs[i].status, ASNGN_PROOF_NOT_RUN);
  }
  ASSERT_TRUE(!f.session.work->state.succeeded);
  free(path);
  drop(&f);
}
TEST(oversized_acceptance_frame_is_rejected_before_decoding) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  char *path = os_path_join(f.dir, "work.xcdn");
  ASSERT_TRUE(path != NULL);
  asngn_buf record = {0};
  char *large = malloc(ASNGN_WORK_FRAME_MAX + 1);
  ASSERT_TRUE(large != NULL);
  memset(large, ' ', ASNGN_WORK_FRAME_MAX + 1);
  ASSERT_OK(asngn_buf_append(&record, large, ASNGN_WORK_FRAME_MAX + 1));
  free(large);
  ASSERT_OK(replace(&f, path, &record));
  asngn_err e = asngn_work_load(&f.session);
  bool hidden = f.session.work == NULL;
  free(path);
  asngn_buf_free(&record);
  drop(&f);
  ASSERT_EQ_INT(e, ASNGN_ERR_LIMIT);
  ASSERT_TRUE(hidden);
}
TEST_LIST = {TEST_ENTRY(acceptance_replay_rejects_duplicate_lossy_and_multivalue_frames),
             TEST_ENTRY(invalid_acceptance_transition_leaves_later_torn_bytes_intact),
             TEST_ENTRY(maximum_criteria_and_long_history_keep_the_latest_exact_state),
             TEST_ENTRY(oversized_acceptance_frame_is_rejected_before_decoding)};
RUN_ALL_TESTS()
