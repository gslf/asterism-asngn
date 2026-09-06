/* Real filesystem limits, aliases and edits during descriptor-based traversal. */
#include "asngn_test.h"
#include "workspace_tree.h"
#include "retrieval.h"

typedef struct {
  char root[256], outside[256];
  asngn_buf seen;
  size_t visits, read_bytes;
  bool cancel, cancel_after_one;
  bool return_limit;
  int mutation;
} fixture;
static bool setup(fixture *f) {
  memset(f, 0, sizeof *f);
  return asngn_test_tmpdir(f->root) && asngn_test_tmpdir(f->outside);
}
static void drop(fixture *f) {
  asngn_buf_free(&f->seen);
  asngn_test_rmtree(f->root);
  asngn_test_rmtree(f->outside);
}
static asngn_err put(const char *root, const char *relative, const char *text) {
  char *path = os_path_join(root, relative);
  if (!path) return ASNGN_ERR_NOMEM;
  char *slash = strrchr(path, '/');
  asngn_err e = ASNGN_OK;
  if (slash) {
    *slash = 0;
    e = os_mkdir_p(path);
    *slash = '/';
  }
  if (e == ASNGN_OK) e = os_write_file(path, text, strlen(text));
  free(path);
  return e;
}
static bool stopped(void *userdata) { return ((fixture *)userdata)->cancel; }
static asngn_err collect(const char *relative, FILE *file, size_t bytes, void *userdata) {
  fixture *f = userdata;
  char *text = NULL;
  size_t len = 0;
  asngn_err e = asngn_tree_read(file, bytes, 262144, &text, &len);
  if (e == ASNGN_OK) e = asngn_buf_printf(&f->seen, "%s:%s\n", relative, text);
  free(text);
  f->visits++;
  f->read_bytes += len;
  if (f->cancel_after_one) f->cancel = true;
  if (f->mutation == 1) e = put(f->root, relative, "changed length during read");
  if (f->mutation == 2) e = put(f->root, "added-during-scan", "new file");
#ifndef _WIN32
  if (f->mutation == 3 && !strcmp(relative, "z/a.trigger")) {
    char *old = os_path_join(f->root, "z"), *moved = os_path_join(f->outside, "moved"),
         *foreign = os_path_join(f->outside, "foreign");
    if (!old || !moved || !foreign) e = ASNGN_ERR_NOMEM;
    else if (rename(old, moved) || symlink(foreign, old)) e = ASNGN_ERR_IO;
    free(old);
    free(moved);
    free(foreign);
  }
#endif
  return e == ASNGN_OK && f->return_limit ? ASNGN_ERR_LIMIT : e;
}
TEST(shared_ignores_keep_configuration_and_dependencies) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, "build/ignored.c", "generated"));
  ASSERT_OK(put(f.root, ".git/ignored", "metadata"));
  ASSERT_OK(put(f.root, ".github/workflows/ci.yml", "workflow"));
  ASSERT_OK(put(f.root, "deps/api.h", "contract"));
  ASSERT_OK(put(f.root, "src/main.c", "code"));
  ASSERT_OK(put(f.root, "scripts/build", "script"));
  asngn_tree_stats stats = {0};
  ASSERT_OK(asngn_tree_walk(f.root, NULL, collect, NULL, &f, &stats));
  ASSERT_EQ_STR(f.seen.data, ".github/workflows/ci.yml:workflow\ndeps/api.h:contract\nscripts/"
                             "build:script\nsrc/main.c:code\n");
  ASSERT_EQ_INT(stats.files, 4);
  ASSERT_EQ_INT(stats.ignored, 2);
  ASSERT_TRUE(asngn_tree_ignored_path("build/main.c"));
  ASSERT_TRUE(!asngn_tree_ignored_path("scripts/build"));
  ASSERT_TRUE(!asngn_tree_ignored_path("builder/main.c"));
  drop(&f);
}
TEST(flat_entry_limit_applies_during_enumeration) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  for (int i = 0; i < 4; i++) {
    char name[20];
    snprintf(name, sizeof name, "file%d", i);
    ASSERT_OK(put(f.root, name, "123456"));
  }
  asngn_tree_limits limits = asngn_tree_default_limits;
  limits.entries = 3;
  asngn_tree_stats stats = {0};
  ASSERT_ERR(asngn_tree_walk(f.root, &limits, collect, NULL, &f, &stats), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(stats.entries, 3);
  ASSERT_EQ_INT(f.visits, 0);
  drop(&f);
}
TEST(byte_limits_apply_before_file_callbacks) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, "a", "123456"));
  ASSERT_OK(put(f.root, "b", "123456"));
  ASSERT_OK(put(f.root, "c", "123456"));
  asngn_tree_limits limits = asngn_tree_default_limits;
  limits.file_bytes = 5;
  ASSERT_ERR(asngn_tree_walk(f.root, &limits, collect, NULL, &f, NULL), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(f.read_bytes, 0);
  limits.file_bytes = 6;
  limits.total_bytes = 12;
  asngn_tree_stats stats = {0};
  ASSERT_ERR(asngn_tree_walk(f.root, &limits, collect, NULL, &f, &stats), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(stats.bytes, 12);
  ASSERT_EQ_INT(f.visits, 2);
  ASSERT_EQ_INT(f.read_bytes, 12);
  drop(&f);
}
TEST(nested_limits_are_global) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, "a/one", "12345"));
  ASSERT_OK(put(f.root, "a/two", "12345"));
  ASSERT_OK(put(f.root, "b/three", "12345"));
  asngn_tree_limits limits = asngn_tree_default_limits;
  limits.total_bytes = 10;
  ASSERT_ERR(asngn_tree_walk(f.root, &limits, collect, NULL, &f, NULL), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(f.read_bytes, 10);
  limits = asngn_tree_default_limits;
  limits.depth = 0;
  ASSERT_ERR(asngn_tree_walk(f.root, &limits, collect, NULL, &f, NULL), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(f.read_bytes, 10);
  drop(&f);
}
TEST(cancel_stops_before_the_next_file) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, "a", "first"));
  ASSERT_OK(put(f.root, "b", "second"));
  f.cancel_after_one = true;
  ASSERT_ERR(asngn_tree_walk(f.root, NULL, collect, stopped, &f, NULL), ASNGN_ERR_CANCELLED);
  ASSERT_EQ_INT(f.visits, 1);
  drop(&f);
}
TEST(changed_files_and_directories_do_not_finish_as_stable) {
  for (int mutation = 1; mutation <= 2; mutation++) {
    for (int limited = 0; limited <= 1; limited++) {
      fixture f;
      ASSERT_TRUE(setup(&f));
      ASSERT_OK(put(f.root, "a", "first"));
      f.mutation = mutation;
      f.return_limit = limited != 0;
      ASSERT_ERR(asngn_tree_walk(f.root, NULL, collect, NULL, &f, NULL), ASNGN_ERR_BUSY);
      drop(&f);
    }
  }
}
#ifndef _WIN32
TEST(aliases_and_special_files_never_reach_the_reader) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, "normal", "visible"));
  ASSERT_OK(put(f.outside, "secret", "SECRET_UNSEEN"));
  char path[512], target[512];
  snprintf(path, sizeof path, "%s/alias-dir", f.root);
  ASSERT_EQ_INT(symlink(f.outside, path), 0);
  snprintf(path, sizeof path, "%s/alias-file", f.root);
  snprintf(target, sizeof target, "%s/secret", f.outside);
  ASSERT_EQ_INT(symlink(target, path), 0);
  snprintf(path, sizeof path, "%s/cycle", f.root);
  ASSERT_EQ_INT(symlink(f.root, path), 0);
  snprintf(path, sizeof path, "%s/pipe", f.root);
  ASSERT_EQ_INT(mkfifo(path, 0600), 0);
  asngn_tree_stats stats = {0};
  ASSERT_OK(asngn_tree_walk(f.root, NULL, collect, NULL, &f, &stats));
  ASSERT_EQ_STR(f.seen.data, "normal:visible\n");
  ASSERT_EQ_INT(stats.excluded, 4);
  asngn_workspace_info workspace = {0};
  snprintf(workspace.canonical_root, sizeof workspace.canonical_root, "%s", f.root);
  snprintf(workspace.repository_root, sizeof workspace.repository_root, "%s", f.root);
  strcpy(workspace.fingerprint, "obsolete");
  asngn_repo_stats census = {.loaded = true};
  ASSERT_ERR(asngn_workspace_snapshot(&workspace, &census), ASNGN_ERR_DENIED);
  ASSERT_TRUE(!workspace.fingerprint[0] && !census.loaded);
  drop(&f);
}
TEST(replaced_ancestor_cannot_redirect_an_open_directory) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, "z/a.trigger", "trigger"));
  ASSERT_OK(put(f.root, "z/b.safe", "ORIGINAL"));
  ASSERT_OK(put(f.outside, "foreign/b.safe", "SECRET_UNSEEN"));
  f.mutation = 3;
  ASSERT_ERR(asngn_tree_walk(f.root, NULL, collect, NULL, &f, NULL), ASNGN_ERR_BUSY);
  ASSERT_TRUE(strstr(f.seen.data, "SECRET_UNSEEN") == NULL);
  ASSERT_TRUE(strstr(f.seen.data, "ORIGINAL") != NULL);
  drop(&f);
}
#endif
TEST(snapshot_tracks_content_rename_and_deletion) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, "one.c", "before"));
  asngn_workspace_info workspace = {0};
  snprintf(workspace.canonical_root, sizeof workspace.canonical_root, "%s", f.root);
  snprintf(workspace.repository_root, sizeof workspace.repository_root, "%s", f.root);
  char previous[65];
  ASSERT_OK(asngn_workspace_snapshot(&workspace, NULL));
  strcpy(previous, workspace.fingerprint);
  ASSERT_OK(asngn_workspace_snapshot(&workspace, NULL));
  ASSERT_EQ_STR(previous, workspace.fingerprint);
  ASSERT_OK(put(f.root, "one.c", "AFTER!"));
  ASSERT_OK(asngn_workspace_snapshot(&workspace, NULL));
  ASSERT_TRUE(strcmp(previous, workspace.fingerprint));
  strcpy(previous, workspace.fingerprint);
  char *one = os_path_join(f.root, "one.c"), *two = os_path_join(f.root, "two.c");
  ASSERT_OK(os_rename(one, two));
  ASSERT_OK(asngn_workspace_snapshot(&workspace, NULL));
  ASSERT_TRUE(strcmp(previous, workspace.fingerprint));
  strcpy(previous, workspace.fingerprint);
  ASSERT_OK(os_remove_file(two));
  asngn_repo_stats stats = {0};
  ASSERT_OK(asngn_workspace_snapshot(&workspace, &stats));
  ASSERT_TRUE(strcmp(previous, workspace.fingerprint));
  ASSERT_TRUE(stats.loaded && !stats.files);
  free(one);
  free(two);
  drop(&f);
}
TEST(retrieval_has_source_versions_and_shared_ignore_policy) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  ASSERT_OK(put(f.root, ".github/workflows/ci.yml", "name: workflow\n"));
  ASSERT_OK(put(f.root, "build/active.c", "excluded even when active\n"));
  ASSERT_OK(put(f.root, "main.c", "αβγ\nline2\n"));
  asngn_ctx *c = calloc(1, sizeof *c);
  ASSERT_TRUE(c);
  os_mutex_init(&c->tele_mu);
  asngn_session session = {.active_file = "build/active.c"};
  snprintf(session.workspace.canonical_root, sizeof session.workspace.canonical_root, "%s", f.root);
  asngn_turn_state turn = {.s = &session};
  code_index *ix = calloc(1, sizeof *ix);
  ASSERT_TRUE(ix);
  ix->v = calloc(CODE_MAX, sizeof *ix->v);
  ASSERT_TRUE(ix->v);
  ASSERT_OK(asngn_code_scan(c, &turn, ix));
  ASSERT_EQ_INT(ix->n, 2);
  ASSERT_EQ_STR(ix->v[0].path, ".github/workflows/ci.yml");
  ASSERT_EQ_STR(ix->v[1].path, "main.c");
  ASSERT_EQ_INT(ix->v[1].line, 1);
  ASSERT_EQ_INT(ix->v[1].end_line, 2);
  uint8_t hash[32];
  asngn_sha256("αβγ\nline2\n", strlen("αβγ\nline2\n"), hash);
  ASSERT_TRUE(!memcmp(hash, ix->v[1].file_hash, 32));
  asngn_code_index_free(ix);
  os_mutex_destroy(&c->tele_mu);
  free(c);
  drop(&f);
}
TEST(active_file_precedes_the_corpus_cap) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  char *body = malloc(240001);
  ASSERT_TRUE(body);
  memset(body, 'x', 240000);
  body[240000] = 0;
  for (int i = 0; i < 9; i++) {
    char name[32];
    snprintf(name, sizeof name, "a%02d.c", i);
    ASSERT_OK(put(f.root, name, body));
  }
  free(body);
  ASSERT_OK(put(f.root, "zz.c", "ACTIVE_FILE\n"));
  asngn_ctx *c = calloc(1, sizeof *c);
  ASSERT_TRUE(c);
  os_mutex_init(&c->tele_mu);
  c->cfg.tele_ring = 8;
  ASSERT_OK(asngn_tele_init(c));
  asngn_session session = {.active_file = "zz.c"};
  snprintf(session.workspace.canonical_root, sizeof session.workspace.canonical_root, "%s", f.root);
  asngn_turn_state turn = {.s = &session};
  code_index *ix = calloc(1, sizeof *ix);
  ASSERT_TRUE(ix);
  ix->v = calloc(CODE_MAX, sizeof *ix->v);
  ASSERT_TRUE(ix->v);
  ASSERT_ERR(asngn_code_scan(c, &turn, ix), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(ix->n, CODE_MAX);
  ASSERT_EQ_STR(ix->v[0].path, "zz.c");
  char **events = NULL;
  size_t count = 0;
  ASSERT_OK(asngn_tele_tail(c, 1, &events, &count));
  ASSERT_EQ_INT(count, 1);
  ASSERT_TRUE(strstr(events[0], "complete:false") != NULL);
  asngn_strings_free(events, count);
  asngn_code_index_free(ix);
  asngn_tele_shutdown(c);
  os_mutex_destroy(&c->tele_mu);
  free(c);
  drop(&f);
}
TEST_LIST = {TEST_ENTRY(shared_ignores_keep_configuration_and_dependencies),
             TEST_ENTRY(flat_entry_limit_applies_during_enumeration),
             TEST_ENTRY(byte_limits_apply_before_file_callbacks),
             TEST_ENTRY(nested_limits_are_global),
             TEST_ENTRY(cancel_stops_before_the_next_file),
             TEST_ENTRY(changed_files_and_directories_do_not_finish_as_stable),
#ifndef _WIN32
             TEST_ENTRY(aliases_and_special_files_never_reach_the_reader),
             TEST_ENTRY(replaced_ancestor_cannot_redirect_an_open_directory),
#endif
             TEST_ENTRY(snapshot_tracks_content_rename_and_deletion),
             TEST_ENTRY(retrieval_has_source_versions_and_shared_ignore_policy),
             TEST_ENTRY(active_file_precedes_the_corpus_cap)};
RUN_ALL_TESTS()
