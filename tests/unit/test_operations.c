#include "asngn_test.h"
#include "asngn_internal.h"

TEST(reservations_survive_failure_and_reopen) {
  char root[256], lock_path[512];
  asngn_ctx *c = calloc(1, sizeof *c);
  asngn_operation failed, passed, pending;
  FILE *lock;
  ASSERT_TRUE(c && asngn_test_tmpdir(root));
  c->root = asngn_strdup(root);
  os_rwlock_init(&c->lock);
  os_mutex_init(&c->err_mu);
  ASSERT_OK(asngn_operations_load(c));
  ASSERT_OK(asngn_operation_begin(c, "model", "answer", 100, &failed));
  ASSERT_OK(asngn_operation_end(c, &failed, 0, 0, false, ASNGN_ERR_CANCELLED));
  ASSERT_OK(asngn_operation_begin(c, "model", "judge", 50, &passed));
  ASSERT_OK(asngn_operation_end(c, &passed, 10, 5, true, ASNGN_OK));
  ASSERT_ERR(asngn_operation_end(c, &passed, 10, 5, true, ASNGN_OK), ASNGN_ERR_INVALID);
  ASSERT_OK(asngn_operation_begin(c, "model", "answer", 25, &pending));
  ASSERT_EQ_INT(c->daily_spent, 140);
  c->daily_spent = 0;
  ASSERT_OK(asngn_operations_load(c));
  ASSERT_EQ_INT(c->daily_spent, 140);
  c->cfg.daily_tokens = 150;
  ASSERT_ERR(asngn_operation_begin(c, "model", "answer", 20, &pending), ASNGN_ERR_LIMIT);
  snprintf(lock_path, sizeof lock_path, "%s/lock", root);
  lock = os_store_lock(lock_path);
  ASSERT_TRUE(lock != NULL);
  ASSERT_TRUE(os_store_lock(lock_path) == NULL);
  fclose(lock);
  lock = os_store_lock(lock_path);
  ASSERT_TRUE(lock != NULL);
  fclose(lock);
  os_mutex_destroy(&c->err_mu);
  os_rwlock_destroy(&c->lock);
  free(c->root); free(c);
  asngn_test_rmtree(root);
}
TEST_LIST = {TEST_ENTRY(reservations_survive_failure_and_reopen)};
RUN_ALL_TESTS()
