/*
 * test_util.c — buffers, slugs, UTF-8, controls, FNV, sentence trim.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

TEST(buf_lifecycle) {
  asngn_buf b;
  char *s;
  asngn_buf_init(&b);
  ASSERT_OK(asngn_buf_appends(&b, "hello"));
  ASSERT_OK(asngn_buf_appendc(&b, ' '));
  ASSERT_OK(asngn_buf_printf(&b, "%s %d", "world", 42));
  ASSERT_EQ_STR(b.data, "hello world 42");
  ASSERT_EQ_INT((long long)b.len, 14);
  s = asngn_buf_detach(&b);
  ASSERT_EQ_STR(s, "hello world 42");
  ASSERT_EQ_INT((long long)b.len, 0);
  free(s);
  asngn_buf_free(&b);
}

TEST(buf_detach_empty) {
  asngn_buf b;
  char *s;
  asngn_buf_init(&b);
  s = asngn_buf_detach(&b);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ_STR(s, "");
  free(s);
  asngn_buf_free(&b);
}

TEST(slug_rules) {
  ASSERT_TRUE(asngn_slug_valid("thesis"));
  ASSERT_TRUE(asngn_slug_valid("a"));
  ASSERT_TRUE(asngn_slug_valid("a-1-b"));
  ASSERT_TRUE(!asngn_slug_valid(""));
  ASSERT_TRUE(!asngn_slug_valid("-lead"));
  ASSERT_TRUE(!asngn_slug_valid("Upper"));
  ASSERT_TRUE(!asngn_slug_valid("has space"));
  ASSERT_TRUE(!asngn_slug_valid("under_score"));
  {
    char big[70];
    memset(big, 'a', sizeof big);
    big[64] = '\0';
    ASSERT_TRUE(asngn_slug_valid(big));
    big[64] = 'a';
    big[65] = '\0';
    ASSERT_TRUE(!asngn_slug_valid(big));
  }
}

TEST(utf8_validation) {
  ASSERT_TRUE(asngn_utf8_valid("plain ascii", 11));
  ASSERT_TRUE(asngn_utf8_valid("caff\xC3\xA8", 6));
  ASSERT_TRUE(asngn_utf8_valid("\xE2\x9C\xA6", 3));      /* U+2726 */
  ASSERT_TRUE(asngn_utf8_valid("\xF0\x9F\x8C\x9F", 4));  /* U+1F31F */
  ASSERT_TRUE(!asngn_utf8_valid("\xC0\xAF", 2));         /* overlong */
  ASSERT_TRUE(!asngn_utf8_valid("\xED\xA0\x80", 3));     /* surrogate */
  ASSERT_TRUE(!asngn_utf8_valid("\xFF", 1));
  ASSERT_TRUE(!asngn_utf8_valid("\xC3", 1));             /* truncated */
}

TEST(strip_controls) {
  char s[] = "a\x01" "b\tc\nd\x7f" "e";
  size_t n = asngn_strip_controls(s, strlen(s));
  s[n] = '\0';
  ASSERT_EQ_STR(s, "ab\tc\nde");
}

TEST(fnv_vectors) {
  /* FNV-1a 64 known vectors */
  ASSERT_TRUE(asngn_fnv1a64("", 0) == 14695981039346656037ULL);
  ASSERT_TRUE(asngn_fnv1a64("a", 1) == 12638187200555641996ULL);
}

TEST(sentence_trim_cases) {
  const char *s1 = "One. Two. Three incomplete";
  const char *s2 = "No boundary at all";
  const char *s3 = "Line one\nline two partial";
  const char *s4 = "Ends clean.";
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s1, strlen(s1)), 9);
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s2, strlen(s2)),
                (long long)strlen(s2));
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s3, strlen(s3)), 9);
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s4, strlen(s4)),
                (long long)strlen(s4));
}

TEST(prefix_and_casecmp) {
  ASSERT_TRUE(asngn_str_has_prefix("asterism", "aster"));
  ASSERT_TRUE(!asngn_str_has_prefix("ast", "aster"));
  ASSERT_EQ_INT(asngn_strcasecmp("TeRsE", "terse"), 0);
  ASSERT_TRUE(asngn_strcasecmp("a", "b") != 0);
}

TEST_LIST = {
  TEST_ENTRY(buf_lifecycle),
  TEST_ENTRY(buf_detach_empty),
  TEST_ENTRY(slug_rules),
  TEST_ENTRY(utf8_validation),
  TEST_ENTRY(strip_controls),
  TEST_ENTRY(fnv_vectors),
  TEST_ENTRY(sentence_trim_cases),
  TEST_ENTRY(prefix_and_casecmp),
};

RUN_ALL_TESTS()
