/* A valid checksum does not make an ambiguous approval record admissible. */
#include "approval.h"
#include "asngn_test.h"
#include "engine_fx.h"

static asngn_approval *review(eng_fx *f) {
  asngn_turn_state t = {.s = f->s, .security_profile = ASNGN_SECURITY_CODING_SANDBOXED};
  asngn_uuid_v4(t.span_root);
  asngn_approval *a = NULL;
  asngn_err e = asngn_tools_select(f->c, &t, "fake mut");
  int i = e == ASNGN_OK ? asngn_tools_find(&t, "fake", "mut") : -1;
  char id[37], snapshot[65];
  if (i >= 0)
    e = asngn_approval_prepare(&t, astools_selection_get(t.tool_selection, (size_t)i),
                               "{msg:\"review α\"}", id, snapshot);
  if (i >= 0 && e == ASNGN_OK)
    (void)asngn_approval_get(f->s, &a);
  asngn_turn_state_free(&t);
  return a;
}
static asngn_err encode(xcdn_value_t *value, asngn_buf *buffer) {
  xcdn_node_t *node = value ? xcdn_node_new(value) : NULL;
  if (!node) {
    xcdn_value_free(value);
    return ASNGN_ERR_NOMEM;
  }
  asngn_err e = asngn_xnode_write(node, false, buffer);
  xcdn_node_free(node);
  return e;
}
static asngn_err replace(eng_fx *f, const char *path, const asngn_buf *record) {
  asngn_session_close(f->s);
  f->s = NULL;
  asngn_err e = os_write_file(path, "", 0);
  asngn_stream stream = {0};
  if (e == ASNGN_OK)
    e = asngn_stream_open(f->c, &stream, path, true);
  if (e == ASNGN_OK)
    e = asngn_wal_append(f->c, &stream, record->data, record->len);
  asngn_stream_close(&stream);
  return e;
}
static asngn_err append_torn_tail(asngn_stream *stream) {
  const char tail[] = "// asngn-wal-v2 ";
  /* Bypass the normal writer's newline to model an incomplete physical write. */
  if (fwrite(tail, 1, sizeof tail - 1, stream->fp) != sizeof tail - 1 || fflush(stream->fp))
    return ASNGN_ERR_IO;
  return os_fsync(stream->fp);
}
TEST(complete_approval_frames_reject_unknown_and_duplicate_fields) {
  for (int mode = 0; mode < 4; mode++) {
    eng_fx f;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    asngn_approval *a = review(&f);
    ASSERT_TRUE(a != NULL);
    char *path = os_path_join(f.s->dir, "approvals.xcdn");
    ASSERT_TRUE(path != NULL);
    xcdn_value_t *value = asngn_approval_encode(a);
    ASSERT_TRUE(value != NULL);
    if (mode == 0)
      ASSERT_TRUE(asngn_xobj_put(value, "unknown", xcdn_value_bool(true)));
    if (mode == 1)
      ASSERT_TRUE(asngn_xobj_put(value, "status", xcdn_value_int(ASNGN_APPROVAL_APPROVED)));
    if (mode == 3)
      ASSERT_TRUE(asngn_xobj_put(value, "status", xcdn_value_int(ASNGN_APPROVAL_PENDING)));
    if (mode == 2) {
      /* Replace a known key with a duplicate without changing the field count. */
      free(value->data.object.entries[4].key);
      value->data.object.entries[4].key = asngn_strdup("status");
      ASSERT_TRUE(value->data.object.entries[4].key != NULL);
    }
    asngn_buf record = {0};
    ASSERT_OK(encode(value, &record));
    ASSERT_OK(replace(&f, path, &record));
    size_t before_size, after_size;
    char *before = asngn_test_read_file(path, &before_size);
    ASSERT_TRUE(before != NULL);
    asngn_err e = asngn_session_open(f.c, "s1", &f.s);
    if (e != ASNGN_ERR_PARSE) {
      /* A negative reproduction must join workers before returning from the fixture. */
      free(before);
      free(path);
      asngn_buf_free(&record);
      asngn_approval_free(a);
      eng_drop(&f);
      ASSERT_EQ_INT(e, ASNGN_ERR_PARSE);
    }
    ASSERT_EQ_INT(e, ASNGN_ERR_PARSE);
    ASSERT_TRUE(f.s == NULL);
    char *after = asngn_test_read_file(path, &after_size);
    ASSERT_TRUE(after != NULL);
    ASSERT_EQ_INT(before_size, after_size);
    ASSERT_TRUE(!memcmp(before, after, before_size));
    free(before);
    free(after);
    free(path);
    asngn_buf_free(&record);
    asngn_approval_free(a);
    eng_drop(&f);
  }
}
TEST(one_checked_frame_cannot_hide_two_approval_transitions) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_approval *a = review(&f);
  ASSERT_TRUE(a != NULL);
  char *path = os_path_join(f.s->dir, "approvals.xcdn");
  ASSERT_TRUE(path != NULL);
  asngn_buf record = {0};
  ASSERT_OK(encode(asngn_approval_encode(a), &record));
  ASSERT_OK(asngn_buf_appendc(&record, '\n'));
  a->status = ASNGN_APPROVAL_DENIED;
  a->sequence++;
  ASSERT_OK(encode(asngn_approval_encode(a), &record));
  ASSERT_OK(replace(&f, path, &record));
  asngn_err e = asngn_session_open(f.c, "s1", &f.s);
  if (e != ASNGN_ERR_PARSE) {
    free(path);
    asngn_buf_free(&record);
    asngn_approval_free(a);
    eng_drop(&f);
    ASSERT_EQ_INT(e, ASNGN_ERR_PARSE);
  }
  ASSERT_TRUE(f.s == NULL);
  free(path);
  asngn_buf_free(&record);
  asngn_approval_free(a);
  eng_drop(&f);
}
TEST(corrupt_transition_blocks_torn_tail_repair) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_approval *a = review(&f);
  ASSERT_TRUE(a != NULL);
  char *path = os_path_join(f.s->dir, "approvals.xcdn");
  ASSERT_TRUE(path != NULL);
  asngn_session_close(f.s);
  f.s = NULL;
  a->status = ASNGN_APPROVAL_APPROVED;
  a->sequence++;
  a->snapshot[0] = a->snapshot[0] == 'a' ? 'b' : 'a';
  asngn_buf record = {0};
  ASSERT_OK(encode(asngn_approval_encode(a), &record));
  asngn_stream stream;
  ASSERT_OK(asngn_stream_open(f.c, &stream, path, true));
  ASSERT_OK(asngn_wal_append(f.c, &stream, record.data, record.len));
  ASSERT_OK(append_torn_tail(&stream));
  asngn_stream_close(&stream);
  size_t before_size, after_size;
  char *before = asngn_test_read_file(path, &before_size);
  ASSERT_TRUE(before != NULL);
  ASSERT_ERR(asngn_session_open(f.c, "s1", &f.s), ASNGN_ERR_PARSE);
  ASSERT_TRUE(f.s == NULL);
  char *after = asngn_test_read_file(path, &after_size);
  ASSERT_TRUE(after != NULL);
  ASSERT_EQ_INT(before_size, after_size);
  ASSERT_TRUE(!memcmp(before, after, before_size));
  free(before);
  free(after);
  free(path);
  asngn_buf_free(&record);
  asngn_approval_free(a);
  eng_drop(&f);
}
TEST(frame_quota_and_maximum_escaped_review) {
  for (int mode = 0; mode < 3; mode++) {
    eng_fx f;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    asngn_approval *a = review(&f);
    ASSERT_TRUE(a != NULL);
    char *path = os_path_join(f.s->dir, "approvals.xcdn");
    ASSERT_TRUE(path != NULL);
    free(a->arguments);
    size_t n = ASNGN_APPROVAL_ARGS_MAX + (mode == 1);
    a->arguments = malloc(n + 1);
    ASSERT_TRUE(a->arguments != NULL);
    memset(a->arguments, 1, n);
    a->arguments[n] = 0;
    asngn_buf record = {0};
    if (mode == 2) {
      char *large = malloc(ASNGN_APPROVAL_FRAME_MAX + 1);
      ASSERT_TRUE(large != NULL);
      memset(large, 'x', ASNGN_APPROVAL_FRAME_MAX + 1);
      ASSERT_OK(asngn_buf_append(&record, large, ASNGN_APPROVAL_FRAME_MAX + 1));
      free(large);
    } else
      ASSERT_OK(encode(asngn_approval_encode(a), &record));
    ASSERT_OK(replace(&f, path, &record));
    asngn_err e = asngn_session_open(f.c, "s1", &f.s);
    ASSERT_EQ_INT(e, mode == 0 ? ASNGN_OK : mode == 1 ? ASNGN_ERR_PARSE : ASNGN_ERR_LIMIT);
    if (!mode) {
      asngn_approval *loaded = NULL;
      ASSERT_OK(asngn_approval_get(f.s, &loaded));
      ASSERT_EQ_INT(loaded->status, ASNGN_APPROVAL_INTERRUPTED);
      ASSERT_EQ_INT(loaded->sequence, 2);
      ASSERT_EQ_STR(loaded->arguments, a->arguments);
      asngn_approval_free(loaded);
    } else
      ASSERT_TRUE(f.s == NULL);
    free(path);
    asngn_buf_free(&record);
    asngn_approval_free(a);
    eng_drop(&f);
  }
}
TEST(long_approval_history_replays_latest_state_and_repairs_only_torn_tail) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_approval *a = review(&f);
  ASSERT_TRUE(a != NULL);
  char *path = os_path_join(f.s->dir, "approvals.xcdn");
  ASSERT_TRUE(path != NULL);
  asngn_session_close(f.s);
  f.s = NULL;
  ASSERT_OK(os_write_file(path, "", 0));
  free(a->arguments);
  a->arguments = malloc(128 * 1024 + 1);
  ASSERT_TRUE(a->arguments != NULL);
  memset(a->arguments, 'k', 128 * 1024);
  a->arguments[128 * 1024] = 0;
  asngn_stream stream;
  ASSERT_OK(asngn_stream_open(f.c, &stream, path, false));
  for (int i = 0; i < 256; i++) {
    a->sequence = (unsigned)i + 1;
    if (!(i % 2))
      asngn_uuid_v4(a->id);
    a->status = i % 2 ? ASNGN_APPROVAL_DENIED : ASNGN_APPROVAL_PENDING;
    asngn_buf record = {0};
    ASSERT_OK(encode(asngn_approval_encode(a), &record));
    ASSERT_OK(asngn_wal_append(f.c, &stream, record.data, record.len));
    asngn_buf_free(&record);
  }
  uint64_t good_size;
  ASSERT_OK(os_file_size(path, &good_size));
  ASSERT_TRUE(good_size > 32u * 1024u * 1024u);
  ASSERT_OK(append_torn_tail(&stream));
  ASSERT_OK(os_fsync(stream.fp));
  asngn_stream_close(&stream);
  ASSERT_OK(asngn_session_open(f.c, "s1", &f.s));
  asngn_approval *loaded = NULL;
  ASSERT_OK(asngn_approval_get(f.s, &loaded));
  ASSERT_EQ_INT(loaded->sequence, 256);
  ASSERT_EQ_STR(loaded->id, a->id);
  ASSERT_EQ_INT(loaded->status, ASNGN_APPROVAL_DENIED);
  ASSERT_EQ_STR(loaded->arguments, a->arguments);
  uint64_t after_size;
  ASSERT_OK(os_file_size(path, &after_size));
  ASSERT_EQ_INT(after_size, good_size);
  asngn_approval_free(loaded);
  free(path);
  asngn_approval_free(a);
  eng_drop(&f);
}
TEST(lossy_strings_and_unwritten_metadata_cannot_become_review_records) {
  for (int mode = 0; mode < 3; mode++) {
    eng_fx f;
    ASSERT_TRUE(eng_setup(&f, "echo", NULL));
    asngn_approval *a = review(&f);
    ASSERT_TRUE(a != NULL);
    char *path = os_path_join(f.s->dir, "approvals.xcdn");
    ASSERT_TRUE(path != NULL);
    free(a->arguments);
    a->arguments = asngn_strdup("before-after");
    ASSERT_TRUE(a->arguments != NULL);
    asngn_buf original = {0}, record = {0};
    ASSERT_OK(encode(asngn_approval_encode(a), &original));
    if (mode == 2) {
      ASSERT_OK(asngn_buf_appends(&record, "#unexpected "));
      ASSERT_OK(asngn_buf_append(&record, original.data, original.len));
    } else {
      char *at = strstr(original.data, "before-after");
      ASSERT_TRUE(at != NULL);
      at += strlen("before");
      ASSERT_OK(asngn_buf_append(&record, original.data, (size_t)(at - original.data)));
      if (mode == 0)
        ASSERT_OK(asngn_buf_appends(&record, "\\u0000"));
      else
        ASSERT_OK(asngn_buf_appendc(&record, (char)-1));
      ASSERT_OK(asngn_buf_appends(&record, at + 1));
    }
    ASSERT_OK(replace(&f, path, &record));
    asngn_err e = asngn_session_open(f.c, "s1", &f.s);
    free(path);
    asngn_buf_free(&original);
    asngn_buf_free(&record);
    asngn_approval_free(a);
    eng_drop(&f);
    ASSERT_EQ_INT(e, ASNGN_ERR_PARSE);
  }
}
TEST(invalid_review_encoding_never_enters_the_journal) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_turn_state t = {.s = f.s, .security_profile = ASNGN_SECURITY_CODING_SANDBOXED};
  asngn_uuid_v4(t.span_root);
  ASSERT_OK(asngn_tools_select(f.c, &t, "fake mut"));
  int i = asngn_tools_find(&t, "fake", "mut");
  ASSERT_TRUE(i >= 0);
  char id[37], snapshot[65];
  asngn_err e = asngn_approval_prepare(&t, astools_selection_get(t.tool_selection, (size_t)i),
                                       "{msg:\"\xff\"}", id, snapshot);
  asngn_turn_state_free(&t);
  asngn_approval *a = NULL;
  asngn_err got = asngn_approval_get(f.s, &a);
  uint64_t bytes = 1;
  asngn_err sized = os_file_size(f.s->approvals->stream.path, &bytes);
  asngn_approval_free(a);
  eng_drop(&f);
  ASSERT_EQ_INT(e, ASNGN_ERR_INVALID);
  ASSERT_EQ_INT(got, ASNGN_ERR_NOT_FOUND);
  ASSERT_OK(sized);
  ASSERT_EQ_INT(bytes, 0);
}
TEST_LIST = {TEST_ENTRY(complete_approval_frames_reject_unknown_and_duplicate_fields),
             TEST_ENTRY(one_checked_frame_cannot_hide_two_approval_transitions),
             TEST_ENTRY(corrupt_transition_blocks_torn_tail_repair),
             TEST_ENTRY(frame_quota_and_maximum_escaped_review),
             TEST_ENTRY(long_approval_history_replays_latest_state_and_repairs_only_torn_tail),
             TEST_ENTRY(lossy_strings_and_unwritten_metadata_cannot_become_review_records),
             TEST_ENTRY(invalid_review_encoding_never_enters_the_journal)};
RUN_ALL_TESTS()
