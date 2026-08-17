/*
 * test_steps.c — asngn_step_parse over every form, good and
 * malformed, including the 300-byte UTF-8-boundary cap.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

/* Bare context: enough for asngn_seterr (errbuf + err_mu). */
static asngn_ctx *bare_ctx(void) {
  asngn_ctx *c = (asngn_ctx *)calloc(1, sizeof(asngn_ctx));
  if (c != NULL) os_mutex_init(&c->err_mu);
  return c;
}

static void bare_ctx_free(asngn_ctx *c) {
  if (c == NULL) return;
  os_mutex_destroy(&c->err_mu);
  free(c);
}

TEST(answer_plain) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "ANSWER", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_ANSWER);
  ASSERT_TRUE(st.text == NULL);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(answer_trimmed) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, " ANSWER  ", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_ANSWER);
  asngn_step_free(&st);
  ASSERT_OK(asngn_step_parse(c, "\tANSWER \r\n", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_ANSWER);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(open_handle) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "OPEN B3", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_OPEN);
  ASSERT_EQ_INT(st.blob_n, 3);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(think_note) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "THINK | note", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  ASSERT_EQ_STR(st.text, "note");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(think_flexible_spacing) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "THINK|note", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  ASSERT_EQ_STR(st.text, "note");
  asngn_step_free(&st);
  ASSERT_OK(asngn_step_parse(c, "THINK  |  note", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  ASSERT_EQ_STR(st.text, "note");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(recall_question) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "RECALL | q", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_RECALL);
  ASSERT_EQ_STR(st.text, "q");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(clarify_question) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "CLARIFY | q?", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_CLARIFY);
  ASSERT_EQ_STR(st.text, "q?");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(call_simple) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "CALL fs.read {path: \"a b\"}", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_CALL);
  ASSERT_EQ_STR(st.call_ref, "fs");
  ASSERT_EQ_STR(st.call_cmd, "read");
  ASSERT_EQ_STR(st.call_args, "{path: \"a b\"}"); /* braces inclusive */
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(call_versioned_ref_and_brace_in_string) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "CALL fs@1.2.0.write {content: \"}\"}", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_CALL);
  ASSERT_EQ_STR(st.call_ref, "fs@1.2.0"); /* command = after the LAST dot */
  ASSERT_EQ_STR(st.call_cmd, "write");
  ASSERT_EQ_STR(st.call_args, "{content: \"}\"}");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(think_capped_on_utf8_boundary) {
  /* Payload "a" + 150×"è" = 301 bytes; the 300-byte cap falls on the
   * continuation byte of the 150th "è", so the parser backs off to 299. */
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  char line[400];
  size_t off, i;
  ASSERT_TRUE(c != NULL);
  memcpy(line, "THINK | a", 9);
  off = 9;
  for (i = 0; i < 150; i++) {
    line[off++] = '\xC3';
    line[off++] = '\xA8';
  }
  line[off] = '\0';
  ASSERT_OK(asngn_step_parse(c, line, &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  ASSERT_TRUE(st.text != NULL);
  ASSERT_EQ_INT((long long)strlen(st.text), 299); /* "a" + 149 full "è" */
  ASSERT_TRUE(asngn_utf8_valid(st.text, strlen(st.text)));
  ASSERT_TRUE(st.text[297] == '\xC3' && st.text[298] == '\xA8');
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(malformed_lines_are_protocol_errors) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_ERR(asngn_step_parse(c, "", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "OPEN Bx", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "OPEN B0", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "THINK |", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "CALLfs.read {}", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "CALL noargs", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "CALL fs.read {unbalanced", &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_TRUE(asngn_last_error(c)[0] != '\0'); /* seterr reached errbuf */
  ASSERT_ERR(asngn_step_parse(c, NULL, &st), ASNGN_ERR_INVALID);
  bare_ctx_free(c);
}

TEST_LIST = {
  TEST_ENTRY(answer_plain),
  TEST_ENTRY(answer_trimmed),
  TEST_ENTRY(open_handle),
  TEST_ENTRY(think_note),
  TEST_ENTRY(think_flexible_spacing),
  TEST_ENTRY(recall_question),
  TEST_ENTRY(clarify_question),
  TEST_ENTRY(call_simple),
  TEST_ENTRY(call_versioned_ref_and_brace_in_string),
  TEST_ENTRY(think_capped_on_utf8_boundary),
  TEST_ENTRY(malformed_lines_are_protocol_errors),
};

RUN_ALL_TESTS()
