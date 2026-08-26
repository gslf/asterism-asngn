/*
 * test_context_golden.c — determinism: zoned prompt assembly is a
 * pure function of session state + config + turn state. Byte-identical
 * outputs, zone structure, working items, whole-item verbatim trimming
 * with the pin-first rule, and the summary zone.
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
               "astools: { enable: false } },\n"
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

/* One scripted DIRECT turn (heuristic classifier, judge off, cache off). */
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

/* Hand-made probe turn state over the std slot; user_msg is borrowed. */
static void fx_probe_state(fx *f, asngn_session *s, char *msg,
                           asngn_turn_state *t, int *slot) {
  memset(t, 0, sizeof *t);
  t->s = s;
  t->user_msg = msg;
  *slot = asngn_models_slot_for_id(f->c, "std");
  t->gen_slot = *slot;
}

/* Release only what the test itself pushed into the probe state. */
static void fx_probe_dispose(asngn_turn_state *t) {
  size_t i;
  for (i = 0; i < t->work_n; i++) free(t->work[i].text);
  free(t->work);
  t->work = NULL;
  t->work_n = t->work_cap = 0;
}

static const char CACHE_OFF[] = "  cache: { enable: false },\n";

/* ── tests ────────────────────────────────────────────────────────────── */

TEST(empty_session_golden) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_state t;
  asngn_prompt p;
  char msg[] = "terza domanda";
  int slot;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "vuota", &s));
  fx_probe_state(&f, s, msg, &t, &slot);
  ASSERT_TRUE(slot >= 0);

  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p));
  /* inline goldens: default base prompt, empty transcript */
  ASSERT_EQ_STR(p.system_text,
                "You are a capable, honest local assistant.");
  ASSERT_EQ_STR(p.user_text,
                "## This turn\nuser: terza domanda\n\nINSTR\n");
  ASSERT_TRUE(p.tok_system > 0);
  ASSERT_EQ_INT((long long)p.tok_memory, 0);
  ASSERT_EQ_INT((long long)p.tok_catalog, 0);
  ASSERT_EQ_INT((long long)p.tok_summary, 0);
  ASSERT_EQ_INT((long long)p.tok_verbatim, 0);
  ASSERT_TRUE(p.tok_working > 0);
  asngn_prompt_free(&p);
  fx_probe_dispose(&t);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(assemble_deterministic) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_state t;
  asngn_prompt p1, p2;
  char msg[] = "terza domanda";
  int slot;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "gold", &s));
  ASSERT_TRUE(fx_turn(&f, s, "prima domanda", "Risposta uno.\n"));
  ASSERT_TRUE(fx_turn(&f, s, "seconda domanda", "Risposta due.\n"));
  fx_probe_state(&f, s, msg, &t, &slot);
  ASSERT_TRUE(slot >= 0);

  /* identical inputs => byte-identical output, twice over */
  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p1));
  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p2));
  ASSERT_EQ_STR(p1.system_text, p2.system_text);
  ASSERT_EQ_STR(p1.user_text, p2.user_text);
  ASSERT_EQ_INT((long long)p1.tok_verbatim, (long long)p2.tok_verbatim);
  asngn_prompt_free(&p2);

  /* zone structure */
  ASSERT_TRUE(asngn_str_has_prefix(p1.system_text,
                                   "You are a capable, honest local "
                                   "assistant."));
  ASSERT_TRUE(strstr(p1.user_text, "## Conversation\n") != NULL);
  ASSERT_TRUE(strstr(p1.user_text, "user: prima domanda\n") != NULL);
  ASSERT_TRUE(strstr(p1.user_text, "assistant: Risposta uno.\n") != NULL);
  ASSERT_TRUE(strstr(p1.user_text, "user: seconda domanda\n") != NULL);
  ASSERT_TRUE(strstr(p1.user_text, "assistant: Risposta due.\n") != NULL);
  ASSERT_TRUE(strstr(p1.user_text,
                     "## This turn\nuser: terza domanda\n") != NULL);
  ASSERT_TRUE(strstr(p1.user_text, "INSTR") != NULL);
  ASSERT_TRUE(p1.tok_verbatim > 0);

  /* working items appear after a push, and the working count grows */
  ASSERT_OK(asngn_work_push(f.c, &t, "nota operativa"));
  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p2));
  ASSERT_TRUE(strcmp(p1.user_text, p2.user_text) != 0);
  ASSERT_TRUE(strstr(p2.user_text, "nota operativa") != NULL);
  ASSERT_TRUE(strstr(p1.user_text, "nota operativa") == NULL);
  ASSERT_TRUE(p2.tok_working > p1.tok_working);
  ASSERT_EQ_STR(p1.system_text, p2.system_text);
  asngn_prompt_free(&p1);
  asngn_prompt_free(&p2);
  fx_probe_dispose(&t);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(verbatim_budget_pin_first) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_state t;
  asngn_prompt p;
  char msg[] = "quarta domanda";
  int slot;
  static const char LONGA[] =
      "Questa risposta contiene molte parole in fila cosi che la sua "
      "resa superi largamente il budget verbatim minuscolo del test.\n";
  static const char LONGB[] =
      "Anche questa seconda risposta occupa parecchi token e quindi non "
      "puo mai entrare in un budget di appena otto token totali.\n";

  /* tiny verbatim budget: 8 tokens (~32 rendered bytes) */
  ASSERT_TRUE(fx_setup(&f,
                       "  cache: { enable: false },\n"
                       "  context: { verbatim_tokens: 8 },\n"));
  ASSERT_OK(asngn_session_open(f.c, "tight", &s));
  ASSERT_TRUE(fx_turn(&f, s, "k1", LONGA));
  ASSERT_TRUE(fx_turn(&f, s,
                      "questa seconda domanda contiene molte piu parole "
                      "della prima", LONGB));
  fx_probe_state(&f, s, msg, &t, &slot);
  ASSERT_TRUE(slot >= 0);

  /* every line overflows the budget (newest first, whole items only):
   * nothing fits, so the zone vanishes together with its heading */
  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p));
  ASSERT_TRUE(strstr(p.user_text, "## Conversation") == NULL);
  ASSERT_EQ_INT((long long)p.tok_verbatim, 0);
  ASSERT_TRUE(strstr(p.user_text, "## This turn\nuser: quarta domanda\n")
              != NULL);
  asngn_prompt_free(&p);

  /* pin turn 1 ("user: k1" = 2 tokens): the pin-first pass takes it
   * while the newer, larger unpinned turns still drop whole */
  ASSERT_OK(asngn_session_pin(s, 1, 1));
  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p));
  ASSERT_TRUE(strstr(p.user_text, "## Conversation\nuser: k1\n") != NULL);
  ASSERT_TRUE(strstr(p.user_text, "assistant:") == NULL);
  ASSERT_TRUE(strstr(p.user_text, "seconda domanda") == NULL);
  ASSERT_TRUE(p.tok_verbatim > 0);
  asngn_prompt_free(&p);
  fx_probe_dispose(&t);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(summary_zone) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_state t;
  asngn_prompt p;
  char msg[] = "terza domanda";
  int slot;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "summ", &s));
  free(s->summary);
  s->summary = asngn_strdup("Riassunto.");
  ASSERT_TRUE(s->summary != NULL);
  fx_probe_state(&f, s, msg, &t, &slot);
  ASSERT_TRUE(slot >= 0);

  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p));
  /* zone 4 sits after the base prompt (no memory, no catalog) and
   * before the user text — inline golden for the whole system text */
  ASSERT_EQ_STR(p.system_text,
                "You are a capable, honest local assistant.\n\n"
                "## Conversation summary\nRiassunto.");
  ASSERT_TRUE(p.tok_summary > 0);
  ASSERT_EQ_STR(p.user_text,
                "## This turn\nuser: terza domanda\n\nINSTR\n");
  asngn_prompt_free(&p);
  fx_probe_dispose(&t);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST(global_budget_reports_zones) {
  fx f;
  asngn_session *s = NULL;
  asngn_turn_state t;
  asngn_prompt p;
  asngn_context_diagnostics d;
  char msg[] = "messaggio che deve rientrare nel budget globale";
  int slot;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  ASSERT_OK(asngn_session_open(f.c, "budget", &s));
  fx_probe_state(&f, s, msg, &t, &slot);
  ASSERT_OK(asngn_context_assemble(f.c, s, &t, NULL, "INSTR", slot, &p));
  f.c->models[slot].cfg.ctx = 96;
  ASSERT_EQ_INT(asngn_context_validate(f.c, slot, &p, 64),
                ASNGN_ERR_CONTEXT);
  ASSERT_OK(asngn_last_context_diagnostics(f.c, &d));
  ASSERT_EQ_INT((long long)d.n_ctx, 96);
  ASSERT_EQ_INT((long long)d.prompt_budget, 0);
  ASSERT_TRUE(d.prompt_total > 0);
  ASSERT_TRUE(d.system > 0);
  ASSERT_TRUE(d.working > 0);
  ASSERT_TRUE(strstr(asngn_last_error(f.c), "working=") != NULL);
  asngn_prompt_free(&p);
  fx_probe_dispose(&t);
  asngn_session_close(s);
  fx_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(empty_session_golden),
  TEST_ENTRY(assemble_deterministic),
  TEST_ENTRY(verbatim_budget_pin_first),
  TEST_ENTRY(summary_zone),
  TEST_ENTRY(global_budget_reports_zones),
};

RUN_ALL_TESTS()
