/*
 * test_cache.c — the semantic cache: miss/hit/adapt over scripted
 * turns, the direct probe API at crafted cosines, the epoch rule, the
 * tools_used exclusion with plan hints, TTL sweeps, and persistence
 * across context reopen (semantic.xcdn + embeddings.bin).
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"
#include "fakes.h"

/* Bag-of-words fake-embed geometry (verified in cosine_geometry):
 *   Q_HIT vs Q_HIT    cos = 1.0            (>= hit 0.95)
 *   Q_HIT vs Q_ADAPT  cos = 5/sqrt(30) ~ 0.9129 (in [0.85, 0.95))
 *   Q_HIT vs Q_MISS   cos ~ 0.54           (< adapt 0.85)          */
static const char Q_HIT[]   = "come rigenero la cache embeddings";
static const char Q_ADAPT[] = "come rigenero la cache embeddings adesso";
static const char Q_MISS[]  = "quale porta usa il server ftp remoto";

/* ── shared fixture ───────────────────────────────────────────────────── */

typedef struct {
  char        root[256];
  char        cfg[300];
  fake_model  nano, light, stdm, embed;
  fake_clock  clk;
  asngn_ctx  *c;
} fx;

static int fx_write_config(fx *f) {
  FILE *fp;
  int ok;
  snprintf(f->cfg, sizeof f->cfg, "%s/config.xcdn", f->root);
  fp = fopen(f->cfg, "wb");
  if (fp == NULL) return 0;
  ok = fprintf(fp,
               "#asngn_config {\n"
               "  integration: { asper: { enable: false }, "
               "astools: { enable: false } },\n"
               "  validation: { judge: \"off\" },\n"
               "  routing: { classifier: \"heuristic\" },\n"
               "  cache: { enable: true },\n"
               "  models: { pool: [\n"
               "    { id: \"nano\", path: \"none.gguf\" },\n"
               "    { id: \"light\", path: \"none.gguf\" },\n"
               "    { id: \"std\", path: \"none.gguf\" },\n"
               "    { id: \"embed\", path: \"none.gguf\", "
               "embedding: true, dim: 16 },\n"
               "  ] },\n"
               "}\n") > 0;
  fclose(fp);
  return ok;
}

static int fx_open_ctx(fx *f) {
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
  if (asngn_open_with(&p, ifaces, 4, ids, &clk, &f->c) != ASNGN_OK)
    return 0;
  asngn_set_logger(f->c, NULL, NULL);
  return 1;
}

static int fx_setup(fx *f) {
  memset(f, 0, sizeof *f);
  if (!asngn_test_tmpdir(f->root)) return 0;
  fake_model_init(&f->nano);
  fake_model_init(&f->light);
  fake_model_init(&f->stdm);
  fake_model_init(&f->embed);
  fake_clock_set(&f->clk, 1755150000);
  if (!fx_write_config(f)) return 0;
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

/* Close and reopen the context over the same root (persistence). */
static int fx_reopen_ctx(fx *f) {
  asngn_close(f->c);
  f->c = NULL;
  return fx_open_ctx(f);
}

/* One scripted DIRECT turn; reply NULL queues nothing (cache-served). */
static int fx_turn(fx *f, asngn_session *s, const char *msg,
                   const char *reply, asngn_turn_result *out) {
  asngn_task *task = NULL;
  asngn_turn_result res;
  asngn_err e;

  if (reply != NULL && !fake_model_push(&f->stdm, reply)) return 0;
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

static double fx_cos(const char *a, const char *b) {
  float va[FAKE_EMBED_DIM], vb[FAKE_EMBED_DIM];
  double acc = 0.0;
  int i;
  fake_embed_text(a, va);
  fake_embed_text(b, vb);
  for (i = 0; i < FAKE_EMBED_DIM; i++)
    acc += (double)va[i] * (double)vb[i];
  return acc;
}

/* Insert one entry through the real API with its fake-embed vector. */
static int fx_insert(fx *f, asngn_session *s, const char *query,
                     const char *answer, int tools_used) {
  float vec[FAKE_EMBED_DIM];
  char *tools[1];
  tools[0] = (char *)"fs.read";
  fake_embed_text(query, vec);
  return asngn_cache_insert(f->c, s, query, vec, answer, "terse", "std",
                            100, tools_used != 0, tools_used ? tools : NULL,
                            tools_used ? (size_t)1 : 0) == ASNGN_OK;
}

/* ── tests ────────────────────────────────────────────────────────────── */

TEST(cosine_geometry) {
  /* the fixture's own premises: the crafted queries land in the three
   * outcome bands of the default thresholds */
  ASSERT_EQ_DBL(fx_cos(Q_HIT, Q_HIT), 1.0, 1e-6);
  ASSERT_TRUE(fx_cos(Q_HIT, Q_ADAPT) >= 0.86);
  ASSERT_TRUE(fx_cos(Q_HIT, Q_ADAPT) < 0.95);
  ASSERT_TRUE(fx_cos(Q_HIT, Q_MISS) < 0.85);
}

TEST(miss_then_verbatim_hit) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_result r;
  asngn_stats st;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "cache1", &s));

  ASSERT_TRUE(fx_turn(&f, s, Q_HIT, "Risposta A.\n", &r));
  ASSERT_EQ_STR(r.cache, "miss");
  ASSERT_EQ_STR(r.answer, "Risposta A.\n");
  asngn_turn_result_free(&r);
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.cache_misses, 1);
  ASSERT_EQ_INT((long long)st.cache_hits, 0);
  ASSERT_EQ_INT((long long)asngn_cache_count(f.c), 1);

  /* the same question again: verbatim reuse, zero generation */
  ASSERT_TRUE(fx_turn(&f, s, Q_HIT, NULL, &r));
  ASSERT_EQ_STR(r.cache, "hit");
  ASSERT_EQ_STR(r.answer, "Risposta A.\n");
  ASSERT_TRUE(r.tokens_saved > 0);
  asngn_turn_result_free(&r);
  ASSERT_EQ_INT(f.stdm.calls, 1); /* the generator was not consulted */
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.cache_hits, 1);
  ASSERT_EQ_INT((long long)st.cache_misses, 1);
  ASSERT_EQ_INT((long long)s->led_n, 2);
  ASSERT_EQ_STR(s->led[1].cache, "hit");
  ASSERT_TRUE(s->led[1].sv_cache > 0);
  /* hit turns insert nothing new */
  ASSERT_EQ_INT((long long)asngn_cache_count(f.c), 1);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(paraphrase_adapt) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_result r;
  asngn_stats st;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "cache2", &s));
  ASSERT_TRUE(fx_turn(&f, s, Q_HIT, "Risposta A.\n", NULL));

  /* overlapping paraphrase: cos ~0.913 lands in [adapt, hit) — the
   * adapter (light role) rewrites the cached answer */
  ASSERT_TRUE(fake_model_push(&f.light, "Risposta adattata.\n"));
  ASSERT_TRUE(fx_turn(&f, s, Q_ADAPT, NULL, &r));
  ASSERT_EQ_STR(r.cache, "adapt");
  ASSERT_EQ_STR(r.answer, "Risposta adattata.\n");
  asngn_turn_result_free(&r);
  ASSERT_EQ_INT(f.stdm.calls, 1);  /* only the first turn generated */
  ASSERT_EQ_INT(f.light.calls, 1); /* one adapt pass */
  ASSERT_EQ_STR(s->led[1].cache, "adapt");
  ASSERT_EQ_STR(s->led[1].tier, "light");
  ASSERT_TRUE(s->led[1].sv_cache > 0);
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.cache_adapts, 1);
  /* adapt turns do not insert a new entry */
  ASSERT_EQ_INT((long long)asngn_cache_count(f.c), 1);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(probe_direct_outcomes) {
  fx f;
  asngn_session *s = NULL;
  asngn_cache_probe_result pr;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "probe", &s));
  ASSERT_TRUE(fx_insert(&f, s, Q_HIT, "Risposta A.", 0));

  ASSERT_OK(asngn_cache_probe(f.c, s, Q_HIT, 0.0, &pr));
  ASSERT_EQ_INT(pr.outcome, ASNGN_CACHE_HIT);
  ASSERT_TRUE(pr.answer != NULL);
  ASSERT_EQ_STR(pr.answer, "Risposta A.");
  ASSERT_EQ_DBL(pr.cos, 1.0, 1e-5);
  ASSERT_TRUE(pr.query_vec != NULL);
  asngn_cache_probe_free(&pr);

  ASSERT_OK(asngn_cache_probe(f.c, s, Q_ADAPT, 0.0, &pr));
  ASSERT_EQ_INT(pr.outcome, ASNGN_CACHE_ADAPT);
  ASSERT_TRUE(pr.answer != NULL);
  ASSERT_TRUE(pr.cos >= 0.85 && pr.cos < 0.95);
  asngn_cache_probe_free(&pr);

  ASSERT_OK(asngn_cache_probe(f.c, s, Q_MISS, 0.0, &pr));
  ASSERT_EQ_INT(pr.outcome, ASNGN_CACHE_MISS);
  ASSERT_TRUE(pr.answer == NULL);
  asngn_cache_probe_free(&pr);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(epoch_blocks_hit_allows_adapt) {
  fx f;
  asngn_session *s = NULL;
  asngn_cache_probe_result pr;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "epoch", &s));
  ASSERT_TRUE(fx_insert(&f, s, Q_HIT, "Risposta A.", 0));

  /* same epoch: verbatim hit */
  ASSERT_OK(asngn_cache_probe(f.c, s, Q_HIT, 0.0, &pr));
  ASSERT_EQ_INT(pr.outcome, ASNGN_CACHE_HIT);
  asngn_cache_probe_free(&pr);

  /* the world moved on: identical cosine, but no verbatim reuse —
   * adaptation is still allowed across epochs */
  s->world_epoch++;
  ASSERT_OK(asngn_cache_probe(f.c, s, Q_HIT, 0.0, &pr));
  ASSERT_EQ_INT(pr.outcome, ASNGN_CACHE_ADAPT);
  ASSERT_EQ_DBL(pr.cos, 1.0, 1e-5);
  asngn_cache_probe_free(&pr);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(tools_used_excluded_plan_hint) {
  fx f;
  asngn_session *s = NULL;
  asngn_cache_probe_result pr;
  char *hint = NULL;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "tools", &s));
  ASSERT_TRUE(fx_insert(&f, s, Q_HIT, "Uscita del tool.", 1));

  /* tool-touched entries never answer, even at cos 1.0 */
  ASSERT_OK(asngn_cache_probe(f.c, s, Q_HIT, 0.0, &pr));
  ASSERT_EQ_INT(pr.outcome, ASNGN_CACHE_MISS);
  ASSERT_TRUE(pr.answer == NULL);

  /* ... but they seed the step loop as a plan hint naming the tools */
  ASSERT_OK(asngn_cache_plan_hint(f.c, s, pr.query_vec, &hint));
  ASSERT_TRUE(hint != NULL);
  ASSERT_TRUE(strstr(hint, "a similar request previously used") != NULL);
  ASSERT_TRUE(strstr(hint, "fs.read") != NULL);
  free(hint);
  asngn_cache_probe_free(&pr);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(ttl_sweep_expires) {
  fx f;
  asngn_session *s = NULL;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "ttl", &s));
  ASSERT_TRUE(fx_insert(&f, s, Q_HIT, "Risposta A.", 0));
  ASSERT_EQ_INT((long long)asngn_cache_count(f.c), 1);

  /* default TTL is P7D: 8 days later the sweep drops the entry */
  fake_clock_advance(&f.clk, 8 * 24 * 3600);
  ASSERT_OK(asngn_cache_sweep(f.c));
  ASSERT_EQ_INT((long long)asngn_cache_count(f.c), 0);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(persistence_across_reopen) {
  fx f;
  asngn_session *s = NULL;
  asngn_cache_probe_result pr;
  char path[400];

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "persist", &s));
  ASSERT_TRUE(fx_insert(&f, s, Q_HIT, "Risposta A.", 0));
  ASSERT_OK(asngn_cache_compact(f.c));
  asngn_session_close(s);
  s = NULL;

  ASSERT_TRUE(fx_reopen_ctx(&f));
  ASSERT_EQ_INT((long long)asngn_cache_count(f.c), 1);
  snprintf(path, sizeof path, "%s/cache/embeddings.bin", f.root);
  ASSERT_TRUE(os_file_exists(path));
  snprintf(path, sizeof path, "%s/cache/semantic.xcdn", f.root);
  ASSERT_TRUE(os_file_exists(path));

  /* the reloaded entry still serves a verbatim hit for its session */
  ASSERT_OK(asngn_session_open(f.c, "persist", &s));
  ASSERT_OK(asngn_cache_probe(f.c, s, Q_HIT, 0.0, &pr));
  ASSERT_EQ_INT(pr.outcome, ASNGN_CACHE_HIT);
  ASSERT_TRUE(pr.answer != NULL);
  ASSERT_EQ_STR(pr.answer, "Risposta A.");
  asngn_cache_probe_free(&pr);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(cosine_geometry),
  TEST_ENTRY(miss_then_verbatim_hit),
  TEST_ENTRY(paraphrase_adapt),
  TEST_ENTRY(probe_direct_outcomes),
  TEST_ENTRY(epoch_blocks_hit_allows_adapt),
  TEST_ENTRY(tools_used_excluded_plan_hint),
  TEST_ENTRY(ttl_sweep_expires),
  TEST_ENTRY(persistence_across_reopen),
};

RUN_ALL_TESTS()
