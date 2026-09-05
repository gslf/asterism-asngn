/* Untrusted text cannot turn a read or missing status into a verification. */
#include "asngn_test.h"
#include "asngn_internal.h"

TEST(workflow_identity) {
  ASSERT_TRUE(asngn_verification_command("project", "test", "{}"));
  ASSERT_TRUE(asngn_verification_command("project@1", "build", "{}"));
  ASSERT_TRUE(!asngn_verification_command("fs", "read", "test.c"));
  ASSERT_TRUE(!asngn_verification_command("proc", "run", "pytest"));
  ASSERT_TRUE(!asngn_verification_command("project", "format", "{}"));
  ASSERT_TRUE(!asngn_verification_command("project-fake", "test", "{}"));
}

TEST(typed_exit_status) {
  ASSERT_TRUE(asngn_verification_result_ok("{verification_schema:1, verification_status:\"passed\", exit_code:0}"));
  ASSERT_TRUE(!asngn_verification_result_ok("{exit_code:0}"));
  ASSERT_TRUE(!asngn_verification_result_ok(NULL));
  ASSERT_TRUE(!asngn_verification_result_ok("{}"));
  ASSERT_TRUE(!asngn_verification_result_ok("{exit_code: 1}"));
  ASSERT_TRUE(!asngn_verification_result_ok("{exit_code: \"0\"}"));
  ASSERT_TRUE(!asngn_verification_result_ok("{output: \"exit_code: 0\"}"));
  ASSERT_TRUE(!asngn_verification_result_ok("{nested: {exit_code: 0}}"));
  ASSERT_TRUE(!asngn_verification_result_ok("{exit_code: 0} {exit_code: 1}"));
}

TEST(snapshot_and_containment) {
  char root[256], path[512];
  asngn_ctx *c = calloc(1, sizeof *c);
  asngn_session session;
  asngn_turn_state turn;
  char *text = NULL;
  size_t len = 0;
  ASSERT_TRUE(c != NULL && asngn_test_tmpdir(root));
  memset(&session, 0, sizeof session);
  memset(&turn, 0, sizeof turn);
  session.ctx = c;
  turn.s = &session;
  snprintf(c->workspace.canonical_root, sizeof c->workspace.canonical_root, "%s", root);
  snprintf(c->workspace.repository_root, sizeof c->workspace.repository_root, "%s", root);
  snprintf(path, sizeof path, "%s/source.c", root);
  ASSERT_OK(os_write_file(path, "before", 6));
  ASSERT_OK(asngn_workspace_refresh(c));
  memcpy(turn.verification_snapshot, c->workspace.fingerprint, 65);
  turn.verification_ok = true;
  ASSERT_TRUE(asngn_verification_current(&turn));
  ASSERT_OK(os_write_file(path, "after", 5));
  ASSERT_TRUE(!asngn_verification_current(&turn));
  ASSERT_TRUE(asngn_workspace_read(root, "../outside", 32, &text, &len) != ASNGN_OK);
  ASSERT_TRUE(asngn_workspace_read(root, "source.c", 2, &text, &len) == ASNGN_ERR_LIMIT);
  ASSERT_OK(asngn_workspace_read(root, "source.c", 32, &text, &len));
  ASSERT_EQ_STR(text, "after");
  free(text);
#ifndef _WIN32
  snprintf(path, sizeof path, "%s/alias", root);
  ASSERT_EQ_INT(symlink("/etc/passwd", path), 0);
  ASSERT_TRUE(asngn_workspace_read(root, "alias", 32768, &text, &len) != ASNGN_OK);
#endif
  asngn_test_rmtree(root);
  free(c);
}

TEST_LIST = {
  TEST_ENTRY(snapshot_and_containment),
  TEST_ENTRY(workflow_identity),
  TEST_ENTRY(typed_exit_status),
};
RUN_ALL_TESTS()
