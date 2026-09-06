/*
 * test_steps.c — asngn_step_parse over every action-object form,
 * valid and malformed, including lossless strings and strict byte limits.
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
  ASSERT_OK(asngn_step_parse(c, "{action: \"answer\"}", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_ANSWER);
  ASSERT_TRUE(st.text == NULL);
  ASSERT_TRUE(st.why == NULL);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(answer_trimmed) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, " {action: \"answer\"}  ", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_ANSWER);
  asngn_step_free(&st);
  ASSERT_OK(asngn_step_parse(c, "\t{action: \"answer\"} \r\n", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_ANSWER);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(open_handle) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(
      c, "{action: \"open\", why: \"reread the blob\", input: {\"blob\":3,\"offset\":4096}}",
      &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_OPEN);
  ASSERT_EQ_INT(st.blob_n, 3);
  ASSERT_EQ_INT(st.blob_offset, 4096);
  ASSERT_EQ_STR(st.why, "reread the blob");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(think_note) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "{action: \"think\", input: \"note\"}",
                             &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  ASSERT_EQ_STR(st.text, "note");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(think_flexible_spacing) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(c, "{action:\"think\",input:\"note\"}", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  ASSERT_EQ_STR(st.text, "note");
  asngn_step_free(&st);
  ASSERT_OK(asngn_step_parse(
      c, "{ action : \"think\" ,  input :  \"note\" }", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  ASSERT_EQ_STR(st.text, "note");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(recall_full_schema) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(
      c,
      "{action: \"recall\", why: \"context needed\", input: \"q\", "
      "success: \"useful notes\", fallback: \"proceed without\"}",
      &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_RECALL);
  ASSERT_EQ_STR(st.text, "q");
  ASSERT_EQ_STR(st.why, "context needed");
  ASSERT_EQ_STR(st.success, "useful notes");
  ASSERT_EQ_STR(st.fallback, "proceed without");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(clarify_question) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(
      c, "{action: \"clarify\", why: \"ambiguo\", input: \"q?\"}", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_CLARIFY);
  ASSERT_EQ_STR(st.text, "q?");
  ASSERT_EQ_STR(st.why, "ambiguo");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(call_simple) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(
      c,
      "{action: \"call\", why: \"read the file\", input: fs.read "
      "{path: \"a b\"}, success: \"contenuto del file\", fallback: "
      "\"answer without it\"}",
      &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_CALL);
  ASSERT_EQ_STR(st.call_ref, "fs");
  ASSERT_EQ_STR(st.call_cmd, "read");
  ASSERT_EQ_STR(st.call_args, "{path: \"a b\"}"); /* braces inclusive */
  ASSERT_EQ_STR(st.why, "read the file");
  ASSERT_EQ_STR(st.success, "contenuto del file");
  ASSERT_EQ_STR(st.fallback, "answer without it");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(call_versioned_ref_and_brace_in_string) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(
      c,
      "{action: \"call\", why: \"w\", input: fs@1.2.0.write "
      "{content: \"}\"}, success: \"s\", fallback: \"f\"}",
      &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_CALL);
  ASSERT_EQ_STR(st.call_ref, "fs@1.2.0"); /* command = after the LAST dot */
  ASSERT_EQ_STR(st.call_cmd, "write");
  ASSERT_EQ_STR(st.call_args, "{content: \"}\"}");
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(oversized_utf8_is_rejected) {
  /* Reject a 2049-byte payload instead of changing the proposed action. */
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  char line[ASNGN_STEP_TEXT_MAX + 128];
  size_t off, i;
  ASSERT_TRUE(c != NULL);
  memcpy(line, "{action: \"think\", input: \"a", 27);
  off = 27;
  for (i = 0; i < ASNGN_STEP_TEXT_MAX / 2; i++) {
    line[off++] = '\xC3';
    line[off++] = '\xA8';
  }
  line[off++] = '"';
  line[off++] = '}';
  line[off] = '\0';
  ASSERT_ERR(asngn_step_parse(c, line, &st), ASNGN_ERR_PROTOCOL);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(oversized_metadata_is_rejected) {
  /* Metadata is either intact or rejected. */
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  char line[ASNGN_STEP_META_MAX + 128];
  size_t off, i;
  ASSERT_TRUE(c != NULL);
  memcpy(line, "{action: \"clarify\", why: \"", 26);
  off = 26;
  for (i = 0; i < ASNGN_STEP_META_MAX + 32; i++) line[off++] = 'x';
  memcpy(line + off, "\", input: \"q\"}", 14);
  line[off + 14] = '\0';
  ASSERT_ERR(asngn_step_parse(c, line, &st), ASNGN_ERR_PROTOCOL);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(clarify_allows_protocol_words_as_content) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_OK(asngn_step_parse(
      c, "{action: \"clarify\", why: \"w\", input: \"Should I call the "
         "API?\"}", &st));
  asngn_step_free(&st);
  ASSERT_OK(asngn_step_parse(
      c, "{action: \"clarify\", why: \"w\", input: \"Use answer as the "
         "label?\"}", &st));
  asngn_step_free(&st);
  /* ordinary questions are untouched */
  ASSERT_OK(asngn_step_parse(
      c, "{action: \"clarify\", why: \"w\", input: \"Which file should "
         "I edit?\"}", &st));
  ASSERT_EQ_STR(st.text, "Which file should I edit?");
  asngn_step_free(&st);
  /* think notes stay internal and permissive */
  ASSERT_OK(asngn_step_parse(
      c, "{action: \"think\", input: \"next step: answer\"}", &st));
  ASSERT_EQ_INT(st.kind, ASNGN_STEP_THINK);
  asngn_step_free(&st);
  bare_ctx_free(c);
}

TEST(malformed_objects_are_protocol_errors) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  ASSERT_TRUE(c != NULL);
  ASSERT_ERR(asngn_step_parse(c, "", &st), ASNGN_ERR_PROTOCOL);
  /* the old line protocol is malformed now */
  ASSERT_ERR(asngn_step_parse(c, "ANSWER", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "THINK | note", &st), ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "CALL fs.read {}", &st),
             ASNGN_ERR_PROTOCOL);
  /* unknown action */
  ASSERT_ERR(asngn_step_parse(c, "{action: \"dance\"}", &st),
             ASNGN_ERR_PROTOCOL);
  /* action must come first */
  ASSERT_ERR(asngn_step_parse(c, "{why: \"w\", action: \"answer\"}", &st),
             ASNGN_ERR_PROTOCOL);
  /* missing / empty / oversized-shape fields */
  ASSERT_ERR(asngn_step_parse(c, "{action: \"think\"}", &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "{action: \"think\", input: \"\"}", &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(
                 c, "{action: \"clarify\", input: \"q\"}", &st),
             ASNGN_ERR_PROTOCOL); /* clarify without why */
  ASSERT_ERR(asngn_step_parse(
                 c, "{action: \"answer\", why: \"w\"}", &st),
             ASNGN_ERR_PROTOCOL); /* answer takes no other fields */
  ASSERT_ERR(asngn_step_parse(
                 c,
                 "{action: \"call\", why: \"w\", input: fs.read {}, "
                 "success: \"s\"}",
                 &st),
             ASNGN_ERR_PROTOCOL); /* call without fallback */
  /* structural breakage */
  ASSERT_ERR(asngn_step_parse(
                 c, "{action: \"open\", why: \"w\", input: \"Bx\"}", &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(
                 c, "{action: \"open\", why: \"w\", input: {\"blob\":0,\"offset\":0}}", &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(
                 c,
                 "{action: \"call\", why: \"w\", input: noargs, success: "
                 "\"s\", fallback: \"f\"}",
                 &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(
                 c,
                 "{action: \"call\", why: \"w\", input: fs.read "
                 "{unbalanced, success: \"s\", fallback: \"f\"}",
                 &st),
             ASNGN_ERR_PROTOCOL);
  /* duplicate key, unknown key, escapes, trailing junk */
  ASSERT_ERR(asngn_step_parse(
                 c, "{action: \"think\", input: \"a\", input: \"b\"}",
                 &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(
                 c, "{action: \"think\", input: \"a\", mood: \"b\"}", &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_OK(asngn_step_parse(c, "{action: \"think\", input: \"a\\\"b\"}", &st));
  ASSERT_EQ_STR(st.text, "a\"b");
  asngn_step_free(&st);
  ASSERT_ERR(asngn_step_parse(
                 c, "{action: \"answer\"} extra", &st),
             ASNGN_ERR_PROTOCOL);
  ASSERT_ERR(asngn_step_parse(c, "{action: \"answer\"", &st),
             ASNGN_ERR_PROTOCOL); /* unterminated object */
  ASSERT_TRUE(asngn_last_error(c)[0] != '\0'); /* seterr reached errbuf */
  ASSERT_ERR(asngn_step_parse(c, NULL, &st), ASNGN_ERR_INVALID);
  bare_ctx_free(c);
}

TEST(provider_json_is_decoded_without_changing_arguments) {
  asngn_ctx *c = bare_ctx();
  asngn_step st;
  char *text = asngn_strdup("{\"action\":\"call\",\"why\":\"read \\\"quoted\\\" path\","
      "\"tool\":\"fs.read\",\"arguments\":{\"path\":\"src/è.c\"},"
      "\"success\":\"observed\",\"fallback\":\"search\"}");
  const char *schema = "{\"oneOf\":[{\"properties\":{\"action\":{\"const\":\"call\"},"
      "\"tool\":{\"const\":\"fs.read\"},\"arguments\":{\"type\":\"object\"}}}]}";
  ASSERT_OK(asngn_protocol_decode(ASNGN_TASK_DECIDE, schema, &text));
  ASSERT_OK(asngn_step_parse(c, text, &st));
  ASSERT_EQ_STR(st.why, "read \"quoted\" path");
  ASSERT_EQ_STR(st.call_args, "{\"path\":\"src/è.c\"}");
  asngn_step_free(&st); free(text);
  text = asngn_strdup("{\"action\":\"answer\",\"action\":\"think\"}");
  ASSERT_ERR(asngn_protocol_decode(ASNGN_TASK_DECIDE, schema, &text), ASNGN_ERR_PROTOCOL);
  free(text);
  text = asngn_strdup("{\"score\":0,\"critique\":\"uncertain\"}");
  ASSERT_OK(asngn_protocol_decode(ASNGN_TASK_JUDGE, NULL, &text));
  ASSERT_EQ_STR(text,"SCORE 0 | uncertain\n"); free(text);
  bare_ctx_free(c);
}

TEST_LIST = {
  TEST_ENTRY(provider_json_is_decoded_without_changing_arguments),
  TEST_ENTRY(answer_plain),
  TEST_ENTRY(answer_trimmed),
  TEST_ENTRY(open_handle),
  TEST_ENTRY(think_note),
  TEST_ENTRY(think_flexible_spacing),
  TEST_ENTRY(recall_full_schema),
  TEST_ENTRY(clarify_question),
  TEST_ENTRY(call_simple),
  TEST_ENTRY(call_versioned_ref_and_brace_in_string),
  TEST_ENTRY(oversized_utf8_is_rejected),
  TEST_ENTRY(oversized_metadata_is_rejected),
  TEST_ENTRY(clarify_allows_protocol_words_as_content),
  TEST_ENTRY(malformed_objects_are_protocol_errors),
};

RUN_ALL_TESTS()
