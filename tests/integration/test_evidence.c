/* Late diagnostics remain exact, bounded and directly reopenable. */
#include "asngn_test.h"
#include "blob.h"
#include "engine_fx.h"
#include "excerpt.h"
#include "native.h"

static char *log_text(size_t *len) {
  asngn_buf b = {0};
  asngn_buf_appends(&b, "begin\n");
  for (size_t i = 0; i < 7000; i++)
    asngn_buf_appends(&b, "ordinary progress αβγ 🌍\n");
  asngn_buf_appends(&b, "late.c:91:2: error: missing FINAL_DIAGNOSTIC\n1 tests failed\nend\n");
  *len = b.len;
  return asngn_buf_detach(&b);
}
TEST(late_diagnostic_is_exact_and_bounded) {
  size_t len = 0;
  char *text = log_text(&len);
  ASSERT_TRUE(text && len > 65536);
  asngn_excerpt view = {0};
  ASSERT_OK(asngn_excerpt_build(text, len, 4096, &view));
  ASSERT_CONTAINS(view.text, "FINAL_DIAGNOSTIC");
  ASSERT_TRUE(strlen(view.text) <= 4096);
  ASSERT_TRUE(view.matches >= 2 && view.count > 0);
  size_t union_bytes = 0;
  for (size_t i = 0; i < view.count; i++) {
    asngn_excerpt_span *s = &view.spans[i];
    ASSERT_TRUE(s->start < s->end && s->end <= len);
    if (i) ASSERT_TRUE(view.spans[i - 1].end < s->start);
    union_bytes += s->end - s->start;
    ASSERT_TRUE(asngn_utf8_valid(text + s->start, s->end - s->start));
    char *piece = asngn_strndup(text + s->start, s->end - s->start);
    ASSERT_CONTAINS(view.text, piece);
    free(piece);
  }
  ASSERT_EQ_INT(view.selected_bytes, union_bytes);
  char *trace = asngn_excerpt_trace(&view, "sha256:test", len, 4096, false);
  asmodel_json_value *meta = NULL;
  ASSERT_TRUE(trace);
  ASSERT_EQ_INT(asmodel_json_parse(trace, strlen(trace), &meta), 0);
  ASSERT_EQ_INT(asmodel_json_int_value(asmodel_json_object_get(meta, "selected_bytes")),
                union_bytes);
  ASSERT_EQ_INT(asmodel_json_array_len(asmodel_json_object_get(meta, "spans")), view.count);
  ASSERT_NOT_CONTAINS(trace, "FINAL_DIAGNOSTIC");
  asmodel_json_free(meta);
  free(trace);
  asngn_excerpt_free(&view);
  free(text);
}
TEST(first_and_latest_diagnostics_survive_pressure) {
  asngn_buf b = {0};
  for (int i = 0; i < 50; i++) {
    asngn_buf_printf(&b, "path.c:1: error: DIAGNOSTIC_%02d\n", i);
    for (int j = 0; j < 100; j++)
      asngn_buf_appends(&b, "padding line 🌍\n");
  }
  for (size_t budget = 512; budget <= 65536; budget *= 2) {
    asngn_excerpt view = {0};
    ASSERT_OK(asngn_excerpt_build(b.data, b.len, budget, &view));
    ASSERT_CONTAINS(view.text, "DIAGNOSTIC_00");
    ASSERT_CONTAINS(view.text, "DIAGNOSTIC_49");
    ASSERT_TRUE(strlen(view.text) <= budget && asngn_utf8_valid(view.text, strlen(view.text)));
    asngn_excerpt_free(&view);
  }
  asngn_buf_free(&b);
}
TEST(unknown_format_keeps_head_and_tail) {
  char text[10000];
  memset(text, 'x', sizeof text);
  memcpy(text, "HEAD", 4);
  memcpy(text + sizeof text - 5, "TAIL", 5);
  asngn_excerpt view = {0};
  ASSERT_OK(asngn_excerpt_build(text, strlen(text), 512, &view));
  ASSERT_CONTAINS(view.text, "HEAD");
  ASSERT_CONTAINS(view.text, "TAIL");
  ASSERT_EQ_INT(view.matches, 0);
  ASSERT_TRUE(strlen(view.text) <= 512);
  asngn_excerpt_free(&view);
}
TEST(log_digest_needs_no_model_and_reopens_late_range) {
  eng_fx f;
  ASSERT_TRUE(eng_setup_asper(&f, "echo", "context:{digest_threshold_chars:512},"));
  size_t len = 0;
  char *text = log_text(&len), *digest = NULL;
  size_t saved = 0, aux = 0;
  ASSERT_OK(asngn_digest_item(f.c, f.s, NULL, "project.test", text, len, &saved, &aux, &digest));
  ASSERT_CONTAINS(digest, "FINAL_DIAGNOSTIC");
  ASSERT_EQ_INT(aux, 0);
  ASSERT_EQ_INT(f.light.calls, 0);
  ASSERT_EQ_INT(f.s->blobs_n, 1);
  const char *found = strstr(text, "late.c");
  ASSERT_TRUE(found);
  char *slice = NULL;
  ASSERT_OK(asngn_blob_read(f.c, &f.s->blobs[0], (size_t)(found - text), 4096, &slice));
  ASSERT_CONTAINS(slice, "late.c:91:2: error: missing FINAL_DIAGNOSTIC");
  ASSERT_CONTAINS(slice, f.s->blobs[0].object_ref);
  free(slice);
  free(text);
  free(digest);
  eng_drop(&f);
}
TEST(compressor_cannot_remove_diagnostic_evidence) {
  eng_fx f;
  ASSERT_TRUE(eng_setup_asper(&f, "echo", "context:{digest_threshold_chars:512},"));
  ASSERT_TRUE(fake_model_push(&f.light, "Everything looks fine."));
  size_t len = 0;
  char *text = log_text(&len), *digest = NULL;
  ASSERT_OK(asngn_digest_item(f.c, f.s, NULL, "custom.read", text, len, NULL, NULL, &digest));
  ASSERT_CONTAINS(f.light.last_user, "FINAL_DIAGNOSTIC");
  ASSERT_CONTAINS(digest, "Everything looks fine.");
  ASSERT_CONTAINS(digest, "FINAL_DIAGNOSTIC");
  free(text);
  free(digest);
  eng_drop(&f);
}
TEST(redacted_offsets_and_unicode_boundaries) {
  eng_fx f;
  ASSERT_TRUE(eng_setup_asper(&f, "echo", "context:{digest_threshold_chars:512},"));
  asngn_buf b = {0};
  asngn_buf_appends(&b, "token sk-abcdefghijklmnopqrstuvwxyz123456789012345678\n");
  for (int i = 0; i < 300; i++)
    asngn_buf_appends(&b, "αβγ\n");
  asngn_buf_appends(&b, "last.c:2: error: AFTER_REDACTION\n");
  char *digest = NULL;
  ASSERT_OK(asngn_digest_item(f.c, f.s, NULL, "proc.run", b.data, b.len, NULL, NULL, &digest));
  void *raw = NULL;
  size_t len = 0;
  ASSERT_OK(asngn_siblings_object_read(f.c, f.s->blobs[0].object_ref, 0, 32768, &raw, &len));
  char *stored = asngn_strndup(raw, len);
  free(raw);
  ASSERT_TRUE(stored);
  ASSERT_NOT_CONTAINS(stored, "sk-abcdefghijklmnopqrstuvwxyz123456789012345678");
  asngn_excerpt view = {0};
  ASSERT_OK(asngn_excerpt_build(stored, len, 512, &view));
  char *slice = NULL;
  size_t offset = (size_t)(strstr(stored, "α") - stored);
  ASSERT_OK(asngn_blob_read(f.c, &f.s->blobs[0], offset, 5, &slice));
  ASSERT_TRUE(asngn_utf8_valid(slice, strlen(slice)));
  ASSERT_CONTAINS(slice, "αβ");
  free(slice);
  slice = NULL;
  ASSERT_ERR(asngn_blob_read(f.c, &f.s->blobs[0], offset + 1, 100, &slice), ASNGN_ERR_INVALID);
  ASSERT_TRUE(slice == NULL);
  ASSERT_ERR(asngn_blob_read(f.c, &f.s->blobs[0], len + 1, 100, &slice), ASNGN_ERR_INVALID);
  ASSERT_OK(asngn_blob_read(f.c, &f.s->blobs[0], len, 100, &slice));
  ASSERT_TRUE(slice == NULL);
  asngn_excerpt_free(&view);
  free(stored);
  free(digest);
  asngn_buf_free(&b);
  eng_drop(&f);
}
TEST(range_contracts_reject_invalid_or_unavailable_inputs) {
  char *schema = NULL;
  ASSERT_OK(asngn_protocol_steps(NULL, false, false, false, false, 2, false, &schema));
  const char *bad[] = {"{\"blob\":1}",
                       "{\"blob\":3,\"offset\":0}",
                       "{\"blob\":1,\"offset\":-1}",
                       "{\"blob\":1,\"offset\":4294967296}",
                       "{\"blob\":1,\"offset\":0,\"extra\":0}",
                       "\"B1\""};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    asngn_buf b = {0};
    asngn_buf_printf(&b, "{\"action\":\"open\",\"why\":\"inspect\",\"input\":%s}", bad[i]);
    char *wire = asngn_buf_detach(&b);
    ASSERT_ERR(asngn_protocol_decode(ASNGN_TASK_DECIDE, schema, &wire), ASNGN_ERR_PROTOCOL);
    free(wire);
  }
  char *wire = asngn_strdup(
      "{\"action\":\"open\",\"why\":\"inspect\",\"input\":{\"blob\":2,\"offset\":80000}}");
  ASSERT_OK(asngn_protocol_decode(ASNGN_TASK_DECIDE, schema, &wire));
  asngn_step step = {0};
  ASSERT_OK(asngn_step_parse(NULL, wire, &step));
  ASSERT_EQ_INT(step.blob_n, 2);
  ASSERT_EQ_INT(step.blob_offset, 80000);
  asngn_step_free(&step);
  free(wire);
  free(schema);
}
TEST(native_ranges_apply_without_certifying_success) {
  eng_fx f;
  ASSERT_TRUE(eng_setup_asper(&f, "echo", "context:{digest_threshold_chars:512},"));
  size_t len = 0;
  char *text = log_text(&len), *digest = NULL;
  ASSERT_OK(asngn_digest_item(f.c, f.s, NULL, "project.test", text, len, NULL, NULL, &digest));
  asngn_turn_state t = {.s = f.s, .gen_slot = 2};
  asngn_native_contract *contract = calloc(1, sizeof *contract);
  ASSERT_TRUE(contract);
  ASSERT_OK(asngn_native_contract_build(f.c, &t, false, contract));
  char args[128];
  size_t offset = (size_t)(strstr(text, "late.c") - text);
  snprintf(args, sizeof args, "{\"blob\":1,\"offset\":%zu}", offset);
  asmodel_tool_calls calls = {.count = 1, .calls = {{"range-1", "asterism_open", args}}};
  asngn_step steps[32] = {0};
  ASSERT_OK(asngn_native_steps(f.c, &t, contract, &calls, steps));
  ASSERT_EQ_INT(steps[0].blob_offset, offset);
  bool done = false;
  ASSERT_OK(asngn_action_apply(f.c, &t, &steps[0], false, false, false, &done));
  ASSERT_TRUE(!done && !t.verification_ok && t.work_n > 0);
  ASSERT_CONTAINS(t.work[t.work_n - 1].text, "FINAL_DIAGNOSTIC");
  asngn_step_free(&steps[0]);
  const char *bad[] = {"{\"blob\":2,\"offset\":0}", "{\"blob\":1}", "{\"blob\":1,\"offset\":-1}",
                       "{\"blob\":1,\"offset\":0,\"x\":0}"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    calls.calls[0].arguments = (char *)bad[i];
    ASSERT_ERR(asngn_native_steps(f.c, &t, contract, &calls, steps), ASNGN_ERR_PROTOCOL);
    asngn_step_free(&steps[0]);
  }
  /* ⁂ asper materializes its own events; this is not a local budget trim. */
  asngn_prompt prompt = {0};
  ASSERT_OK(asngn_context_assemble(f.c, f.s, &t, NULL, "inspect", 2, &prompt));
  ASSERT_CONTAINS(prompt.selection_json, "asper_owns_events");
  ASSERT_NOT_CONTAINS(prompt.selection_json, "\"reason\":\"working_budget\"");
  asngn_prompt_free(&prompt);
  asngn_turn_state_free(&t);
  free(contract);
  free(digest);
  free(text);
  eng_drop(&f);
}
TEST(invalid_text_creates_no_blob) {
  eng_fx f;
  ASSERT_TRUE(eng_setup_asper(&f, "echo", "context:{digest_threshold_chars:512},"));
  char text[1024], *digest = NULL;
  memset(text, 'x', sizeof text);
  text[sizeof text - 1] = 0;
  text[100] = (char)-1;
  ASSERT_ERR(
      asngn_digest_item(f.c, f.s, NULL, "project.test", text, strlen(text), NULL, NULL, &digest),
      ASNGN_ERR_INVALID);
  ASSERT_TRUE(!digest && !f.s->blobs_n);
  eng_drop(&f);
}
TEST(compressor_only_trace_has_no_extractive_ranges) {
  eng_fx f;
  ASSERT_TRUE(eng_setup_asper(&f, "echo", "context:{digest_threshold_chars:512},"));
  ASSERT_TRUE(fake_model_push(&f.light, "A plain summary."));
  char text[8192], *digest = NULL;
  memset(text, 'x', sizeof text - 1);
  text[sizeof text - 1] = 0;
  ASSERT_OK(asngn_digest_item(f.c, f.s, NULL, "custom.read", text, strlen(text), NULL, NULL, &digest));
  ASSERT_CONTAINS(digest, "A plain summary.");
  char **events = NULL;
  size_t count = 0;
  ASSERT_OK(asngn_tele_tail(f.c, 1, &events, &count));
  ASSERT_EQ_INT(count, 1);
  ASSERT_CONTAINS(events[0], "kind: \"evidence_selection\"");
  ASSERT_CONTAINS(events[0], "\"spans\":[]");
  ASSERT_CONTAINS(events[0], "\"selected_bytes\":0");
  ASSERT_CONTAINS(events[0], "\"compressor_used\":true");
  asngn_strings_free(events, count);
  free(digest);
  eng_drop(&f);
}
TEST_LIST = {TEST_ENTRY(late_diagnostic_is_exact_and_bounded),
             TEST_ENTRY(first_and_latest_diagnostics_survive_pressure),
             TEST_ENTRY(unknown_format_keeps_head_and_tail),
             TEST_ENTRY(log_digest_needs_no_model_and_reopens_late_range),
             TEST_ENTRY(compressor_cannot_remove_diagnostic_evidence),
             TEST_ENTRY(redacted_offsets_and_unicode_boundaries),
             TEST_ENTRY(range_contracts_reject_invalid_or_unavailable_inputs),
             TEST_ENTRY(native_ranges_apply_without_certifying_success),
             TEST_ENTRY(invalid_text_creates_no_blob),
             TEST_ENTRY(compressor_only_trace_has_no_extractive_ranges)};
RUN_ALL_TESTS()
