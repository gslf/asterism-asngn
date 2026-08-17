/*
 * test_time.c — RFC 3339 round-trips, ISO 8601 durations, clock vtable.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"
#include "fakes.h"

TEST(rfc3339_roundtrip_epoch) {
  asngn_time t = -1;
  char out[21];
  ASSERT_TRUE(asngn_time_parse_rfc3339("1970-01-01T00:00:00Z", &t));
  ASSERT_EQ_INT(t, 0);
  asngn_time_format_rfc3339(0, out);
  ASSERT_EQ_STR(out, "1970-01-01T00:00:00Z");
}

TEST(rfc3339_roundtrip_2026) {
  asngn_time t = 0;
  char out[21];
  ASSERT_TRUE(asngn_time_parse_rfc3339("2026-08-14T10:00:00Z", &t));
  ASSERT_EQ_INT(t, 1786701600LL);
  asngn_time_format_rfc3339(t, out);
  ASSERT_EQ_STR(out, "2026-08-14T10:00:00Z");
}

TEST(rfc3339_leap_day) {
  asngn_time t = 0;
  char out[21];
  ASSERT_TRUE(asngn_time_parse_rfc3339("2024-02-29T12:34:56Z", &t));
  ASSERT_EQ_INT(t, 1709210096LL);
  asngn_time_format_rfc3339(t, out);
  ASSERT_EQ_STR(out, "2024-02-29T12:34:56Z");
  /* Feb 29 only exists in leap years. */
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2023-02-29T00:00:00Z", &t));
}

TEST(rfc3339_parse_rejects) {
  asngn_time t = 0;
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2026-13-01T00:00:00Z", &t));
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2026-08-32T00:00:00Z", &t));
  /* The parser requires an explicit Z/z or a numeric offset: a bare
   * local time is rejected (time.c: sign must be Z, z, + or -). */
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2026-08-14T10:00:00", &t));
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2026-08-14T10:00:00Zx", &t));
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2026-08-14 10:00:00Z", &t));
  ASSERT_TRUE(!asngn_time_parse_rfc3339("", &t));
  ASSERT_TRUE(!asngn_time_parse_rfc3339(NULL, &t));
}

TEST(rfc3339_offsets_and_fractions) {
  asngn_time t = 0;
  /* Numeric offsets ARE part of the contract and normalize to UTC. */
  ASSERT_TRUE(asngn_time_parse_rfc3339("2026-08-14T12:00:00+02:00", &t));
  ASSERT_EQ_INT(t, 1786701600LL);
  ASSERT_TRUE(asngn_time_parse_rfc3339("2026-08-14T04:30:00-05:30", &t));
  ASSERT_EQ_INT(t, 1786701600LL);
  /* Fractional seconds are validated then truncated. */
  ASSERT_TRUE(asngn_time_parse_rfc3339("2026-08-14T10:00:00.25Z", &t));
  ASSERT_EQ_INT(t, 1786701600LL);
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2026-08-14T10:00:00.Z", &t));
  ASSERT_TRUE(!asngn_time_parse_rfc3339("2026-08-14T10:00:00+25:00", &t));
}

TEST(duration_accepts) {
  int64_t s = 0;
  ASSERT_TRUE(asngn_duration_parse("PT30S", &s));
  ASSERT_EQ_INT(s, 30);
  ASSERT_TRUE(asngn_duration_parse("PT5M", &s));
  ASSERT_EQ_INT(s, 300);
  ASSERT_TRUE(asngn_duration_parse("P7D", &s));
  ASSERT_EQ_INT(s, 604800);
  ASSERT_TRUE(asngn_duration_parse("P1W", &s));
  ASSERT_EQ_INT(s, 604800);
  ASSERT_TRUE(asngn_duration_parse("PT1H30M", &s));
  ASSERT_EQ_INT(s, 5400);
  ASSERT_TRUE(asngn_duration_parse("P1DT12H", &s));
  ASSERT_EQ_INT(s, 129600);
}

TEST(duration_rejects) {
  int64_t s = 0;
  ASSERT_TRUE(!asngn_duration_parse("P1M", &s));   /* months rejected  */
  ASSERT_TRUE(!asngn_duration_parse("P1Y", &s));   /* years rejected   */
  ASSERT_TRUE(!asngn_duration_parse("PT1.5S", &s)); /* no fractions    */
  ASSERT_TRUE(!asngn_duration_parse("", &s));
  ASSERT_TRUE(!asngn_duration_parse("5s", &s));
  ASSERT_TRUE(!asngn_duration_parse("-PT30S", &s)); /* no negatives    */
  ASSERT_TRUE(!asngn_duration_parse("PT-30S", &s));
  ASSERT_TRUE(!asngn_duration_parse("P", &s));
  ASSERT_TRUE(!asngn_duration_parse("PT", &s));
  ASSERT_TRUE(!asngn_duration_parse("P1W2D", &s)); /* PnW is exclusive */
  ASSERT_TRUE(!asngn_duration_parse(NULL, &s));
}

TEST(clock_fake_vtable) {
  fake_clock fc;
  asngn_clock ck;
  fake_clock_set(&fc, 1000000);
  ck = fake_clock_make(&fc);
  ASSERT_EQ_INT(asngn_clock_now(&ck), 1000000);
  ASSERT_EQ_INT(asngn_clock_mono_ms(&ck), 1000000000LL);
  fake_clock_advance(&fc, 5);
  ASSERT_EQ_INT(asngn_clock_now(&ck), 1000005);
  ASSERT_EQ_INT(asngn_clock_mono_ms(&ck), 1000005000LL);
  fake_clock_set(&fc, 42);
  ASSERT_EQ_INT(asngn_clock_now(&ck), 42);
  ASSERT_EQ_INT(asngn_clock_mono_ms(&ck), 42000);
}

TEST(clock_null_falls_back_to_real_time) {
  asngn_clock zeroed;
  ASSERT_TRUE(asngn_clock_now(NULL) > 1700000000LL);
  ASSERT_TRUE(asngn_clock_mono_ms(NULL) > 0);
  /* A clock with NULL function pointers also falls back. */
  memset(&zeroed, 0, sizeof zeroed);
  ASSERT_TRUE(asngn_clock_now(&zeroed) > 1700000000LL);
  ASSERT_TRUE(asngn_clock_mono_ms(&zeroed) > 0);
  /* The system vtable agrees with the fallback (same sources). */
  {
    asngn_clock sys = asngn_clock_system();
    ASSERT_TRUE(asngn_clock_now(&sys) > 1700000000LL);
  }
}

TEST_LIST = {
  TEST_ENTRY(rfc3339_roundtrip_epoch),
  TEST_ENTRY(rfc3339_roundtrip_2026),
  TEST_ENTRY(rfc3339_leap_day),
  TEST_ENTRY(rfc3339_parse_rejects),
  TEST_ENTRY(rfc3339_offsets_and_fractions),
  TEST_ENTRY(duration_accepts),
  TEST_ENTRY(duration_rejects),
  TEST_ENTRY(clock_fake_vtable),
  TEST_ENTRY(clock_null_falls_back_to_real_time),
};

RUN_ALL_TESTS()
