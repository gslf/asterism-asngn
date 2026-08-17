/*
 * test_redact.c — the redaction corpus: structural patterns, PEM
 * blocks, headers, key=value pairs, the entropy pass, and tag equality.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

/* «redacted:xxxxxxxx» is 21 bytes: « (2) + "redacted:" (9) + 8 hex + » (2). */
#define TAG_LEN 21
#define TAG_OPEN "\xC2\xABredacted:"

static int has(const char *hay, const char *needle) {
  return hay != NULL && strstr(hay, needle) != NULL;
}

TEST(aws_key_masked) {
  const char *in = "key AKIAIOSFODNN7EXAMPLE end";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(!has(out, "AKIAIOSFODNN7EXAMPLE"));
  ASSERT_TRUE(has(out, TAG_OPEN));
  ASSERT_TRUE(strncmp(out, "key ", 4) == 0);
  ASSERT_TRUE(strcmp(out + strlen(out) - 4, " end") == 0);
  free(out);
}

TEST(aws_15_char_variant_not_masked) {
  /* "AKIA" + only 15 [A-Z0-9]: not the 20-byte shape, not entropy-long. */
  const char *in = "key AKIAIOSFODNN7EXAMPL end";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out == NULL);
  ASSERT_EQ_INT((long long)n, 0);
}

TEST(gcp_key_masked) {
  /* "AIza" + exactly 35 [A-Za-z0-9_-]. */
  const char *in = "k AIzaSyA1234567890abcdefghijkl_mnopqrstu .";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(!has(out, "AIza"));
  ASSERT_TRUE(has(out, TAG_OPEN));
  free(out);
}

TEST(github_token_masked) {
  /* ghp_ + 36 alnum; the prefix is masked together with the run. */
  const char *in = "t ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 u";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(!has(out, "ghp_"));
  ASSERT_TRUE(!has(out, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"));
  ASSERT_TRUE(strncmp(out, "t " TAG_OPEN, 2 + 11) == 0);
  ASSERT_EQ_INT((long long)strlen(out), 2 + TAG_LEN + 2); /* "t " tag " u" */
  free(out);
}

TEST(openai_key_masked) {
  const char *in = "use sk-abcdef1234567890ABCDEF here";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(!has(out, "sk-abcdef1234567890ABCDEF"));
  ASSERT_TRUE(has(out, TAG_OPEN));
  ASSERT_TRUE(strncmp(out, "use ", 4) == 0);
  ASSERT_TRUE(strcmp(out + strlen(out) - 5, " here") == 0);
  free(out);
}

TEST(pem_rsa_block_fully_masked) {
  const char *in =
      "before\n"
      "-----BEGIN RSA PRIVATE KEY-----\n"
      "MIIEowIBAAKCAQEA\n"
      "-----END RSA PRIVATE KEY-----\n"
      "after";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(!has(out, "BEGIN"));
  ASSERT_TRUE(!has(out, "PRIVATE"));
  ASSERT_TRUE(!has(out, "MIIEow"));
  ASSERT_TRUE(!has(out, "-----"));
  ASSERT_TRUE(strncmp(out, "before\n", 7) == 0);
  ASSERT_TRUE(strcmp(out + strlen(out) - 6, "\nafter") == 0);
  /* the whole block collapses into exactly one tag */
  ASSERT_EQ_INT((long long)strlen(out), 7 + TAG_LEN + 6);
  free(out);
}

TEST(pem_missing_end_masks_to_eof) {
  const char *in = "x -----BEGIN RSA PRIVATE KEY-----\nkeydata";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(strncmp(out, "x " TAG_OPEN, 2 + 11) == 0);
  ASSERT_EQ_INT((long long)strlen(out), 2 + TAG_LEN);
  ASSERT_TRUE(strcmp(out + strlen(out) - 2, "\xC2\xBB") == 0);
  free(out);
}

TEST(bearer_header_masks_only_token) {
  const char *in = "Authorization: Bearer abc123def456";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(strncmp(out, "Authorization: Bearer " TAG_OPEN, 22 + 11) == 0);
  ASSERT_TRUE(!has(out, "abc123def456"));
  ASSERT_EQ_INT((long long)strlen(out), 22 + TAG_LEN);
  free(out);
}

TEST(password_pair_masks_value_only) {
  const char *in = "password=hunter2secret";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(strncmp(out, "password=" TAG_OPEN, 9 + 11) == 0);
  ASSERT_TRUE(!has(out, "hunter2secret"));
  ASSERT_EQ_INT((long long)strlen(out), 9 + TAG_LEN);
  free(out);
}

TEST(api_key_quoted_masks_value_only) {
  const char *in = "api_key: \"quoted-value-here\"";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(strncmp(out, "api_key: \"" TAG_OPEN, 10 + 11) == 0);
  ASSERT_TRUE(!has(out, "quoted-value-here"));
  ASSERT_TRUE(out[strlen(out) - 1] == '"'); /* closing quote survives */
  ASSERT_EQ_INT((long long)strlen(out), 10 + TAG_LEN + 1);
  free(out);
}

TEST(high_entropy_run_masked) {
  /* 40 distinct base64-ish chars: entropy log2(40) ≈ 5.3 bits/char. */
  const char *in = "hash abcdefghijklmnopqrstuvwxyz0123456789ABCD tail";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(strncmp(out, "hash " TAG_OPEN, 5 + 11) == 0);
  ASSERT_TRUE(strcmp(out + strlen(out) - 5, " tail") == 0);
  ASSERT_TRUE(!has(out, "abcdefgh"));
  free(out);
}

TEST(low_entropy_run_not_masked) {
  const char *in = "run aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa end"; /* 36 a */
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out == NULL);
  ASSERT_EQ_INT((long long)n, 0);
}

TEST(plain_english_not_masked) {
  const char *in = "The quick brown fox jumps over the lazy dog.";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out == NULL);
  ASSERT_EQ_INT((long long)n, 0);
}

TEST(equal_secrets_equal_tags) {
  const char *in = "AKIAIOSFODNN7EXAMPLE AKIAIOSFODNN7EXAMPLE";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 2);
  ASSERT_EQ_INT((long long)strlen(out), TAG_LEN + 1 + TAG_LEN);
  ASSERT_TRUE(out[TAG_LEN] == ' ');
  ASSERT_TRUE(memcmp(out, out + TAG_LEN + 1, TAG_LEN) == 0);
  free(out);
}

TEST(output_is_valid_utf8) {
  const char *in = "caff\xC3\xA8 password=hunter2secret \xC3\xA8";
  char *out = NULL;
  size_t n = 0;
  ASSERT_OK(asngn_redact(in, strlen(in), &out, &n));
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_INT((long long)n, 1);
  ASSERT_TRUE(asngn_utf8_valid(out, strlen(out)));
  ASSERT_TRUE(strncmp(out, "caff\xC3\xA8 password=", 15) == 0);
  ASSERT_TRUE(strcmp(out + strlen(out) - 3, " \xC3\xA8") == 0);
  free(out);
}

TEST(null_and_empty_input_safe) {
  char *out = (char *)"sentinel";
  size_t n = 99;
  ASSERT_OK(asngn_redact(NULL, 0, &out, &n));
  ASSERT_TRUE(out == NULL);
  ASSERT_EQ_INT((long long)n, 0);
  out = (char *)"sentinel";
  n = 99;
  ASSERT_OK(asngn_redact("", 0, &out, &n));
  ASSERT_TRUE(out == NULL);
  ASSERT_EQ_INT((long long)n, 0);
  ASSERT_ERR(asngn_redact("x", 1, NULL, &n), ASNGN_ERR_INVALID);
  ASSERT_ERR(asngn_redact("x", 1, &out, NULL), ASNGN_ERR_INVALID);
}

TEST_LIST = {
  TEST_ENTRY(aws_key_masked),
  TEST_ENTRY(aws_15_char_variant_not_masked),
  TEST_ENTRY(gcp_key_masked),
  TEST_ENTRY(github_token_masked),
  TEST_ENTRY(openai_key_masked),
  TEST_ENTRY(pem_rsa_block_fully_masked),
  TEST_ENTRY(pem_missing_end_masks_to_eof),
  TEST_ENTRY(bearer_header_masks_only_token),
  TEST_ENTRY(password_pair_masks_value_only),
  TEST_ENTRY(api_key_quoted_masks_value_only),
  TEST_ENTRY(high_entropy_run_masked),
  TEST_ENTRY(low_entropy_run_not_masked),
  TEST_ENTRY(plain_english_not_masked),
  TEST_ENTRY(equal_secrets_equal_tags),
  TEST_ENTRY(output_is_valid_utf8),
  TEST_ENTRY(null_and_empty_input_safe),
};

RUN_ALL_TESTS()
