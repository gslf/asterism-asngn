/*
 * test_log.c — leveled file sink, callback fan-out, format, rotation.
 *
 * Uses a bare context with cfg.log_* set by hand; the log module only
 * touches log_mu / log_fp / log_size / log_cb, the clock and errbuf.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

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
  asngn_log_close(c);
  free(c->cfg.log_path);
  os_mutex_destroy(&c->err_mu);
  os_mutex_destroy(&c->log_mu);
  free(c);
}

static void cfg_log(asngn_ctx *c, const char *path) {
  c->cfg.log_path = asngn_strdup(path);
  c->cfg.log_level = ASNGN_LOG_INFO;
  c->cfg.log_max_size_kb = 1;
  c->cfg.log_max_files = 2;
  c->cfg.log_sync = false;
}

typedef struct {
  int n;
  int saw_info, saw_debug;
  char last[768];
} log_capture;

static void capture_cb(int level, const char *msg, void *ud) {
  log_capture *cap = (log_capture *)ud;
  cap->n++;
  if (level == ASNGN_LOG_INFO) cap->saw_info = 1;
  if (level == ASNGN_LOG_DEBUG) cap->saw_debug = 1;
  snprintf(cap->last, sizeof cap->last, "%s", msg);
}

/* "YYYY-MM-DDTHH:MM:SSZ" at the head of a record. */
static int stamp_ok(const char *line) {
  static const char *pat = "dddd-dd-ddTdd:dd:ddZ";
  int i;
  for (i = 0; i < 20; i++) {
    char p = pat[i], ch = line[i];
    if (ch == '\0') return 0;
    if (p == 'd') {
      if (ch < '0' || ch > '9') return 0;
    } else if (ch != p) {
      return 0;
    }
  }
  return 1;
}

TEST(log_gating_format_and_callback) {
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  log_capture cap;
  char *text;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/engine.log", dir);
  cfg_log(c, path);
  memset(&cap, 0, sizeof cap);
  c->log_cb = capture_cb; /* asngn_set_logger equivalent */
  c->log_ud = &cap;

  ASSERT_OK(asngn_log_open(c));
  asngn_log(c, ASNGN_LOG_INFO, "session", "hello info %d", 7);
  asngn_log(c, ASNGN_LOG_DEBUG, "cache", "debug hidden");
  asngn_log(c, ASNGN_LOG_INFO, "route", "line one\nline two");
  asngn_log_close(c);

  /* The callback sink sees every record at any level. */
  ASSERT_EQ_INT(cap.n, 3);
  ASSERT_TRUE(cap.saw_info);
  ASSERT_TRUE(cap.saw_debug);
  ASSERT_TRUE(strstr(cap.last, "line one line two") != NULL);
  ASSERT_TRUE(strchr(cap.last, '\n') == NULL);

  text = asngn_test_read_file(path, NULL);
  ASSERT_TRUE(text != NULL);
  /* File sink gated by cfg.log_level = INFO: DEBUG stays out. */
  ASSERT_TRUE(strstr(text, "hello info 7") != NULL);
  ASSERT_TRUE(strstr(text, "debug hidden") == NULL);
  ASSERT_TRUE(strstr(text, "DEBUG") == NULL);
  /* Embedded newline flattened to a space. */
  ASSERT_TRUE(strstr(text, "line one line two") != NULL);
  /* "<RFC3339> LEVEL SUBSYS  msg": stamp(20) ' ' %-5s ' ' %-8s ' '. */
  ASSERT_TRUE(stamp_ok(text));
  ASSERT_TRUE(text[20] == ' ');
  ASSERT_TRUE(strncmp(text + 21, "INFO ", 5) == 0);
  ASSERT_TRUE(text[26] == ' ');
  ASSERT_TRUE(strncmp(text + 27, "session ", 8) == 0);
  ASSERT_TRUE(text[35] == ' ');
  ASSERT_TRUE(strncmp(text + 36, "hello info 7\n", 13) == 0);
  free(text);

  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(log_rotation_keeps_max_files) {
  char dir[256], path[300], rot1[310], rot2[310];
  asngn_ctx *c = bare_ctx();
  int i;
  char *text;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/engine.log", dir);
  snprintf(rot1, sizeof rot1, "%s.1", path);
  snprintf(rot2, sizeof rot2, "%s.2", path);
  cfg_log(c, path); /* max_size_kb = 1, max_files = 2 */

  ASSERT_OK(asngn_log_open(c));
  for (i = 0; i < 40; i++)
    asngn_log(c, ASNGN_LOG_INFO, "session",
              "rotation filler line %02d ....................", i);
  asngn_log_close(c);

  ASSERT_TRUE(os_file_exists(path));
  ASSERT_TRUE(os_file_exists(rot1));  /* .1 appeared                 */
  ASSERT_TRUE(!os_file_exists(rot2)); /* overflow beyond max_files=2 */
  {
    uint64_t sz = 0;
    ASSERT_OK(os_file_size(rot1, &sz));
    ASSERT_TRUE(sz > 0 && sz <= 1024 + 128); /* about one rotation unit */
  }
  /* Both files still hold well-formed records. */
  text = asngn_test_read_file(rot1, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(stamp_ok(text));
  ASSERT_TRUE(strstr(text, "rotation filler line") != NULL);
  free(text);

  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(log_open_without_path_is_noop) {
  asngn_ctx *c = bare_ctx();
  ASSERT_TRUE(c != NULL);
  /* cfg.log_path NULL: open succeeds and no file sink is installed. */
  ASSERT_OK(asngn_log_open(c));
  ASSERT_TRUE(c->log_fp == NULL);
  asngn_log(c, ASNGN_LOG_ERROR, "session", "goes nowhere"); /* no crash */
  asngn_log_close(c);
  bare_ctx_free(c);
}

TEST_LIST = {
  TEST_ENTRY(log_gating_format_and_callback),
  TEST_ENTRY(log_rotation_keeps_max_files),
  TEST_ENTRY(log_open_without_path_is_noop),
};

RUN_ALL_TESTS()
