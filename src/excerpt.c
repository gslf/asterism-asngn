/* Bounded compiler/test recognizers select evidence; they never verify success. */
#include "excerpt.h"
#include "asmodel_json.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *text, *reason;
} marker;
static const marker markers[] = {{": error:", "compiler"},
                                 {": fatal error:", "compiler"},
                                 {"error[E", "compiler"},
                                 {" error C", "compiler"},
                                 {"CMake Error", "build"},
                                 {"FAILED:", "build"},
                                 {"Traceback (most recent call last):", "traceback"},
                                 {"AssertionError", "assertion"},
                                 {"FAILED ", "test"},
                                 {"FAIL: ", "test"},
                                 {"***Failed", "test"},
                                 {"ERROR collecting ", "test"},
                                 {"test result: FAILED", "test"},
                                 {"tests failed", "summary"}};
static int by_start(const void *a, const void *b) {
  const asngn_excerpt_span *x = a, *y = b;
  return (x->start > y->start) - (x->start < y->start);
}
static asngn_excerpt_span window(const char *text, size_t len, size_t start, size_t width,
                                 const char *reason) {
  while (start < len && ((unsigned char)text[start] & 0xc0) == 0x80)
    start++;
  size_t end = width < len - start ? start + width : len;
  while (end > start && end < len && ((unsigned char)text[end] & 0xc0) == 0x80)
    end--;
  return (asngn_excerpt_span){start, end, reason};
}
void asngn_excerpt_free(asngn_excerpt *view) {
  if (view) {
    free(view->text);
    memset(view, 0, sizeof *view);
  }
}
asngn_err asngn_excerpt_build(const char *text, size_t len, size_t budget, asngn_excerpt *out) {
  if (!out) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof *out);
  if (!text || budget < 512 || budget > 65536 || memchr(text, 0, len) ||
      !asngn_utf8_valid(text, len))
    return ASNGN_ERR_INVALID;
  size_t slots = (budget - 256) / 128;
  if (slots < 2) slots = 2;
  if (slots > ASNGN_EXCERPT_SPANS) slots = ASNGN_EXCERPT_SPANS;
  size_t width = (budget - 256) / slots - 64;
  for (size_t pos = 0; pos < len; pos++) {
    const char *reason = NULL;
    for (size_t j = 0; j < sizeof markers / sizeof markers[0]; j++) {
      size_t n = strlen(markers[j].text);
      if (n <= len - pos && !memcmp(text + pos, markers[j].text, n)) {
        reason = markers[j].reason;
        break;
      }
    }
    if (!reason) continue;
    out->matches++;
    asngn_excerpt_span span =
        window(text, len, pos > width / 4 ? pos - width / 4 : 0, width, reason);
    /* Keep the first diagnostic and the most recent ones, without duplicates. */
    if (out->count && span.start <= out->spans[out->count - 1].end &&
        span.end <= out->spans[out->count - 1].end)
      continue;
    if (out->count == slots) {
      memmove(out->spans + 1, out->spans + 2, (slots - 2) * sizeof span);
      out->count--;
    }
    out->spans[out->count++] = span;
  }
  if (len <= budget) {
    out->text = asngn_strndup(text, len);
    out->spans[0] = (asngn_excerpt_span){0, len, "complete"};
    out->count = 1;
    out->selected_bytes = len;
    return out->text ? ASNGN_OK : ASNGN_ERR_NOMEM;
  }
  if (!out->count) {
    /* Unknown formats still retain the end of the output. */
    width = (budget - 256) / 2 - 64;
    out->spans[0] = window(text, len, 0, width, "head");
    out->spans[1] = window(text, len, len - width, width, "tail");
    out->count = 2;
  }
  qsort(out->spans, out->count, sizeof out->spans[0], by_start);
  size_t merged = 0;
  for (size_t i = 0; i < out->count; i++) {
    asngn_excerpt_span span = out->spans[i];
    if (merged && span.start <= out->spans[merged - 1].end) {
      if (span.end > out->spans[merged - 1].end) out->spans[merged - 1].end = span.end;
      out->spans[merged - 1].reason = "diagnostics";
    } else out->spans[merged++] = span;
  }
  out->count = merged;
  asngn_buf b = {0};
  asngn_err e = asngn_buf_printf(
      &b,
      "[extractive view: %zu diagnostic markers; other bytes omitted; not a verification result]\n",
      out->matches);
  for (size_t i = 0; e == ASNGN_OK && i < out->count; i++) {
    const asngn_excerpt_span *span = &out->spans[i];
    e = asngn_buf_printf(&b, "\n[bytes %zu-%zu; %s]\n", span->start, span->end, span->reason);
    if (e == ASNGN_OK) e = asngn_buf_append(&b, text + span->start, span->end - span->start);
    if (e == ASNGN_OK) e = asngn_buf_appendc(&b, '\n');
    out->selected_bytes += span->end - span->start;
  }
  if (e == ASNGN_OK && b.len > budget) e = ASNGN_ERR_LIMIT;
  if (e == ASNGN_OK) out->text = asngn_buf_detach(&b);
  asngn_buf_free(&b);
  return e;
}

char *asngn_excerpt_trace(const asngn_excerpt *view, const char *source, size_t bytes,
                          size_t budget, bool compressor_used) {
  asmodel_json_value *v = asmodel_json_object(), *spans = asmodel_json_array();
  int bad = !v || !spans;
  for (size_t i = 0; !bad && i < view->count; i++) {
    const asngn_excerpt_span *span = &view->spans[i];
    asmodel_json_value *item = asmodel_json_object();
    bad = !item ||
          asmodel_json_object_set(item, "start", asmodel_json_int((long long)span->start)) ||
          asmodel_json_object_set(item, "end", asmodel_json_int((long long)span->end)) ||
          asmodel_json_object_set(item, "reason", asmodel_json_string(span->reason));
    if (bad) asmodel_json_free(item);
    else bad = asmodel_json_array_push(spans, item);
  }
  if (bad) asmodel_json_free(spans);
  else bad = asmodel_json_object_set(v, "spans", spans);
#define SET(key, value) bad = bad || asmodel_json_object_set(v, key, value)
  SET("schema", asmodel_json_int(1));
  SET("policy", asmodel_json_string("diagnostic-windows-v1"));
  SET("source", asmodel_json_string(source));
  SET("source_bytes", asmodel_json_int((long long)bytes));
  SET("selected_bytes", asmodel_json_int((long long)view->selected_bytes));
  SET("diagnostic_markers", asmodel_json_int((long long)view->matches));
  SET("excerpt_budget_bytes", asmodel_json_int((long long)budget));
  SET("compressor_used", asmodel_json_bool(compressor_used));
#undef SET
  char *json = bad ? NULL : asmodel_json_write(v, 0);
  asmodel_json_free(v);
  return json;
}
