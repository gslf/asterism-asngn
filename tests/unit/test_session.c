/*
 * test_session.c — session store: layout creation, slugs,
 * open/busy semantics, sorted listing, manifest and transcript
 * persistence, pinning and the pin limit.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"
#include "fakes.h"

/* ── shared fixture ───────────────────────────────────────────────────── */

typedef struct {
  char        root[256];
  char        cfg[300];
  fake_model  nano, light, stdm, embed;
  fake_clock  clk;
  asngn_ctx  *c;
} fx;

static int fx_write_config(fx *f, const char *extra) {
  FILE *fp;
  int ok;
  snprintf(f->cfg, sizeof f->cfg, "%s/config.xcdn", f->root);
  fp = fopen(f->cfg, "wb");
  if (fp == NULL) return 0;
  ok = fprintf(fp,
               "#asngn_config {\n"
               "  integration: { asper: { enable: true, root: \"memory\" }, "
               "astools: { enable: false, workspace: \".\" } },\n"
               "  validation: { judge: \"off\" },\n"
               "  routing: { classifier: \"heuristic\" },\n"
               "  models: { pool: [\n"
               "    { id: \"nano\", path: \"none.gguf\" },\n"
               "    { id: \"light\", path: \"none.gguf\" },\n"
               "    { id: \"std\", path: \"none.gguf\" },\n"
               "    { id: \"embed\", path: \"none.gguf\", "
               "embedding: true, dim: 16 },\n"
               "  ] },\n"
               "%s"
               "}\n",
               extra != NULL ? extra : "") > 0;
  fclose(fp);
  return ok;
}

static int fx_open_ctx_at(fx *f, const char *workspace) {
  asngn_model_iface ifaces[4];
  const char *ids[4] = { "nano", "light", "std", "embed" };
  asngn_clock clk;
  asngn_open_params p;

  ifaces[0] = fake_model_iface(&f->nano);
  ifaces[1] = fake_model_iface(&f->light);
  ifaces[2] = fake_model_iface(&f->stdm);
  ifaces[3] = fake_model_iface(&f->embed);
  clk = fake_clock_make(&f->clk);
  memset(&p, 0, sizeof p);
  p.engine_root = f->root;
  p.config_path = f->cfg;
  p.workspace_root = workspace;
  if (asngn_open_with(&p, ifaces, 4, ids, &clk, &f->c) != ASNGN_OK)
    return 0;
  asngn_set_logger(f->c, NULL, NULL);
  return 1;
}

static int fx_open_ctx(fx *f) { return fx_open_ctx_at(f, NULL); }

static int fx_setup(fx *f, const char *extra) {
  memset(f, 0, sizeof *f);
  if (!asngn_test_tmpdir(f->root)) return 0;
  fake_model_init(&f->nano);
  fake_model_init(&f->light);
  fake_model_init(&f->stdm);
  fake_model_init(&f->embed);
  fake_clock_set(&f->clk, 1755150000);
  if (!fx_write_config(f, extra)) return 0;
  return fx_open_ctx(f);
}

static void fx_drop(fx *f) {
  asngn_close(f->c);
  f->c = NULL;
  fake_model_dispose(&f->nano);
  fake_model_dispose(&f->light);
  fake_model_dispose(&f->stdm);
  fake_model_dispose(&f->embed);
  asngn_test_rmtree(f->root);
}

/* One scripted DIRECT turn: classifier heuristic (no nano pass), judge
 * off, cache disabled — exactly one generation. A SIMPLE DIRECT turn
 * with a clean ledger window starts one tier below the generator (G1),
 * so replies are popped from the light fake. reply NULL queues nothing
 * (cache-served turns). */
static int fx_turn(fx *f, asngn_session *s, const char *msg,
                   const char *reply, asngn_turn_result *out) {
  asngn_task *task = NULL;
  asngn_turn_result res;
  asngn_err e;

  if (reply != NULL && !fake_model_push(&f->light, reply)) return 0;
  if (asngn_submit(s, msg, NULL, NULL, NULL, &task) != ASNGN_OK) return 0;
  memset(&res, 0, sizeof res);
  e = asngn_task_wait(task, 30000, &res);
  asngn_task_free(task);
  if (e != ASNGN_OK) {
    asngn_turn_result_free(&res);
    return 0;
  }
  if (out != NULL) *out = res;
  else asngn_turn_result_free(&res);
  return 1;
}

static const char CACHE_OFF[] = "  cache: { enable: false },\n";

/* ── tests ────────────────────────────────────────────────────────────── */

TEST(create_layout_and_busy) {
  fx f;
  asngn_session *s = NULL, *dup = NULL;
  char path[400];

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ASSERT_EQ_STR(asngn_session_slug(s), "alpha");
  snprintf(path, sizeof path, "%s/sessions/alpha/session.xcdn", f.root);
  ASSERT_TRUE(os_file_exists(path));
  snprintf(path, sizeof path, "%s/memory", f.root);
  ASSERT_TRUE(os_file_exists(path));

  /* a second open of the same slug is BUSY while the handle lives */
  ASSERT_ERR(asngn_session_open(f.c, "alpha", &dup), ASNGN_ERR_BUSY);
  ASSERT_TRUE(dup == NULL);

  /* close releases the slug for reopening */
  asngn_session_close(s);
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(generated_and_invalid_slugs) {
  fx f;
  asngn_session *s = NULL, *g = NULL;
  const char *slug;
  size_t i;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_ERR(asngn_session_open(f.c, "Not Valid!", &s), ASNGN_ERR_INVALID);
  ASSERT_TRUE(s == NULL);
  ASSERT_ERR(asngn_session_open(f.c, "-lead", &s), ASNGN_ERR_INVALID);

  ASSERT_OK(asngn_session_open(f.c, NULL, &g));
  slug = asngn_session_slug(g);
  ASSERT_EQ_INT((long long)strlen(slug), 10);
  ASSERT_TRUE(slug[0] == 's' && slug[1] == '-');
  for (i = 2; i < 10; i++) {
    char ch = slug[i];
    ASSERT_TRUE((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
  }
  asngn_session_close(g);
  fx_drop(&f);
}

TEST(list_sorted) {
  fx f;
  asngn_session *a = NULL, *g = NULL;
  char **slugs = NULL;
  size_t n = 0;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "alpha", &a));
  ASSERT_OK(asngn_session_open(f.c, NULL, &g));
  ASSERT_OK(asngn_session_list(f.c, &slugs, &n));
  ASSERT_EQ_INT((long long)n, 2);
  /* sorted ascending: "alpha" < "s-........" */
  ASSERT_EQ_STR(slugs[0], "alpha");
  ASSERT_EQ_STR(slugs[1], asngn_session_slug(g));
  asngn_strings_free(slugs, n);
  asngn_session_close(a);
  asngn_session_close(g);
  fx_drop(&f);
}

TEST(manifest_project_persists) {
  fx f;
  asngn_session *s = NULL;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  /* Session/project metadata is restored while Asper owns memory. */
  ASSERT_OK(asngn_session_project(s, "proj-x"));
  ASSERT_EQ_STR(s->project, "proj-x");
  asngn_session_close(s);
  s = NULL;

  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ASSERT_TRUE(s->project != NULL);
  ASSERT_EQ_STR(s->project, "proj-x");
  ASSERT_EQ_INT(s->created_at, 1755150000);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(transcript_roundtrip) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_result res;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ASSERT_TRUE(fx_turn(&f, s, "How are things today", "Hello!\n", &res));
  ASSERT_EQ_STR(res.answer, "Hello!\n");
  ASSERT_EQ_INT((long long)res.turn, 2);
  ASSERT_EQ_STR(res.klass, "simple");
  ASSERT_EQ_STR(res.cache, "off");
  asngn_turn_result_free(&res);
  ASSERT_EQ_INT(f.light.calls, 1); /* simple: frugal start, one tier down */
  ASSERT_EQ_INT(f.stdm.calls, 0);
  ASSERT_EQ_INT(f.nano.calls, 0); /* heuristic classifier: no nano pass */
  asngn_session_close(s);
  s = NULL;

  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ASSERT_EQ_INT((long long)s->log_n, 2);
  ASSERT_EQ_INT((long long)s->turns, 2);
  ASSERT_EQ_STR(s->log[0].role, "user");
  ASSERT_EQ_STR(s->log[0].text, "How are things today");
  ASSERT_EQ_INT((long long)s->log[0].n, 1);
  ASSERT_EQ_STR(s->log[1].role, "assistant");
  ASSERT_EQ_STR(s->log[1].text, "Hello!\n");
  ASSERT_EQ_INT((long long)s->log[1].n, 2);
  /* route summary fields on the assistant turn */
  ASSERT_EQ_STR(s->log[1].klass, "simple");
  ASSERT_EQ_STR(s->log[1].detail, "terse");
  ASSERT_EQ_STR(s->log[1].mode, "direct");
  ASSERT_EQ_STR(s->log[1].tier, "light"); /* simple: frugal start */
  ASSERT_EQ_STR(s->log[1].cache, "off");
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(transcript_public_api) {
  fx f;
  asngn_session *s = NULL;
  asngn_transcript_entry *ents = NULL;
  size_t n = 0;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));

  /* empty session: NULL/0, still ASNGN_OK */
  ASSERT_OK(asngn_session_transcript(s, &ents, &n));
  ASSERT_TRUE(ents == NULL);
  ASSERT_EQ_INT((long long)n, 0);

  ASSERT_TRUE(fx_turn(&f, s, "How are things today", "Hello!\n", NULL));
  ASSERT_OK(asngn_session_transcript(s, &ents, &n));
  ASSERT_EQ_INT((long long)n, 2);
  ASSERT_EQ_STR(ents[0].role, "user");
  ASSERT_EQ_STR(ents[0].text, "How are things today");
  ASSERT_EQ_INT((long long)ents[0].turn, 1);
  ASSERT_EQ_STR(ents[0].tier, "");
  ASSERT_EQ_STR(ents[1].role, "assistant");
  ASSERT_EQ_STR(ents[1].text, "Hello!\n");
  ASSERT_EQ_INT((long long)ents[1].turn, 2);
  ASSERT_EQ_STR(ents[1].tier, "light");
  ASSERT_TRUE(ents[1].at > 0);
  asngn_free(ents);
  asngn_session_close(s);
  s = NULL;

  /* the replay survives a close/reopen: history comes back from disk */
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ents = NULL;
  n = 0;
  ASSERT_OK(asngn_session_transcript(s, &ents, &n));
  ASSERT_EQ_INT((long long)n, 2);
  ASSERT_EQ_STR(ents[1].text, "Hello!\n");
  asngn_free(ents);

  ASSERT_ERR(asngn_session_transcript(NULL, &ents, &n),
             ASNGN_ERR_INVALID);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(pin_persists) {
  fx f;
  asngn_session *s = NULL;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ASSERT_TRUE(fx_turn(&f, s, "How are things today", "Hello!\n", NULL));
  ASSERT_OK(asngn_session_pin(s, 1, 1));
  ASSERT_TRUE(s->log[0].pinned);
  asngn_session_close(s);
  s = NULL;

  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ASSERT_EQ_INT((long long)s->log_n, 2);
  ASSERT_TRUE(s->log[0].pinned);
  ASSERT_TRUE(!s->log[1].pinned);

  ASSERT_OK(asngn_session_pin(s, 1, 0)); /* unpin */
  ASSERT_TRUE(!s->log[0].pinned);
  ASSERT_ERR(asngn_session_pin(s, 99, 1), ASNGN_ERR_NOT_FOUND);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(pin_limit) {
  fx f;
  asngn_session *s = NULL;

  ASSERT_TRUE(fx_setup(&f,
                       "  cache: { enable: false },\n"
                       "  context: { pinned_max: 1 },\n"));
  ASSERT_OK(asngn_session_open(f.c, "alpha", &s));
  ASSERT_TRUE(fx_turn(&f, s, "How are things today", "Hello!\n", NULL));
  ASSERT_OK(asngn_session_pin(s, 1, 1));
  ASSERT_ERR(asngn_session_pin(s, 2, 1), ASNGN_ERR_INVALID);
  ASSERT_TRUE(!s->log[1].pinned);
  /* re-pinning the already pinned turn is a no-op, not an error */
  ASSERT_OK(asngn_session_pin(s, 1, 1));
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(workspace_is_bound_and_live_fingerprinted) {
  fx f;
  asngn_session *s = NULL;
  asngn_workspace_info before, after, bound;
  char *path;
  static const char body[] = "changed outside the engine\n";

  ASSERT_TRUE(fx_setup(&f, "  cache: { enable: false },\n"));
  ASSERT_OK(asngn_workspace_get(f.c, &before));
  ASSERT_TRUE(before.canonical_root[0] != '\0');
  ASSERT_TRUE(before.project_id[0] != '\0');
  ASSERT_TRUE(before.fingerprint[0] != '\0');
  path = os_path_join(before.canonical_root, "editor-change.txt");
  ASSERT_TRUE(path != NULL);
  ASSERT_OK(os_write_file(path, body, sizeof body - 1));
  ASSERT_OK(asngn_workspace_get(f.c, &after));
  ASSERT_TRUE(strcmp(before.fingerprint, after.fingerprint) != 0);
  ASSERT_OK(asngn_session_open(f.c, "workspace", &s));
  ASSERT_OK(asngn_session_workspace(s, &bound));
  ASSERT_OK(asngn_workspace_get(f.c, &after));
  ASSERT_EQ_STR(bound.canonical_root, after.canonical_root);
  ASSERT_EQ_STR(bound.fingerprint, after.fingerprint);
  asngn_session_close(s);
  free(path);
  fx_drop(&f);
}

TEST(workspace_discovers_repository_identity) {
  fx f;
  asngn_workspace_info info;
  char *git = NULL, *refs = NULL, *head = NULL, *branch = NULL, *nested = NULL;
  char *expected_root = NULL;
  static const char oid[] = "0123456789abcdef0123456789abcdef01234567\n";

  ASSERT_TRUE(fx_setup(&f, "  cache: { enable: false },\n"));
  asngn_close(f.c); f.c = NULL;
  git = os_path_join(f.root, ".git");
  refs = git != NULL ? os_path_join(git, "refs/heads") : NULL;
  head = git != NULL ? os_path_join(git, "HEAD") : NULL;
  branch = refs != NULL ? os_path_join(refs, "main") : NULL;
  nested = os_path_join(f.root, "src/module");
  ASSERT_TRUE(git && refs && head && branch && nested);
  ASSERT_OK(os_mkdir_p(refs));
  ASSERT_OK(os_mkdir_p(nested));
  ASSERT_OK(os_write_file(head, "ref: refs/heads/main\n", 21));
  ASSERT_OK(os_write_file(branch, oid, sizeof oid - 1));
  ASSERT_TRUE(fx_open_ctx_at(&f, nested));
  ASSERT_OK(asngn_workspace_get(f.c, &info));
  expected_root = os_realpath(f.root);
  ASSERT_TRUE(expected_root != NULL);
  ASSERT_EQ_STR(info.repository_root, expected_root);
  ASSERT_EQ_STR(info.branch, "main");
  ASSERT_EQ_STR(info.head, "0123456789abcdef0123456789abcdef01234567");
  free(expected_root);
  free(git); free(refs); free(head); free(branch); free(nested);
  fx_drop(&f);
}

TEST(session_workspace_is_private_and_switches_deterministically) {
  fx f;
  asngn_session *alpha = NULL, *beta = NULL;
  asngn_workspace_info aw, bw;
  char *marker = NULL, *beta_marker = NULL, *alpha_dir = NULL;

  ASSERT_TRUE(fx_setup(&f, "  cache: { enable: false },\n"));
  free(f.c->cfg.astools_workspace);
  f.c->cfg.astools_workspace = asngn_strdup("session");
  f.c->session_workspaces = true;
  ASSERT_TRUE(f.c->cfg.astools_workspace != NULL);

  ASSERT_OK(asngn_session_open(f.c, "alpha", &alpha));
  ASSERT_OK(asngn_session_open(f.c, "beta", &beta));
  ASSERT_OK(asngn_session_workspace(alpha, &aw));
  ASSERT_OK(asngn_session_workspace(beta, &bw));
  ASSERT_TRUE(strcmp(aw.canonical_root, bw.canonical_root) != 0);
  ASSERT_TRUE(strstr(aw.canonical_root, "sessions") != NULL);
  ASSERT_TRUE(strstr(aw.canonical_root, "alpha") != NULL);
  ASSERT_TRUE(strstr(aw.canonical_root, "workspace") != NULL);
  ASSERT_TRUE(strstr(bw.canonical_root, "beta") != NULL);

  ASSERT_OK(asngn_session_workspace_activate(alpha));
  ASSERT_EQ_STR(f.c->workspace.canonical_root, aw.canonical_root);
  marker = os_path_join(f.c->workspace.canonical_root, "alpha-only.txt");
  ASSERT_TRUE(marker != NULL);
  ASSERT_OK(os_write_file(marker, "alpha\n", 6));
  ASSERT_OK(asngn_session_workspace_activate(beta));
  ASSERT_EQ_STR(f.c->workspace.canonical_root, bw.canonical_root);
  beta_marker = os_path_join(bw.canonical_root, "alpha-only.txt");
  ASSERT_TRUE(beta_marker != NULL);
  ASSERT_TRUE(!os_file_exists(beta_marker));

  alpha_dir = asngn_strdup(alpha->dir);
  ASSERT_TRUE(alpha_dir != NULL);
  free(beta_marker);
  free(marker);
  asngn_session_close(beta);
  asngn_session_close(alpha);
  ASSERT_OK(asngn_session_delete(f.c, "alpha"));
  ASSERT_TRUE(!os_file_exists(alpha_dir));
  free(alpha_dir);
  fx_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(create_layout_and_busy),
  TEST_ENTRY(generated_and_invalid_slugs),
  TEST_ENTRY(list_sorted),
  TEST_ENTRY(manifest_project_persists),
  TEST_ENTRY(transcript_roundtrip),
  TEST_ENTRY(transcript_public_api),
  TEST_ENTRY(pin_persists),
  TEST_ENTRY(pin_limit),
  TEST_ENTRY(workspace_is_bound_and_live_fingerprinted),
  TEST_ENTRY(workspace_discovers_repository_identity),
  TEST_ENTRY(session_workspace_is_private_and_switches_deterministically),
};

RUN_ALL_TESTS()
