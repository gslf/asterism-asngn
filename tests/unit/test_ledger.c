/*
 * test_ledger.c — the per-turn token ledger: entry fields
 * after scripted turns, totals, feedback persistence, and the QpT math.
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
               "  integration: { asper: { enable: false }, "
               "astls: { enable: false } },\n"
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
  if (!fx_write_config(f, "  cache: { enable: false },\n")) return 0;
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

/* One scripted DIRECT turn (heuristic classifier, judge off, cache off):
 * exactly one std-fake generation per turn. */
static int fx_turn(fx *f, asngn_session *s, const char *msg,
                   const char *reply) {
  asngn_task *task = NULL;
  asngn_turn_result res;
  asngn_err e;

  if (!fake_model_push(&f->stdm, reply)) return 0;
  if (asngn_submit(s, msg, NULL, NULL, NULL, &task) != ASNGN_OK) return 0;
  memset(&res, 0, sizeof res);
  e = asngn_task_wait(task, 30000, &res);
  asngn_task_free(task);
  asngn_turn_result_free(&res);
  return e == ASNGN_OK;
}

/* ── tests ────────────────────────────────────────────────────────────── */

TEST(two_turns_entry_fields) {
  fx f;
  asngn_session *s = NULL;
  const asngn_ledger_entry *e0, *e1;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "led", &s));
  ASSERT_TRUE(fx_turn(&f, s, "Prima domanda", "Risposta uno.\n"));
  ASSERT_TRUE(fx_turn(&f, s, "Seconda domanda", "Risposta due.\n"));

  ASSERT_EQ_INT((long long)s->led_n, 2);
  ASSERT_TRUE(s->spent_tokens > 0);
  e0 = &s->led[0];
  e1 = &s->led[1];
  ASSERT_EQ_INT((long long)e0->turn, 2); /* assistant ordinals: 2 and 4 */
  ASSERT_EQ_INT((long long)e1->turn, 4);
  ASSERT_EQ_STR(e0->klass, "simple");
  ASSERT_EQ_STR(e0->detail, "terse");   /* short message, no cue */
  ASSERT_EQ_STR(e0->mode, "direct");
  ASSERT_EQ_STR(e0->tier, "std");
  ASSERT_EQ_STR(e0->cache, "off");      /* cache disabled */
  ASSERT_EQ_STR(e1->cache, "off");
  ASSERT_EQ_INT(e0->at, 1755150000);
  ASSERT_EQ_INT(e0->escalations, 0);
  ASSERT_TRUE(!e0->has_judge);          /* judge off */
  ASSERT_TRUE(!e0->has_user_fb);
  ASSERT_TRUE(!e0->capped);
  /* "Risposta uno.\n" = 14 bytes -> 3 fake tokens; DIRECT: no decisions */
  ASSERT_EQ_INT((long long)e0->gt_answer, 3);
  ASSERT_EQ_INT((long long)e0->gt_decision, 0);
  ASSERT_EQ_INT((long long)e0->sv_cache, 0);
  ASSERT_TRUE(e0->pt_system > 0);
  ASSERT_TRUE(asngn_ledger_total_tokens(e0) > 0);
  ASSERT_TRUE(asngn_ledger_total_tokens(e1) > 0);
  ASSERT_EQ_INT(s->spent_tokens,
                (long long)(asngn_ledger_total_tokens(e0) +
                            asngn_ledger_total_tokens(e1)));
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(feedback_persists) {
  fx f;
  asngn_session *s = NULL;
  size_t t2;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "led", &s));
  ASSERT_TRUE(fx_turn(&f, s, "Prima domanda", "Risposta uno.\n"));
  ASSERT_TRUE(fx_turn(&f, s, "Seconda domanda", "Risposta due.\n"));
  t2 = s->led[1].turn;

  ASSERT_OK(asngn_feedback(s, t2, 1));
  ASSERT_TRUE(s->led[1].has_user_fb);
  ASSERT_EQ_INT(s->led[1].user_fb, 1);
  ASSERT_ERR(asngn_feedback(s, t2, 5), ASNGN_ERR_INVALID);
  ASSERT_ERR(asngn_feedback(s, 99, 1), ASNGN_ERR_NOT_FOUND);
  asngn_session_close(s);
  s = NULL;

  /* the feedback record replays from the append-only ledger stream */
  ASSERT_OK(asngn_session_open(f.c, "led", &s));
  ASSERT_EQ_INT((long long)s->led_n, 2);
  ASSERT_TRUE(!s->led[0].has_user_fb);
  ASSERT_TRUE(s->led[1].has_user_fb);
  ASSERT_EQ_INT(s->led[1].user_fb, 1);

  /* clearing also persists (null user in the amendment) */
  ASSERT_OK(asngn_feedback(s, t2, 0));
  ASSERT_TRUE(!s->led[1].has_user_fb);
  asngn_session_close(s);
  s = NULL;
  ASSERT_OK(asngn_session_open(f.c, "led", &s));
  ASSERT_TRUE(!s->led[1].has_user_fb);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(qpt_math) {
  ASSERT_EQ_DBL(asngn_qpt(0.5, 500), 1.0, 1e-12);
  ASSERT_EQ_DBL(asngn_qpt(1.0, 2000), 0.5, 1e-12);
  ASSERT_EQ_DBL(asngn_qpt(0.25, 1000), 0.25, 1e-12);
  ASSERT_EQ_DBL(asngn_qpt(2.0, 1000), 1.0, 1e-12);  /* q clamps to 1 */
  ASSERT_EQ_DBL(asngn_qpt(-1.0, 1000), 0.0, 1e-12); /* q clamps to 0 */
  ASSERT_EQ_DBL(asngn_qpt(0.5, 0), 500.0, 1e-9);    /* total clamps to 1 */
}

TEST(qpt_window_and_feedback) {
  fx f;
  asngn_session *s = NULL;
  size_t total;
  double before;

  ASSERT_TRUE(fx_setup(&f));
  ASSERT_OK(asngn_session_open(f.c, "qpt", &s));
  ASSERT_TRUE(fx_turn(&f, s, "Prima domanda", "Risposta uno.\n"));
  ASSERT_EQ_INT((long long)s->led_n, 1);

  /* no judge: q = 0.5; QpT = q / (total / 1000), computed by hand */
  total = asngn_ledger_total_tokens(&s->led[0]);
  ASSERT_TRUE(total > 0);
  ASSERT_EQ_DBL(asngn_session_qpt(s),
                0.5 / ((double)total / 1000.0), 1e-9);
  ASSERT_EQ_DBL(asngn_session_qpt(s), asngn_qpt(0.5, total), 1e-12);

  /* +1 feedback lifts q to 0.5 + 0.3 = 0.8 and raises QpT */
  before = asngn_session_qpt(s);
  ASSERT_OK(asngn_feedback(s, s->led[0].turn, 1));
  ASSERT_EQ_DBL(asngn_session_qpt(s), asngn_qpt(0.8, total), 1e-9);
  ASSERT_TRUE(asngn_session_qpt(s) > before);

  /* -1 feedback drops it below the neutral value */
  ASSERT_OK(asngn_feedback(s, s->led[0].turn, -1));
  ASSERT_EQ_DBL(asngn_session_qpt(s), asngn_qpt(0.2, total), 1e-9);
  ASSERT_TRUE(asngn_session_qpt(s) < before);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(two_turns_entry_fields),
  TEST_ENTRY(feedback_persists),
  TEST_ENTRY(qpt_window_and_feedback),
  TEST_ENTRY(qpt_math),
};

RUN_ALL_TESTS()
