/*
 * test_stream.c — append streams, torn-tail repair, atomic replace.
 *
 * Uses a bare context: the stream module only touches errbuf/err_mu and
 * the log fields (all NULL/zero from calloc) plus the clock.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

static asngn_ctx *bare_ctx(void) {
  asngn_ctx *c = (asngn_ctx *)calloc(1, sizeof *c);
  if (c == NULL) return NULL;
  os_mutex_init(&c->err_mu);
  os_mutex_init(&c->log_mu);
  c->clock = asngn_clock_system();
  return c;
}

static void bare_ctx_free(asngn_ctx *c) {
  if (c == NULL) return;
  os_mutex_destroy(&c->err_mu);
  os_mutex_destroy(&c->log_mu);
  free(c);
}

TEST(stream_append_then_load) {
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  asngn_stream st;
  xcdn_document_t *doc = NULL;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/turns.xcdn", dir);

  ASSERT_OK(asngn_stream_open(c, &st, path, false));
  ASSERT_OK(asngn_stream_append(c, &st, "#t {n: 1}", 9));
  ASSERT_OK(asngn_stream_append(c, &st, "#t {n: 2}", 9));
  ASSERT_OK(asngn_stream_append(c, &st, "#t {n: 3}", 9));
  asngn_stream_close(&st);

  ASSERT_OK(asngn_stream_load(c, path, "turns", &doc));
  ASSERT_TRUE(doc != NULL);
  ASSERT_EQ_INT((long long)doc->values_len, 3);
  xcdn_document_free(doc);

  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(stream_load_missing_file_is_ok_null) {
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  xcdn_document_t *doc = (xcdn_document_t *)&doc; /* poison */
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/absent.xcdn", dir);
  ASSERT_OK(asngn_stream_load(c, path, "absent", &doc));
  ASSERT_TRUE(doc == NULL);
  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(stream_torn_tail_truncates) {
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  asngn_stream st;
  xcdn_document_t *doc = NULL;
  uint64_t size_good = 0, size_torn = 0, size_after = 0;
  FILE *f;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/turns.xcdn", dir);

  ASSERT_OK(asngn_stream_open(c, &st, path, false));
  ASSERT_OK(asngn_stream_append(c, &st, "#t {n: 1}", 9));
  ASSERT_OK(asngn_stream_append(c, &st, "#t {n: 2}", 9));
  asngn_stream_close(&st);
  ASSERT_OK(os_file_size(path, &size_good));

  /* Simulate a crash mid-append: a partial value, no newline. */
  f = fopen(path, "ab");
  ASSERT_TRUE(f != NULL);
  ASSERT_EQ_INT((long long)fwrite("#t {n: 3", 1, 8, f), 8);
  fclose(f);
  ASSERT_OK(os_file_size(path, &size_torn));
  ASSERT_EQ_INT((long long)size_torn, (long long)(size_good + 8));

  /* Torn-tail rule: load succeeds with the two good values and the file
   * is truncated back to the last good offset. */
  ASSERT_OK(asngn_stream_load(c, path, "turns", &doc));
  ASSERT_TRUE(doc != NULL);
  ASSERT_EQ_INT((long long)doc->values_len, 2);
  xcdn_document_free(doc);
  doc = NULL;
  ASSERT_OK(os_file_size(path, &size_after));
  ASSERT_EQ_INT((long long)size_after, (long long)size_good);

  /* Reload of the repaired file is clean. */
  ASSERT_OK(asngn_stream_load(c, path, "turns", &doc));
  ASSERT_TRUE(doc != NULL);
  ASSERT_EQ_INT((long long)doc->values_len, 2);
  xcdn_document_free(doc);

  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(stream_midfile_corruption_is_fatal) {
  static const char *body = "#t {n: 1}\n{{{{\n#t {n: 3}\n";
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  xcdn_document_t *doc = NULL;
  uint64_t size_before = 0, size_after = 0;
  FILE *f;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/turns.xcdn", dir);

  f = fopen(path, "wb");
  ASSERT_TRUE(f != NULL);
  ASSERT_EQ_INT((long long)fwrite(body, 1, strlen(body), f),
                (long long)strlen(body));
  fclose(f);
  ASSERT_OK(os_file_size(path, &size_before));

  ASSERT_ERR(asngn_stream_load(c, path, "turns", &doc), ASNGN_ERR_PARSE);
  ASSERT_TRUE(doc == NULL);
  ASSERT_OK(os_file_size(path, &size_after));
  ASSERT_EQ_INT((long long)size_after, (long long)size_before);

  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(write_atomic_replaces_without_tmp) {
  char dir[256], path[300], tmp[310];
  asngn_ctx *c = bare_ctx();
  char *text;
  size_t len = 0;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/atomic.txt", dir);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);

  ASSERT_OK(asngn_write_atomic(c, path, "first", 5));
  text = asngn_test_read_file(path, &len);
  ASSERT_TRUE(text != NULL);
  ASSERT_EQ_STR(text, "first");
  free(text);

  ASSERT_OK(asngn_write_atomic(c, path, "second", 6));
  text = asngn_test_read_file(path, &len);
  ASSERT_TRUE(text != NULL);
  ASSERT_EQ_STR(text, "second");
  ASSERT_EQ_INT((long long)len, 6);
  free(text);

  ASSERT_TRUE(!os_file_exists(tmp)); /* no .tmp left behind */

  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST_LIST = {
  TEST_ENTRY(stream_append_then_load),
  TEST_ENTRY(stream_load_missing_file_is_ok_null),
  TEST_ENTRY(stream_torn_tail_truncates),
  TEST_ENTRY(stream_midfile_corruption_is_fatal),
  TEST_ENTRY(write_atomic_replaces_without_tmp),
};

RUN_ALL_TESTS()
