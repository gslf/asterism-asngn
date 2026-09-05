/*
 * test_json.c — the strict RFC 8259 codec of asngn-mcp (src/json.h):
 * parse + every accessor, stable compact writes, strict rejections
 * (trailing commas, lone surrogates, NaN, depth, garbage, bad UTF-8),
 * container ownership, and escape round-trips.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asmodel_json.h"

static asmodel_json_value *parse_ok(const char *s) {
  asmodel_json_value *v = NULL;
  if (asmodel_json_parse(s, strlen(s), &v) != 0) return NULL;
  return v;
}

static int parse_fails(const char *s) {
  asmodel_json_value *v = NULL;
  if (asmodel_json_parse(s, strlen(s), &v) == 0) {
    asmodel_json_free(v);
    return 0;
  }
  return v == NULL; /* contract: *out is NULL on any error */
}

/* ── tests ────────────────────────────────────────────────────────────── */

TEST(parse_and_accessors) {
  const char *src = "{\"a\":[1,2.5,\"x\",true,null],\"b\":{\"c\":\"d\"}}";
  asmodel_json_value *v, *a, *b, *e;

  v = parse_ok(src);
  ASSERT_TRUE(v != NULL);
  ASSERT_EQ_INT(asmodel_json_typeof(v), ASMODEL_JSON_OBJECT);
  ASSERT_EQ_INT((long long)asmodel_json_object_count(v), 2);
  ASSERT_EQ_STR(asmodel_json_object_key_at(v, 0), "a");
  ASSERT_EQ_STR(asmodel_json_object_key_at(v, 1), "b");

  a = asmodel_json_object_get(v, "a");
  ASSERT_EQ_INT(asmodel_json_typeof(a), ASMODEL_JSON_ARRAY);
  ASSERT_EQ_INT((long long)asmodel_json_array_len(a), 5);
  e = asmodel_json_array_at(a, 0);
  ASSERT_EQ_INT(asmodel_json_typeof(e), ASMODEL_JSON_NUMBER);
  ASSERT_TRUE(asmodel_json_is_int(e));
  ASSERT_EQ_INT(asmodel_json_int_value(e), 1);
  ASSERT_EQ_DBL(asmodel_json_double_value(e), 1.0, 0.0);
  e = asmodel_json_array_at(a, 1);
  ASSERT_TRUE(!asmodel_json_is_int(e));            /* 2.5 carries no int view  */
  ASSERT_EQ_DBL(asmodel_json_double_value(e), 2.5, 0.0);
  ASSERT_EQ_INT(asmodel_json_int_value(e), 2);     /* truncates non-ints       */
  e = asmodel_json_array_at(a, 2);
  ASSERT_EQ_INT(asmodel_json_typeof(e), ASMODEL_JSON_STRING);
  ASSERT_EQ_STR(asmodel_json_string_value(e), "x");
  ASSERT_EQ_INT((long long)asmodel_json_string_length(e), 1);
  e = asmodel_json_array_at(a, 3);
  ASSERT_EQ_INT(asmodel_json_typeof(e), ASMODEL_JSON_BOOL);
  ASSERT_EQ_INT(asmodel_json_bool_value(e), 1);
  e = asmodel_json_array_at(a, 4);
  ASSERT_EQ_INT(asmodel_json_typeof(e), ASMODEL_JSON_NULL);
  ASSERT_TRUE(asmodel_json_array_at(a, 5) == NULL);

  b = asmodel_json_object_get(v, "b");
  ASSERT_EQ_INT(asmodel_json_typeof(b), ASMODEL_JSON_OBJECT);
  ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_object_get(b, "c")), "d");
  ASSERT_TRUE(asmodel_json_object_value_at(v, 1) == b);
  ASSERT_TRUE(asmodel_json_object_get(v, "missing") == NULL);

  /* NULL-safe accessors */
  ASSERT_EQ_INT(asmodel_json_typeof(NULL), ASMODEL_JSON_NULL);
  ASSERT_EQ_INT(asmodel_json_int_value(NULL), 0);
  ASSERT_TRUE(asmodel_json_string_value(NULL) == NULL);
  ASSERT_EQ_INT((long long)asmodel_json_array_len(NULL), 0);
  ASSERT_EQ_INT((long long)asmodel_json_object_count(NULL), 0);
  asmodel_json_free(v);
}

TEST(compact_write_roundtrip_stable) {
  const char *src = "{\"a\":[1,2.5,\"x\",true,null],\"b\":{\"c\":\"d\"}}";
  asmodel_json_value *v, *v2, *cl;
  char *w1, *w2, *wp;

  v = parse_ok(src);
  ASSERT_TRUE(v != NULL);
  w1 = asmodel_json_write(v, 0);
  ASSERT_TRUE(w1 != NULL);
  ASSERT_EQ_STR(w1, src);                  /* canonical compact form  */
  ASSERT_TRUE(strchr(w1, '\n') == NULL);   /* single-line transport   */

  v2 = parse_ok(w1);
  ASSERT_TRUE(v2 != NULL);
  w2 = asmodel_json_write(v2, 0);
  ASSERT_TRUE(w2 != NULL);
  ASSERT_EQ_STR(w1, w2);                   /* write∘parse is stable   */
  free(w2);

  /* pretty output reparses to the same compact bytes */
  wp = asmodel_json_write(v, 1);
  ASSERT_TRUE(wp != NULL);
  ASSERT_TRUE(strchr(wp, '\n') != NULL);
  asmodel_json_free(v2);
  v2 = parse_ok(wp);
  ASSERT_TRUE(v2 != NULL);
  free(wp);
  w2 = asmodel_json_write(v2, 0);
  ASSERT_EQ_STR(w1, w2);
  free(w2);

  /* deep copy writes identically */
  cl = asmodel_json_clone(v);
  ASSERT_TRUE(cl != NULL);
  w2 = asmodel_json_write(cl, 0);
  ASSERT_EQ_STR(w1, w2);
  free(w2);
  asmodel_json_free(cl);
  free(w1);
  asmodel_json_free(v);
  asmodel_json_free(v2);
}

TEST(strict_rejections) {
  char deep[141], ok64[129];
  int i;
  asmodel_json_value *v;

  ASSERT_TRUE(parse_fails("[1,2,]"));        /* trailing comma        */
  ASSERT_TRUE(parse_fails("{\"a\":1,}"));
  ASSERT_TRUE(parse_fails("\"\\ud800\""));   /* lone high surrogate   */
  ASSERT_TRUE(parse_fails("\"\\udc00\""));   /* lone low surrogate    */
  ASSERT_TRUE(parse_fails("\"\\ud800\\ud800\""));
  ASSERT_TRUE(parse_fails("NaN"));           /* no NaN/Inf literals   */
  ASSERT_TRUE(parse_fails("{\"a\":NaN}"));
  ASSERT_TRUE(parse_fails("Infinity"));
  ASSERT_TRUE(parse_fails("{} x"));          /* trailing garbage      */
  ASSERT_TRUE(parse_fails("1 2"));
  ASSERT_TRUE(parse_fails("01"));            /* leading zero          */
  ASSERT_TRUE(parse_fails("\"\xFF\""));      /* invalid UTF-8 byte    */
  ASSERT_TRUE(parse_fails("\"a\x01" "b\""));  /* raw control char     */
  ASSERT_TRUE(parse_fails(""));
  ASSERT_TRUE(parse_fails("{\"a\" 1}"));     /* missing colon         */
  ASSERT_TRUE(parse_fails("'x'"));           /* no single quotes      */

  /* depth cap 64: 70-deep nesting is rejected ... */
  for (i = 0; i < 70; i++) deep[i] = '[';
  for (i = 0; i < 70; i++) deep[70 + i] = ']';
  deep[140] = '\0';
  ASSERT_TRUE(parse_fails(deep));

  /* ... while 64-deep still parses */
  for (i = 0; i < 64; i++) ok64[i] = '[';
  for (i = 0; i < 64; i++) ok64[64 + i] = ']';
  ok64[128] = '\0';
  v = parse_ok(ok64);
  ASSERT_TRUE(v != NULL);
  asmodel_json_free(v);
}

TEST(container_ownership) {
  asmodel_json_value *obj, *arr, *n;
  char *w;
  int st = 0;

  /* chained construction: every push/set owns its value even on
   * failure, so one final free releases the whole tree */
  obj = asmodel_json_object();
  ASSERT_TRUE(obj != NULL);
  arr = asmodel_json_array();
  ASSERT_TRUE(arr != NULL);
  st |= asmodel_json_object_set(obj, "s", asmodel_json_string("v"));
  st |= asmodel_json_array_push(arr, asmodel_json_int(1));
  st |= asmodel_json_array_push(arr, asmodel_json_double(2.5));
  st |= asmodel_json_array_push(arr, asmodel_json_bool(1));
  st |= asmodel_json_array_push(arr, asmodel_json_null());
  st |= asmodel_json_object_set(obj, "a", arr);      /* obj owns arr now       */
  st |= asmodel_json_object_set(obj, "s", asmodel_json_string("w")); /* replace, no dup  */
  ASSERT_EQ_INT(st, 0);
  ASSERT_EQ_INT((long long)asmodel_json_object_count(obj), 2);
  ASSERT_EQ_STR(asmodel_json_string_value(asmodel_json_object_get(obj, "s")), "w");
  w = asmodel_json_write(obj, 0);
  ASSERT_TRUE(w != NULL);
  ASSERT_EQ_STR(w, "{\"s\":\"w\",\"a\":[1,2.5,true,null]}");
  free(w);
  asmodel_json_free(obj);                            /* free exactly once      */

  /* failure paths return -1 and consume the value */
  ASSERT_EQ_INT(asmodel_json_array_push(NULL, asmodel_json_int(1)), -1);
  ASSERT_EQ_INT(asmodel_json_object_set(NULL, "k", asmodel_json_int(1)), -1);
  n = asmodel_json_int(3);
  ASSERT_EQ_INT(asmodel_json_array_push(n, asmodel_json_int(1)), -1); /* not an array    */
  ASSERT_EQ_INT(asmodel_json_object_set(n, NULL, asmodel_json_int(1)), -1);
  ASSERT_EQ_INT(asmodel_json_array_push(n, NULL), -1);      /* NULL value      */
  asmodel_json_free(n);

  /* constructor guards */
  ASSERT_TRUE(asmodel_json_string("bad \xFF utf8") == NULL);
  ASSERT_TRUE(asmodel_json_string(NULL) == NULL);
  ASSERT_TRUE(asmodel_json_double(HUGE_VAL) == NULL);       /* no Inf          */
}

TEST(escape_roundtrip) {
  asmodel_json_value *v;
  char *w;

  /* "è" (U+00E8) passes through as raw UTF-8 */
  v = asmodel_json_string("caff\xC3\xA8");
  ASSERT_TRUE(v != NULL);
  w = asmodel_json_write(v, 0);
  ASSERT_EQ_STR(w, "\"caff\xC3\xA8\"");
  asmodel_json_free(v);
  v = parse_ok(w);
  free(w);
  ASSERT_TRUE(v != NULL);
  ASSERT_EQ_STR(asmodel_json_string_value(v), "caff\xC3\xA8");
  asmodel_json_free(v);

  /* named control escapes */
  v = asmodel_json_string("a\tb\nc");
  ASSERT_TRUE(v != NULL);
  w = asmodel_json_write(v, 0);
  ASSERT_EQ_STR(w, "\"a\\tb\\nc\"");
  asmodel_json_free(v);
  v = parse_ok(w);
  free(w);
  ASSERT_TRUE(v != NULL);
  ASSERT_EQ_STR(asmodel_json_string_value(v), "a\tb\nc");
  asmodel_json_free(v);

  /* \uXXXX escapes: BMP, a surrogate pair, and a bare control */
  v = parse_ok("\"\\u00e8 \\ud83c\\udf1f \\u0001\"");
  ASSERT_TRUE(v != NULL);
  ASSERT_EQ_STR(asmodel_json_string_value(v),
                "\xC3\xA8 \xF0\x9F\x8C\x9F \x01");
  w = asmodel_json_write(v, 0);
  ASSERT_EQ_STR(w, "\"\xC3\xA8 \xF0\x9F\x8C\x9F \\u0001\"");
  asmodel_json_free(v);
  v = parse_ok(w);
  free(w);
  ASSERT_TRUE(v != NULL);
  ASSERT_EQ_STR(asmodel_json_string_value(v),
                "\xC3\xA8 \xF0\x9F\x8C\x9F \x01");
  asmodel_json_free(v);

  /* \u0000: embedded NUL survives with an explicit length */
  v = parse_ok("\"a\\u0000b\"");
  ASSERT_TRUE(v != NULL);
  ASSERT_EQ_INT((long long)asmodel_json_string_length(v), 3);
  ASSERT_TRUE(memcmp(asmodel_json_string_value(v), "a\0b", 3) == 0);
  w = asmodel_json_write(v, 0);
  ASSERT_EQ_STR(w, "\"a\\u0000b\"");
  free(w);
  asmodel_json_free(v);
}

TEST(number_edges) {
  asmodel_json_value *v;

  v = parse_ok("-0");
  ASSERT_TRUE(v != NULL);
  ASSERT_TRUE(!asmodel_json_is_int(v)); /* "-0" keeps its sign only as double */
  ASSERT_EQ_DBL(asmodel_json_double_value(v), 0.0, 0.0);
  asmodel_json_free(v);

  v = parse_ok("9223372036854775807");
  ASSERT_TRUE(v != NULL);
  ASSERT_TRUE(asmodel_json_is_int(v));
  ASSERT_EQ_INT(asmodel_json_int_value(v), 9223372036854775807LL);
  asmodel_json_free(v);

  v = parse_ok("1e2");
  ASSERT_TRUE(v != NULL);
  ASSERT_TRUE(!asmodel_json_is_int(v)); /* exponent form carries no int view  */
  ASSERT_EQ_DBL(asmodel_json_double_value(v), 100.0, 0.0);
  asmodel_json_free(v);
}

TEST_LIST = {
  TEST_ENTRY(parse_and_accessors),
  TEST_ENTRY(compact_write_roundtrip_stable),
  TEST_ENTRY(strict_rejections),
  TEST_ENTRY(container_ownership),
  TEST_ENTRY(escape_roundtrip),
  TEST_ENTRY(number_edges),
};

RUN_ALL_TESTS()
