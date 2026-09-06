#include "asngn_internal.h"
#include "asngn_test.h"
#include "fakes.h"
#include "operation_record.h"

typedef struct {
  char root[256];
  asngn_ctx *ctx;
  fake_clock clock;
} fixture;

static bool setup(fixture *f) {
  memset(f, 0, sizeof *f);
  if (!asngn_test_tmpdir(f->root) || !(f->ctx = calloc(1, sizeof *f->ctx)))
    return false;
  f->ctx->root = asngn_strdup(f->root);
  os_rwlock_init(&f->ctx->lock);
  os_mutex_init(&f->ctx->err_mu);
  fake_clock_set(&f->clock, 100 * 86400);
  f->ctx->clock = fake_clock_make(&f->clock);
  return f->ctx->root && asngn_operations_load(f->ctx) == ASNGN_OK;
}
static void drop(fixture *f) {
  os_mutex_destroy(&f->ctx->err_mu);
  os_rwlock_destroy(&f->ctx->lock);
  free(f->ctx->root);
  free(f->ctx);
  asngn_test_rmtree(f->root);
}
static bool consistent(const asngn_consumption_totals *v) {
  return v->charged_tokens == v->known_input_tokens + v->known_output_tokens + v->unsettled_tokens +
                                  v->unknown_tokens &&
         v->unsettled_calls + v->unknown_calls <= v->calls &&
         v->failed_calls + v->cancelled_calls <= v->calls - v->unsettled_calls;
}

/* The fixture uses the real codec/framer with one sync after construction.
 * Reusing equal-size UUID slots avoids millions of fixture JSON allocations. */
static asngn_err settled_history(fixture *f, const char *path, size_t calls) {
  asngn_operation op = {.model = "model", .kind = "generate", .day = 100};
  strcpy(op.id, "00000000-0000-4000-8000-000000000000");
  char *begin = asngn_operation_encode(&op, "reserved", 0, 0, 0, false, ASNGN_OK);
  char *end = asngn_operation_encode(&op, "settled", 0, 0, 0, true, ASNGN_OK);
  char *begin_id = begin ? strstr(begin, op.id) : NULL;
  char *end_id = end ? strstr(end, op.id) : NULL;
  asngn_stream stream = {0};
  asngn_err e =
      begin_id && end_id ? asngn_stream_open(f->ctx, &stream, path, false) : ASNGN_ERR_NOMEM;
  for (size_t i = 0; e == ASNGN_OK && i < calls; i++) {
    char prefix[9];
    snprintf(prefix, sizeof prefix, "%08x", (unsigned)i);
    memcpy(begin_id, prefix, 8);
    memcpy(end_id, prefix, 8);
    e = asngn_wal_append(f->ctx, &stream, begin, strlen(begin));
    if (e == ASNGN_OK)
      e = asngn_wal_append(f->ctx, &stream, end, strlen(end));
  }
  if (e == ASNGN_OK)
    e = os_fsync(stream.fp);
  asngn_stream_close(&stream);
  free(begin);
  free(end);
  return e;
}

TEST(admission_reserves_room_for_the_final_settlement) {
  fixture f;
  asngn_operation final, denied;
  asngn_consumption usage;
  ASSERT_TRUE(setup(&f));
  char *path = os_path_join(f.root, "operations.xcdn");
  ASSERT_TRUE(path);
  ASSERT_OK(settled_history(&f, path, ASNGN_OPERATIONS_MAX - 1));
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 10, &final));
  uint64_t before, after;
  ASSERT_OK(os_file_size(path, &before));
  asngn_err e = asngn_operation_begin(f.ctx, "model", "generate", NULL, 0, &denied);
  if (e != ASNGN_ERR_LIMIT) {
    if (e == ASNGN_OK) {
      asngn_err first = asngn_operation_end(f.ctx, &final, 2, 1, true, ASNGN_OK);
      asngn_err second = asngn_operation_end(f.ctx, &denied, 0, 0, true, ASNGN_OK);
      asngn_err recovered = asngn_operations_load(f.ctx);
      fprintf(stderr, "  over-admission: settlements %s/%s, replay %s\n", asngn_err_name(first),
              asngn_err_name(second), asngn_err_name(recovered));
    }
    free(path);
    drop(&f);
    ASSERT_ERR(e, ASNGN_ERR_LIMIT);
  }
  ASSERT_OK(os_file_size(path, &after));
  ASSERT_EQ_INT(before, after);
  ASSERT_OK(asngn_operation_end(f.ctx, &final, 2, 1, true, ASNGN_OK));
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.lifetime.calls, ASNGN_OPERATIONS_MAX);
  ASSERT_EQ_INT(usage.lifetime.unsettled_calls, 0);
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, 3);
  ASSERT_ERR(asngn_operation_begin(f.ctx, "model", "generate", NULL, 0, &denied), ASNGN_ERR_LIMIT);

  /* A complete over-quota record is not a torn tail that recovery may discard. */
  asngn_uuid_v4(final.id);
  char *extra = asngn_operation_encode(&final, "reserved", 0, 0, 0, false, ASNGN_OK);
  asngn_stream stream;
  ASSERT_TRUE(extra);
  ASSERT_OK(asngn_stream_open(f.ctx, &stream, path, true));
  ASSERT_OK(asngn_wal_append(f.ctx, &stream, extra, strlen(extra)));
  asngn_stream_close(&stream);
  free(extra);
  FILE *tail = os_fopen(path, "ab");
  ASSERT_TRUE(tail);
  ASSERT_TRUE(fputs("// incomplete", tail) >= 0);
  ASSERT_OK(os_fsync(tail));
  fclose(tail);
  ASSERT_OK(os_file_size(path, &before));
  ASSERT_ERR(asngn_operations_load(f.ctx), ASNGN_ERR_LIMIT);
  ASSERT_OK(os_file_size(path, &after));
  ASSERT_EQ_INT(before, after);
  ASSERT_EQ_INT(f.ctx->consumption.lifetime.calls, ASNGN_OPERATIONS_MAX);
  free(path);
  drop(&f);
}

TEST(all_outcomes_survive_reopen_without_conversation_commits) {
  fixture f;
  asngn_operation cancelled, passed, pending, failed;
  asngn_consumption usage, replayed;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 100, &cancelled));
  ASSERT_OK(asngn_operation_end(f.ctx, &cancelled, 7, 9, false, ASNGN_ERR_CANCELLED));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 50, &passed));
  ASSERT_OK(asngn_operation_end(f.ctx, &passed, 10, 5, true, ASNGN_OK));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "embed-query", NULL, 25, &pending));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "embed-document", NULL, 30, &failed));
  ASSERT_OK(asngn_operation_end(f.ctx, &failed, 5, 2, true, ASNGN_ERR_MODEL));
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.lifetime.calls, 4);
  ASSERT_EQ_INT(usage.lifetime.unsettled_calls, 1);
  ASSERT_EQ_INT(usage.lifetime.unknown_calls, 1);
  ASSERT_EQ_INT(usage.lifetime.failed_calls, 1);
  ASSERT_EQ_INT(usage.lifetime.cancelled_calls, 1);
  ASSERT_EQ_INT(usage.lifetime.known_input_tokens, 15);
  ASSERT_EQ_INT(usage.lifetime.known_output_tokens, 7);
  ASSERT_EQ_INT(usage.lifetime.unsettled_tokens, 25);
  ASSERT_EQ_INT(usage.lifetime.unknown_tokens, 100);
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, 147);
  ASSERT_TRUE(consistent(&usage.lifetime));
  memset(&f.ctx->consumption, 0, sizeof f.ctx->consumption);
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_get_consumption(f.ctx, &replayed));
  ASSERT_TRUE(!memcmp(&usage, &replayed, sizeof usage));
  ASSERT_EQ_INT(f.ctx->sessions_n, 0);
  drop(&f);
}

TEST(midnight_and_clock_rollback_preserve_reservation_days) {
  fixture f;
  asngn_operation old, current, rolled_back;
  asngn_consumption usage;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 100, &old));
  fake_clock_advance(&f.clock, 86400);
  ASSERT_EQ_INT(asngn_daily_spend(f.ctx), 0); /* No new inference is needed to roll the view. */
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 50, &current));
  ASSERT_OK(asngn_operation_end(f.ctx, &old, 10, 5, true, ASNGN_OK));
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.today.charged_tokens, 50);
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, 65);
  ASSERT_OK(asngn_operation_end(f.ctx, &current, 3, 2, true, ASNGN_OK));
  fake_clock_advance(&f.clock, -86400);
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 17, &rolled_back));
  ASSERT_EQ_INT(rolled_back.day, 101);
  ASSERT_OK(asngn_operation_end(f.ctx, &rolled_back, 0, 0, false, ASNGN_ERR_MODEL));
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.utc_day, 101);
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, 37);
  ASSERT_EQ_INT(usage.today.charged_tokens, 22);
  ASSERT_EQ_INT(usage.today.calls, 2);
  ASSERT_TRUE(consistent(&usage.today) && consistent(&usage.lifetime));
  drop(&f);
}

TEST(lifetime_overflow_rejects_admission_across_days) {
  fixture f;
  asngn_operation huge, next;
  asngn_consumption usage;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, INT64_MAX, &huge));
  fake_clock_advance(&f.clock, 86400);
  ASSERT_ERR(asngn_operation_begin(f.ctx, "model", "generate", NULL, 1, &next), ASNGN_ERR_LIMIT);
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, INT64_MAX);
  ASSERT_EQ_INT(usage.today.charged_tokens, 0);
  ASSERT_EQ_INT(usage.lifetime.calls, 1);
  ASSERT_OK(asngn_operation_end(f.ctx, &huge, 1, 0, true, ASNGN_OK));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 1, &next));
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, 2);
  drop(&f);
}

static int fail_sync(void *ud, const char *point) {
  (void)ud;
  return !strcmp(point, "stream_sync");
}
TEST(uncertain_persistence_never_exposes_a_successful_snapshot) {
  fixture f;
  asngn_operation op;
  asngn_consumption usage, zero = {0};
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 100, &op));
  f.ctx->fault = fail_sync;
  ASSERT_ERR(asngn_operation_end(f.ctx, &op, 1, 2, true, ASNGN_ERR_MODEL), ASNGN_ERR_IO);
  f.ctx->fault = NULL;
  memset(&usage, 0x55, sizeof usage);
  ASSERT_ERR(asngn_get_consumption(f.ctx, &usage), ASNGN_ERR_IO);
  ASSERT_TRUE(!memcmp(&usage, &zero, sizeof usage));
  ASSERT_EQ_INT(asngn_daily_spend(f.ctx), INT64_MAX);
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, 3);
  ASSERT_EQ_INT(usage.lifetime.failed_calls, 1);
  ASSERT_EQ_INT(usage.lifetime.unsettled_calls, 0);
  drop(&f);
}

TEST(zero_reservations_keep_unknown_and_unsettled_call_counts) {
  fixture f;
  asngn_operation unknown, pending, known;
  asngn_consumption usage;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 0, &unknown));
  ASSERT_OK(asngn_operation_end(f.ctx, &unknown, 0, 0, false, ASNGN_ERR_CANCELLED));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 0, &pending));
  ASSERT_OK(asngn_operation_begin(f.ctx, "model", "generate", NULL, 0, &known));
  ASSERT_OK(asngn_operation_end(f.ctx, &known, 0, 0, true, ASNGN_OK));
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.lifetime.charged_tokens, 0);
  ASSERT_EQ_INT(usage.lifetime.calls, 3);
  ASSERT_EQ_INT(usage.lifetime.unsettled_calls, 1);
  ASSERT_EQ_INT(usage.lifetime.unknown_calls, 1);
  ASSERT_EQ_INT(usage.lifetime.cancelled_calls, 1);
  drop(&f);
}

TEST(reservation_owns_model_and_kind_identity_until_settlement) {
  fixture f;
  asngn_operation op;
  char model[] = "model", kind[] = "generate";
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(asngn_operation_begin(f.ctx, model, kind, NULL, 5, &op));
  model[0] = 'x';
  kind[0] = 'x';
  ASSERT_OK(asngn_operation_end(f.ctx, &op, 1, 0, true, ASNGN_OK));
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_EQ_STR(op.model, "model");
  ASSERT_EQ_STR(op.kind, "generate");
  drop(&f);
}

#ifndef ASNGN_NO_THREADS
typedef struct {
  asngn_ctx *owner;
  int ok;
} lane;
static void *produce(void *ud) {
  lane *l = ud;
  asngn_ctx *borrowed = calloc(1, sizeof *borrowed);
  if (!borrowed)
    return NULL;
  borrowed->owner = l->owner;
  l->ok = 1;
  for (size_t i = 0; i < 40 && l->ok; i++) {
    asngn_operation op;
    asngn_consumption usage;
    l->ok = asngn_operation_begin(borrowed, "model", "generate", NULL, 100, &op) == ASNGN_OK &&
            asngn_get_consumption(borrowed, &usage) == ASNGN_OK && consistent(&usage.lifetime) &&
            asngn_operation_end(borrowed, &op, 2, 1, true,
                                i % 2 ? ASNGN_ERR_CANCELLED : ASNGN_OK) == ASNGN_OK;
  }
  free(borrowed);
  return NULL;
}
TEST(concurrent_lanes_share_one_atomic_consumption_projection) {
  fixture f;
  lane lanes[4];
  os_thread threads[4];
  asngn_consumption usage, replayed;
  ASSERT_TRUE(setup(&f));
  size_t started = 0;
  for (; started < 4; started++) {
    lanes[started] = (lane){.owner = f.ctx};
    if (os_thread_start(&threads[started], produce, &lanes[started]) != ASNGN_OK)
      break;
  }
  for (size_t i = 0; i < started; i++)
    os_thread_join(&threads[i]);
  ASSERT_EQ_INT(started, 4);
  for (size_t i = 0; i < 4; i++)
    ASSERT_TRUE(lanes[i].ok);
  ASSERT_OK(asngn_get_consumption(f.ctx, &usage));
  ASSERT_EQ_INT(usage.lifetime.calls, 160);
  ASSERT_EQ_INT(usage.lifetime.known_input_tokens, 320);
  ASSERT_EQ_INT(usage.lifetime.known_output_tokens, 160);
  ASSERT_EQ_INT(usage.lifetime.cancelled_calls, 80);
  ASSERT_EQ_INT(usage.lifetime.unsettled_calls, 0);
  ASSERT_OK(asngn_operations_load(f.ctx));
  ASSERT_OK(asngn_get_consumption(f.ctx, &replayed));
  ASSERT_TRUE(!memcmp(&usage, &replayed, sizeof usage));
  drop(&f);
}
#endif

TEST_LIST = {
    TEST_ENTRY(admission_reserves_room_for_the_final_settlement),
    TEST_ENTRY(all_outcomes_survive_reopen_without_conversation_commits),
    TEST_ENTRY(midnight_and_clock_rollback_preserve_reservation_days),
    TEST_ENTRY(lifetime_overflow_rejects_admission_across_days),
    TEST_ENTRY(uncertain_persistence_never_exposes_a_successful_snapshot),
    TEST_ENTRY(zero_reservations_keep_unknown_and_unsettled_call_counts),
    TEST_ENTRY(reservation_owns_model_and_kind_identity_until_settlement),
#ifndef ASNGN_NO_THREADS
    TEST_ENTRY(concurrent_lanes_share_one_atomic_consumption_projection),
#endif
};
RUN_ALL_TESTS()
