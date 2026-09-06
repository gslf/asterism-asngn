/* Bounded repository admission on actual workspace files. */
#include "asngn_test.h"
#include "retrieval.h"
#include "retrieval_select.h"
#include "workspace_tree.h"
#include "xcdn.h"
typedef struct {
  char root[256];
} fixture;
static bool setup(fixture *f) { return asngn_test_tmpdir(f->root); }
static void drop(fixture *f) { asngn_test_rmtree(f->root); }
static asngn_err put(const char *root, const char *relative, const char *text) {
  char *path = os_path_join(root, relative);
  if (!path)
    return ASNGN_ERR_NOMEM;
  char *slash = strrchr(path, '/');
  asngn_err e = ASNGN_OK;
  if (slash) {
    *slash = 0;
    e = os_mkdir_p(path);
    *slash = '/';
  }
  if (e == ASNGN_OK)
    e = os_write_file(path, text, strlen(text));
  free(path);
  return e;
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
  ASSERT_EQ_INT(ix->n, 9 * CODE_FILE_CHUNKS + 1);
  ASSERT_EQ_STR(ix->v[0].path, "zz.c");
  char **events = NULL;
  size_t count = 0;
  ASSERT_OK(asngn_tele_tail(c, 1, &events, &count));
  ASSERT_EQ_INT(count, 1);
  ASSERT_TRUE(strstr(events[0], "complete:false") != NULL);
  ASSERT_TRUE(strstr(events[0], "traversal_complete:true") != NULL);
  asngn_strings_free(events, count);
  asngn_code_index_free(ix);
  asngn_tele_shutdown(c);
  os_mutex_destroy(&c->tele_mu);
  free(c);
  drop(&f);
}
TEST(late_evidence_survives_early_corpus_pressure) {
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
  const char *evidence = "int recover(void) { return ERR_TORN_WRITE; }\n";
  ASSERT_OK(put(f.root, "zz_recovery.c", evidence));
  asngn_ctx *c = calloc(1, sizeof *c);
  ASSERT_TRUE(c);
  os_mutex_init(&c->tele_mu);
  asngn_session session = {0};
  snprintf(session.workspace.canonical_root, sizeof session.workspace.canonical_root, "%s", f.root);
  asngn_turn_state turn = {.s = &session, .retrieval_query = "ERR_TORN_WRITE"};
  code_index *ix = calloc(1, sizeof *ix);
  ASSERT_TRUE(ix);
  ix->v = calloc(CODE_MAX, sizeof *ix->v);
  ASSERT_TRUE(ix->v);
  asngn_err error = asngn_code_scan(c, &turn, ix);
  ASSERT_TRUE(error == ASNGN_OK || error == ASNGN_ERR_LIMIT);
  bool found = false;
  for (size_t i = 0; i < ix->n; i++)
    if (!strcmp(ix->v[i].path, "zz_recovery.c")) {
      found = true;
      ASSERT_EQ_STR(ix->v[i].text, evidence);
    }
  ASSERT_TRUE(found);
  asngn_code_index_free(ix);
  os_mutex_destroy(&c->tele_mu);
  free(c);
  drop(&f);
}
/* Exercise capacity and exact ranges without depending on a model score. */
TEST(global_capacity_keeps_late_query_matches_and_final_source_ranges) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  char body[20001];
  for (size_t i = 0; i < 20000; i++)
    body[i] = i % 80 == 79 ? '\n' : 'x';
  body[20000] = 0;
  for (int i = 0; i < 140; i++) {
    char name[32];
    snprintf(name, sizeof name, "a%03d.c", i);
    ASSERT_OK(put(f.root, name, body));
  }
  const char *needle = "ERR_TORN_WRITE 失敗_原因";
  memcpy(body + 19000, needle, strlen(needle));
  body[18999] = '\n';
  body[19000 + strlen(needle)] = '\n';
  ASSERT_OK(put(f.root, "zz.c", body));
  asngn_ctx *c = calloc(1, sizeof *c);
  ASSERT_TRUE(c);
  os_mutex_init(&c->tele_mu);
  asngn_session session = {0};
  snprintf(session.workspace.canonical_root, sizeof session.workspace.canonical_root, "%s", f.root);
  asngn_turn_state turn = {.s = &session, .retrieval_query = (char *)needle};
  code_index *ix = calloc(1, sizeof *ix);
  ASSERT_TRUE(ix);
  ix->v = calloc(CODE_MAX, sizeof *ix->v);
  ASSERT_TRUE(ix->v);
  ASSERT_ERR(asngn_code_scan(c, &turn, ix), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(ix->n, CODE_MAX);
  size_t seen = 0;
  uint8_t hash[32];
  asngn_sha256(body, strlen(body), hash);
  for (size_t i = 0; i < ix->n; i++) {
    chunk *v = &ix->v[i];
    if (strcmp(v->path, "zz.c"))
      continue;
    seen++;
    ASSERT_TRUE(strstr(v->text, needle) != NULL);
    ASSERT_TRUE(v->offset > 16000);
    ASSERT_TRUE(!memcmp(body + v->offset, v->text, strlen(v->text)));
    ASSERT_TRUE(!memcmp(v->file_hash, hash, 32));
    ASSERT_TRUE(asngn_utf8_valid(v->text, strlen(v->text)));
    size_t line = 1;
    for (size_t j = 0; j < v->offset; j++)
      if (body[j] == '\n')
        line++;
    ASSERT_EQ_INT(v->line, line);
  }
  ASSERT_EQ_INT(seen, 1);
  asngn_code_index_free(ix);
  os_mutex_destroy(&c->tele_mu);
  free(c);
  drop(&f);
}
TEST(admission_rejects_stale_active_bytes_and_never_crosses_its_stop) {
  asngn_ctx *c = calloc(1, sizeof *c);
  ASSERT_TRUE(c);
  asngn_session session = {.active_file = "active.txt"};
  asngn_turn_state turn = {.s = &session};
  code_index *ix = calloc(1, sizeof *ix);
  ASSERT_TRUE(ix);
  ix->v = calloc(CODE_MAX, sizeof *ix->v);
  ASSERT_TRUE(ix->v);
  code_selection s = {.c = c, .turn = &turn, .index = ix};
  ASSERT_OK(asngn_code_select_file(&s, "active.txt", "old", 3, true));
  ASSERT_ERR(asngn_code_select_file(&s, "active.txt", "x\0y", 3, false), ASNGN_ERR_BUSY);
  ASSERT_EQ_INT(ix->n, 1);
  turn.cancel = true;
  ASSERT_ERR(asngn_code_select_file(&s, "later.c", "new", 3, false), ASNGN_ERR_CANCELLED);
  ASSERT_EQ_INT(ix->n, 1);
  turn.cancel = false;
  turn.deadline_mono = 1;
  ASSERT_ERR(asngn_code_select_file(&s, "later.c", "new", 3, false), ASNGN_ERR_TIMEOUT);
  ASSERT_EQ_INT(ix->n, 1);
  asngn_code_index_free(ix);
  free(c);
}
TEST(content_budget_is_independent_of_metadata_and_active_recheck) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  char *body = malloc(CODE_FILE_BYTES + 1);
  ASSERT_TRUE(body);
  memset(body, 'x', CODE_FILE_BYTES);
  body[CODE_FILE_BYTES] = 0;
  ASSERT_OK(put(f.root, "a_active.txt", body));
  for (unsigned i = 0; i < CODE_SCAN_BYTES / CODE_FILE_BYTES + 2; i++) {
    char name[32];
    snprintf(name, sizeof name, "b%03u.c", i);
    ASSERT_OK(put(f.root, name, body));
  }
  free(body);
  ASSERT_OK(put(f.root, "z_meta.json", "{\"visible\":true}"));
  asngn_ctx *c = calloc(1, sizeof *c);
  ASSERT_TRUE(c);
  os_mutex_init(&c->tele_mu);
  c->cfg.tele_ring = 8;
  ASSERT_OK(asngn_tele_init(c));
  for (int phase = 0; phase < 2; phase++) {
    asngn_session session = {.active_file = "a_active.txt"};
    if (phase) {
      char *old = os_path_join(f.root, "a_active.txt"),
           *later = os_path_join(f.root, "zz_active.txt");
      ASSERT_TRUE(old && later);
      ASSERT_OK(os_rename(old, later));
      free(old);
      free(later);
      session.active_file = "zz_active.txt";
    }
    snprintf(session.workspace.canonical_root, sizeof session.workspace.canonical_root, "%s",
             f.root);
    asngn_turn_state turn = {.s = &session};
    code_index *ix = calloc(1, sizeof *ix);
    ASSERT_TRUE(ix);
    ix->v = calloc(CODE_MAX, sizeof *ix->v);
    ASSERT_TRUE(ix->v);
    ASSERT_ERR(asngn_code_scan(c, &turn, ix), ASNGN_ERR_LIMIT);
    ASSERT_EQ_STR(ix->v[0].path, session.active_file);
    char **events = NULL;
    size_t n = 0;
    ASSERT_OK(asngn_tele_tail(c, 1, &events, &n));
    ASSERT_EQ_INT(n, 1);
    xcdn_error_t error;
    xcdn_document_t *doc = xcdn_parse(events[0], &error);
    ASSERT_TRUE(doc);
    const xcdn_node_t *data = xcdn_object_get(doc->values[0]->value, "data");
    ASSERT_TRUE(data != NULL);
    const xcdn_node_t *bytes = xcdn_object_get(data->value, "read_bytes"),
                      *walk = xcdn_object_get(data->value, "traversal_complete"),
                      *skipped = xcdn_object_get(data->value, "skipped_budget");
    ASSERT_TRUE(bytes && walk && skipped);
    ASSERT_EQ_INT(xcdn_value_as_int(bytes->value), CODE_SCAN_BYTES);
    ASSERT_TRUE(xcdn_value_as_bool(walk->value));
    ASSERT_EQ_INT(xcdn_value_as_int(skipped->value), 5);
    xcdn_document_free(doc);
    asngn_strings_free(events, n);
    asngn_code_index_free(ix);
  }
  asngn_tele_shutdown(c);
  os_mutex_destroy(&c->tele_mu);
  free(c);
  drop(&f);
}
TEST_LIST = {TEST_ENTRY(retrieval_has_source_versions_and_shared_ignore_policy),
             TEST_ENTRY(active_file_precedes_the_corpus_cap),
             TEST_ENTRY(late_evidence_survives_early_corpus_pressure),
             TEST_ENTRY(global_capacity_keeps_late_query_matches_and_final_source_ranges),
             TEST_ENTRY(admission_rejects_stale_active_bytes_and_never_crosses_its_stop),
             TEST_ENTRY(content_budget_is_independent_of_metadata_and_active_recheck)};
RUN_ALL_TESTS()
