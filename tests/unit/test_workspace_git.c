/* Git metadata cannot redirect snapshot reads or fabricate a valid identity. */
#include "asngn_test.h"
#include "workspace_git.h"

static const char oid[] = "0123456789abcdef0123456789abcdef01234567";
typedef struct { char root[256]; } fixture;
static asngn_err put(fixture *f, const char *name, const char *text) {
  char *path = os_path_join(f->root, name);
  if (!path) return ASNGN_ERR_NOMEM;
  char *slash = strrchr(path, '/');
  asngn_err e = ASNGN_OK;
  if (slash) { *slash = 0; e = os_mkdir_p(path); *slash = '/'; }
  if (e == ASNGN_OK) e = os_write_file(path, text, strlen(text));
  free(path);
  return e;
}
static asngn_err snapshot(fixture *f, const char *directory, asngn_workspace_info *w) {
  memset(w, 0, sizeof *w);
  char *path = os_path_join(f->root, directory);
  if (!path) return ASNGN_ERR_NOMEM;
  snprintf(w->repository_root, sizeof w->repository_root, "%s", path);
  snprintf(w->canonical_root, sizeof w->canonical_root, "%s", path);
  free(path);
  strcpy(w->fingerprint, "obsolete");
  return asngn_workspace_snapshot(w, NULL);
}
static bool setup(fixture *f) {
  return asngn_test_tmpdir(f->root) &&
         put(f, "main/.git/HEAD", "ref: refs/heads/feature/review\n") == ASNGN_OK &&
         put(f, "main/.git/refs/heads/feature/review", oid) == ASNGN_OK;
}
TEST(loose_packed_unborn_and_detached_identity) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(snapshot(&f, "main", &w));
  ASSERT_EQ_STR(w.branch, "feature/review");
  ASSERT_EQ_STR(w.head, oid);
  char *ref = os_path_join(f.root, "main/.git/refs/heads/feature/review");
  ASSERT_TRUE(ref);
  ASSERT_OK(os_remove_file(ref));
  free(ref);
  ASSERT_OK(snapshot(&f, "main", &w));
  ASSERT_EQ_STR(w.head, "");
  char packed[256];
  snprintf(packed, sizeof packed, "# pack-refs with: peeled fully-peeled sorted\n%s refs/heads/feature/review\n", oid);
  ASSERT_OK(put(&f, "main/.git/packed-refs", packed));
  ASSERT_OK(snapshot(&f, "main", &w));
  ASSERT_EQ_STR(w.head, oid);
  ASSERT_OK(put(&f, "main/.git/HEAD", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"));
  ASSERT_OK(snapshot(&f, "main", &w));
  ASSERT_EQ_STR(w.branch, "detached");
  ASSERT_EQ_INT(strlen(w.head), 64);
  asngn_test_rmtree(f.root);
}
TEST(empty_ancestor_marker_does_not_claim_a_repository) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(&f, ".git/placeholder", "not a repository"));
  ASSERT_OK(put(&f, "loose/code/source.c", "source"));
  char *nested = os_path_join(f.root, "loose/code");
  asngn_ctx *c = calloc(1, sizeof *c);
  ASSERT_TRUE(c && nested);
  os_mutex_init(&c->err_mu);
  ASSERT_OK(asngn_workspace_info_init(c, nested, &w));
  ASSERT_EQ_STR(w.repository_root, nested);
  ASSERT_TRUE(!w.head[0] && !w.branch[0] && w.fingerprint[0]);
  free(nested);
  ASSERT_OK(put(&f, "main/src/source.c", "source"));
  nested = os_path_join(f.root, "main/src");
  ASSERT_TRUE(nested);
  ASSERT_OK(asngn_workspace_info_init(c, nested, &w));
  ASSERT_EQ_STR(w.head, oid);
  ASSERT_EQ_STR(w.branch, "feature/review");
  os_mutex_destroy(&c->err_mu);
  free(c); free(nested);
  asngn_test_rmtree(f.root);
}
TEST(reference_paths_and_oid_payloads_are_strict) {
  const char *invalid[] = {"ref: ../secret\n", "ref: refs/heads/../../secret\n",
    "ref: /etc/secret\n", "ref: refs/heads/name.lock\n", "ref: refs//head\n",
    "ref: refs/heads/x\nsecond-line\n", "SECRET_UNSEEN", "ref: refs/heads/.hidden\n"};
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(&f, "main/secret", "SECRET_UNSEEN"));
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
    ASSERT_OK(put(&f, "main/.git/HEAD", invalid[i]));
    ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_PARSE);
    ASSERT_TRUE(!w.head[0] && !w.branch[0] && !w.fingerprint[0]);
  }
  asngn_test_rmtree(f.root);
}
TEST(symbolic_chain_is_bounded) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(&f, "main/.git/HEAD", "ref: refs/heads/link\n"));
  ASSERT_OK(put(&f, "main/.git/refs/heads/link", "ref: refs/heads/feature/review\n"));
  ASSERT_OK(snapshot(&f, "main", &w));
  ASSERT_EQ_STR(w.head, oid);
  ASSERT_EQ_STR(w.branch, "link");
  ASSERT_OK(put(&f, "main/.git/refs/heads/link", "ref: refs/heads/link\n"));
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_LIMIT);
  ASSERT_TRUE(!w.fingerprint[0]);
  asngn_test_rmtree(f.root);
}
TEST(binary_metadata_and_long_branch_names_do_not_get_truncated) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  char payload[160] = "ref: refs/heads/";
  size_t start = strlen(payload);
  memset(payload + start, 'x', sizeof payload - start - 1);
  payload[sizeof payload - 1] = 0;
  ASSERT_OK(put(&f, "main/.git/HEAD", payload));
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_LIMIT);
  char *head = os_path_join(f.root, "main/.git/HEAD");
  ASSERT_TRUE(head);
  memcpy(payload, oid, strlen(oid) + 1);
  memcpy(payload + strlen(oid) + 1, "hidden", 6);
  ASSERT_OK(os_write_file(head, payload, strlen(oid) + 7));
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_PARSE);
  free(head);
  ASSERT_OK(put(&f, "main/.git/HEAD", oid));
  ASSERT_OK(put(&f, "main/.git/commondir", "../../external\n"));
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_UNSUPPORTED);
  ASSERT_TRUE(!w.head[0] && !w.fingerprint[0]);
  asngn_test_rmtree(f.root);
}
TEST(duplicate_packed_reference_is_not_an_arbitrary_choice) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(&f, "main/.git/HEAD", "ref: refs/heads/packed\n"));
  char packed[256];
  snprintf(packed, sizeof packed, "%s refs/heads/packed\n%s refs/heads/packed\n", oid, oid);
  ASSERT_OK(put(&f, "main/.git/packed-refs", packed));
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_PARSE);
  ASSERT_TRUE(!w.head[0] && !w.fingerprint[0]);
  asngn_test_rmtree(f.root);
}
TEST(metadata_size_and_format_limits_fail_closed) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  char oversized[ASNGN_GIT_LINE_MAX + 2];
  memset(oversized, 'x', sizeof oversized - 1);
  oversized[sizeof oversized - 1] = 0;
  ASSERT_OK(put(&f, "main/.git/HEAD", oversized));
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_LIMIT);
  ASSERT_OK(put(&f, "main/.git/HEAD", "ref: refs/heads/packed\n"));
  char *path = os_path_join(f.root, "main/.git/packed-refs");
  ASSERT_TRUE(path);
  FILE *file = os_fopen(path, "wb");
  ASSERT_TRUE(file);
  ASSERT_EQ_INT(fseek(file, ASNGN_GIT_PACKED_MAX, SEEK_SET), 0);
  ASSERT_EQ_INT(fputc('x', file), 'x');
  ASSERT_EQ_INT(fclose(file), 0);
  free(path);
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_LIMIT);
  ASSERT_OK(put(&f, "main/.git/reftable/tables.list", "table\n"));
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_UNSUPPORTED);
  asngn_test_rmtree(f.root);
}
TEST(linked_worktree_uses_the_registered_common_store) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(&f, "job/.git", "gitdir: ../main/.git/worktrees/job\n"));
  ASSERT_OK(put(&f, "main/.git/worktrees/job/HEAD", "ref: refs/heads/feature/review\n"));
  ASSERT_OK(put(&f, "main/.git/worktrees/job/commondir", "../..\n"));
  ASSERT_OK(put(&f, "main/.git/worktrees/job/gitdir", "../../../../job/.git\n"));
  ASSERT_OK(snapshot(&f, "job", &w));
  ASSERT_EQ_STR(w.head, oid);
  ASSERT_EQ_STR(w.branch, "feature/review");
  ASSERT_OK(put(&f, "main/.git/worktrees/job/HEAD", "ref: refs/worktree/isolated\n"));
  ASSERT_OK(put(&f, "main/.git/worktrees/job/refs/worktree/isolated", oid));
  ASSERT_OK(put(&f, "main/.git/refs/worktree/isolated", "WRONG_STORE"));
  ASSERT_OK(snapshot(&f, "job", &w));
  ASSERT_EQ_STR(w.head, oid);
  char *registration = os_path_join(f.root, "main/.git/worktrees/job/gitdir");
  ASSERT_TRUE(registration);
  ASSERT_OK(os_remove_file(registration));
  free(registration);
  ASSERT_ERR(snapshot(&f, "job", &w), ASNGN_ERR_PARSE);
  ASSERT_TRUE(!w.fingerprint[0]);
  ASSERT_OK(put(&f, "main/.git/worktrees/job/gitdir", "../../HEAD\n"));
  ASSERT_ERR(snapshot(&f, "job", &w), ASNGN_ERR_DENIED);
  ASSERT_OK(put(&f, "main/.git/worktrees/job/gitdir", "../../../../job/.git\n"));
  ASSERT_OK(put(&f, "main/.git/worktrees/job/commondir", "../../../..\n"));
  ASSERT_ERR(snapshot(&f, "job", &w), ASNGN_ERR_DENIED);
  ASSERT_OK(put(&f, "job/.git", "gitdir: ../main/.git\n"));
  ASSERT_ERR(snapshot(&f, "job", &w), ASNGN_ERR_UNSUPPORTED);
  asngn_test_rmtree(f.root);
}
#ifndef _WIN32
TEST(metadata_aliases_and_special_files_are_never_read) {
  fixture f;
  asngn_workspace_info w;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(&f, "external", oid));
  char *head = os_path_join(f.root, "main/.git/HEAD"), *target = os_path_join(f.root, "external");
  ASSERT_TRUE(head && target);
  ASSERT_OK(os_remove_file(head));
  ASSERT_EQ_INT(symlink(target, head), 0);
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_DENIED);
  ASSERT_OK(os_remove_file(head));
  ASSERT_EQ_INT(mkfifo(head, 0600), 0);
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_DENIED);
  ASSERT_OK(os_remove_file(head));
  ASSERT_OK(put(&f, "main/.git/HEAD", "ref: refs/heads/alias\n"));
  free(head);
  head = os_path_join(f.root, "main/.git/refs/heads/alias");
  ASSERT_TRUE(head);
  ASSERT_EQ_INT(symlink(target, head), 0);
  ASSERT_ERR(snapshot(&f, "main", &w), ASNGN_ERR_DENIED);
  ASSERT_TRUE(!w.fingerprint[0]);
  free(head);
  free(target);
  asngn_test_rmtree(f.root);
}
#endif
TEST_LIST = {
  TEST_ENTRY(loose_packed_unborn_and_detached_identity),
  TEST_ENTRY(empty_ancestor_marker_does_not_claim_a_repository),
  TEST_ENTRY(reference_paths_and_oid_payloads_are_strict),
  TEST_ENTRY(symbolic_chain_is_bounded),
  TEST_ENTRY(binary_metadata_and_long_branch_names_do_not_get_truncated),
  TEST_ENTRY(duplicate_packed_reference_is_not_an_arbitrary_choice),
  TEST_ENTRY(metadata_size_and_format_limits_fail_closed),
  TEST_ENTRY(linked_worktree_uses_the_registered_common_store),
#ifndef _WIN32
  TEST_ENTRY(metadata_aliases_and_special_files_are_never_read),
#endif
};
RUN_ALL_TESTS()
