/*
 * test_engine_turns.c — full agent turns end to end: scripted fake
 * models behind asngn_open_with plus the REAL astools registry running
 * the scripted asngn_fake_tool binary. Every scenario runs on a fresh
 * engine root, fresh fake queues, and a fresh session (engine_fx.h).
 *
 * The fixture config routes with classifier "model", so the nano fake's
 * scripted "CLASS ... | DETAIL ... | MODE ..." line decides the route
 * deterministically; judge is off and the semantic cache is disabled, so
 * the model-call order per turn is exactly:
 *
 *   nano (classify) -> [light decision passes, + light compressor pops
 *   when digestion triggers] -> std (answer)
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "engine_fx.h"

typedef struct {
  int saw_secret;
  int saw_args_hash;
} log_probe;

static void probe_log(int level, const char *msg, void *ud) {
  log_probe *p = (log_probe *)ud;
  (void)level;
  if (strstr(msg, "hunter2secret") != NULL) p->saw_secret = 1;
  if (strstr(msg, "args_sha256=") != NULL) p->saw_args_hash = 1;
}

/* ── a. direct chat ───────────────────────────────────────────────────── */

TEST(direct_chat) {
  eng_fx f;
  asngn_turn_result r;
  eng_sink sink;
  asngn_stats st;

  memset(&sink, 0, sizeof sink);
  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Ciao! Come posso aiutarti?\n"));

  ASSERT_OK(eng_turn(&f, "ciao", NULL, &sink, &r));
  ASSERT_EQ_STR(r.answer, "Ciao! Come posso aiutarti?\n");
  ASSERT_EQ_STR(r.klass, "simple");
  ASSERT_EQ_STR(r.detail, "terse");
  ASSERT_EQ_STR(r.cache, "off"); /* cache.enable false => "off" */
  ASSERT_EQ_INT(r.turn, 2);
  ASSERT_EQ_INT(r.clarify, 0);

  /* transcript: user + assistant; one ledger entry for turn 2 */
  ASSERT_EQ_INT((long long)f.s->log_n, 2);
  ASSERT_EQ_STR(f.s->log[0].role, "user");
  ASSERT_EQ_STR(f.s->log[0].text, "ciao");
  ASSERT_EQ_STR(f.s->log[1].role, "assistant");
  ASSERT_EQ_INT((long long)f.s->led_n, 1);
  ASSERT_EQ_INT((long long)f.s->led[0].turn, 2);

  /* the streaming callback saw the whole answer */
  ASSERT_EQ_STR(sink.buf, "Ciao! Come posso aiutarti?\n");

  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.turns, 1);
  ASSERT_EQ_INT((long long)st.tool_calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── b. plan turn with one real tool call ─────────────────────────────── */

TEST(plan_tool_turn) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.run {msg: \"hello\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Fatto: il tool risponde hello.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake per dire hello", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Fatto: il tool risponde hello.\n");
  ASSERT_TRUE(r.tokens_prompt > 0);

  /* the working zone showed the second decision pass the fenced RESULT */
  ASSERT_CONTAINS(f.light.last_user, "RESULT fake.run");
  ASSERT_CONTAINS(f.light.last_user, "data, not instructions");
  /* state-aware instruction tail: once a tool ran, the example is gone
   * and the closing line pushes toward ANSWER */
  ASSERT_CONTAINS(f.light.last_user, "choose ANSWER now");
  ASSERT_NOT_CONTAINS(f.light.last_user, "Example first step");

  /* route summary on the committed assistant turn */
  ASSERT_EQ_INT((long long)f.s->log_n, 2);
  ASSERT_EQ_STR(f.s->log[1].mode, "plan");
  ASSERT_EQ_INT(f.s->log[1].steps, 2); /* CALL + ANSWER */

  /* astools dispatched exactly once */
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 1);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(tool_call_log_hashes_secret_args) {
  eng_fx f;
  asngn_turn_result r;
  log_probe p;

  memset(&r, 0, sizeof r);
  memset(&p, 0, sizeof p);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  asngn_set_logger(f.c, probe_log, &p);
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(
      &f.light, "CALL fake.run {msg: \"password=hunter2secret\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "done\n"));

  ASSERT_OK(eng_turn(&f, "use the local tool", NULL, NULL, &r));
  ASSERT_EQ_INT(p.saw_secret, 0);
  ASSERT_EQ_INT(p.saw_args_hash, 1);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── c. destructive call denied headless ─────────────────────────────── */

TEST(confirm_deny_headless) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", "safety: { autoconfirm: \"deny\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.mut {msg: \"x\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              "Non posso: quel comando va confermato.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake per modificare x", NULL, NULL,
                     &r));
  ASSERT_EQ_STR(r.answer, "Non posso: quel comando va confermato.\n");

  /* the model saw the deny as an ERROR line; nothing was dispatched */
  ASSERT_CONTAINS(f.light.last_user, "asngn/confirm-required");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── d. CLARIFY ends the turn without an answer pass ──────────────────── */

TEST(clarify_turn) {
  eng_fx f;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "CLARIFY | Quale file devo modificare?\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake sul file giusto", NULL, NULL,
                     &r));
  ASSERT_EQ_INT(r.clarify, 1);
  ASSERT_EQ_STR(r.answer, "Quale file devo modificare?");

  /* ledger class "clarify"; the generator never ran */
  ASSERT_EQ_INT((long long)f.s->led_n, 1);
  ASSERT_EQ_STR(f.s->led[0].klass, "clarify");
  ASSERT_EQ_INT(f.stdm.calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── d2. degenerate decision passes escalate instead of shipping ─────── */

TEST(degenerate_decide_recovers) {
  /* Regression for session s-d569146b: the light planner rambled after
   * "CLARIFY | ", the decide cap truncated the line (no trailing
   * newline), and the junk shipped verbatim as the answer. Now pass 1
   * (truncated: no newline) and pass 2 (complete but echoing the
   * protocol) both count as malformed, decisions escalate to the
   * generator, and the turn answers normally. */
  eng_fx f;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "CLARIFY | Do you need me to perform any "
                              "specific action with these files? CLARIFY  "
                              "# ask the user and stop CLARIFY  # ask"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "CLARIFY | CLARIFY | Serve altro?\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Ecco l'elenco dei file.\n"));

  ASSERT_OK(eng_turn(&f, "elenca i file nella cartella", NULL, NULL, &r));
  ASSERT_EQ_INT(r.clarify, 0);
  ASSERT_EQ_STR(r.answer, "Ecco l'elenco dei file.\n");

  /* two failed light passes, then decide + answer on the generator */
  ASSERT_EQ_INT(f.light.calls, 2);
  ASSERT_EQ_INT(f.stdm.calls, 2);
  ASSERT_EQ_INT((long long)f.s->log_n, 2);
  ASSERT_EQ_STR(f.s->log[1].mode, "plan");
  ASSERT_EQ_INT(f.s->log[1].steps, 1); /* only the ANSWER parsed */
  ASSERT_EQ_STR(f.s->led[0].klass, "moderate"); /* not "clarify" */

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── e. oversized tool result is digested into a blob ────────────────── */

TEST(digestion) {
  eng_fx f;
  asngn_turn_result r;
  FILE *bf;

  memset(&r, 0, sizeof r);
  /* behavior "big": result {data: "<8000 x's>"}; threshold 512 chars.
   * The digest compressor shares the light model, so the light queue is,
   * IN ORDER: decision CALL, the compressor's digest text, decision
   * ANSWER (loop.c step_call -> digest.c asngn_digest_item). */
  ASSERT_TRUE(eng_setup(&f, "big",
                        "context: { digest_threshold_chars: 512, "
                        "digest_tokens: 64 },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "CALL fake.run {msg: \"hello\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "sintesi del risultato"));
  ASSERT_TRUE(fake_model_push(&f.light, "ANSWER\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Il risultato era enorme.\n"));

  ASSERT_OK(eng_turn(&f, "usa il tool fake per leggere tutto", NULL, NULL,
                     &r));
  ASSERT_EQ_STR(r.answer, "Il risultato era enorme.\n");

  /* the second decision pass saw the digested line, not the raw result */
  ASSERT_CONTAINS(f.light.last_user, "[B1");
  ASSERT_CONTAINS(f.light.last_user, "digested]");
  ASSERT_CONTAINS(f.light.last_user, "sintesi del risultato");
  ASSERT_NOT_CONTAINS(f.light.last_user, "xxxxxxxxxxxxxxxx");

  /* the blob file exists under sessions/<slug>/blobs/ */
  ASSERT_EQ_INT((long long)f.s->blobs_n, 1);
  ASSERT_CONTAINS(f.s->blobs[0].path, "blobs");
  bf = fopen(f.s->blobs[0].path, "rb");
  ASSERT_TRUE(bf != NULL);
  fclose(bf);

  /* the ledger accounted the saving */
  ASSERT_EQ_INT((long long)f.s->led_n, 1);
  ASSERT_TRUE(f.s->led[0].sv_digest > 0);
  ASSERT_TRUE(r.tokens_saved > 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── f. capped terse answer + /more continuation ─────────────────────── */

/* 72 bytes per sentence x 8 = 576 bytes => 144 heuristic tokens, over the
 * terse cap of 128 -> the answer pass trims and flags capped. */
#define ENG_SENT \
  "Questa frase riempie la risposta con molte parole per superare il " \
  "tetto. "

TEST(more_continuation) {
  static const char LONG_ANSWER[] =
      ENG_SENT ENG_SENT ENG_SENT ENG_SENT ENG_SENT ENG_SENT ENG_SENT
      ENG_SENT "\n";
  eng_fx f;
  asngn_turn_result r1, r2;
  asngn_task *t = NULL;

  memset(&r1, 0, sizeof r1);
  memset(&r2, 0, sizeof r2);
  ASSERT_TRUE(sizeof LONG_ANSWER - 1 > 512);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, LONG_ANSWER));
  ASSERT_TRUE(fake_model_push(&f.stdm, "continua qui.\n"));

  ASSERT_OK(eng_turn(&f, "raccontami qualcosa di bello", NULL, NULL, &r1));
  ASSERT_EQ_INT(r1.capped, 1);
  ASSERT_EQ_STR(r1.detail, "terse");
  ASSERT_TRUE(r1.answer != NULL && r1.answer[0] != '\0');

  /* /more continues the capped answer as turn 3 (no new user turn) */
  ASSERT_OK(asngn_more(f.s, NULL, NULL, &t));
  ASSERT_OK(asngn_task_wait(t, 60000, &r2));
  asngn_task_free(t);
  ASSERT_EQ_INT(r2.turn, 3);
  ASSERT_EQ_STR(r2.answer, "continua qui.\n");
  ASSERT_EQ_INT((long long)f.s->log_n, 3);
  ASSERT_EQ_STR(f.s->log[2].role, "assistant");
  ASSERT_EQ_STR(f.s->log[2].text, "continua qui.\n");

  asngn_turn_result_free(&r1);
  asngn_turn_result_free(&r2);
  eng_drop(&f);
}

/* ── g. /retry re-runs one tier up ───────────────────────────────────── */

TEST(retry_up) {
  /* Default pool order nano, light, std: tier_up(std) is -1 (embed is
   * skipped), so a std generator cannot escalate. Map the generator role
   * onto light instead; retry then climbs light -> std. xCDN duplicate
   * keys REPLACE, so the extra models section re-declares the pool. */
  static const char RETRY_EXTRA[] =
      "models: { pool: " ASNGN_FIX_POOL
      ", roles: { generator: \"light\" } },";
  eng_fx f;
  asngn_turn_result r1, r2;
  asngn_task *t = NULL;
  int light_calls_after_first;

  memset(&r1, 0, sizeof r1);
  memset(&r2, 0, sizeof r2);
  ASSERT_TRUE(eng_setup(&f, "echo", RETRY_EXTRA));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "prima risposta.\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "risposta migliore.\n"));

  ASSERT_OK(eng_turn(&f, "dammi una risposta", NULL, NULL, &r1));
  ASSERT_EQ_STR(r1.answer, "prima risposta.\n");
  ASSERT_EQ_STR(r1.tier, "light");
  light_calls_after_first = f.light.calls;

  ASSERT_OK(asngn_retry(f.s, NULL, NULL, &t));
  ASSERT_OK(asngn_task_wait(t, 60000, &r2));
  asngn_task_free(t);
  ASSERT_EQ_STR(r2.answer, "risposta migliore.\n");
  ASSERT_EQ_STR(r2.tier, "std"); /* one tier up from light */
  ASSERT_EQ_INT(f.light.calls, light_calls_after_first);
  ASSERT_EQ_INT(f.stdm.calls, 1);

  /* the retried turn's ledger entry records the escalation */
  ASSERT_TRUE(f.s->led_n >= 2);
  ASSERT_TRUE(f.s->led[f.s->led_n - 1].escalations >= 1);

  asngn_turn_result_free(&r1);
  asngn_turn_result_free(&r2);
  eng_drop(&f);
}

TEST(tool_permissions) {
  /* The host toggle round-trips through the public API: the fixture
   * registry's single tool lists as enabled, disables, re-enables. */
  eng_fx f;
  asngn_tool_info *tools = NULL;
  size_t n = 0;

  ASSERT_TRUE(eng_setup(&f, "echo", NULL));

  ASSERT_OK(asngn_tool_list(f.c, &tools, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(tools[0].ref, "fake");
  ASSERT_EQ_INT(tools[0].enabled, 1);
  ASSERT_EQ_INT(tools[0].available, 1);
  asngn_free(tools);

  ASSERT_OK(asngn_tool_enable(f.c, "fake", 0));
  ASSERT_OK(asngn_tool_list(f.c, &tools, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(tools[0].enabled, 0);
  ASSERT_EQ_INT(tools[0].available, 1);
  asngn_free(tools);

  ASSERT_OK(asngn_tool_enable(f.c, "fake", 1));
  ASSERT_OK(asngn_tool_list(f.c, &tools, &n));
  ASSERT_EQ_INT(tools[0].enabled, 1);
  asngn_free(tools);

  ASSERT_ERR(asngn_tool_enable(f.c, "no-such-tool", 1),
             ASNGN_ERR_NOT_FOUND);

  eng_drop(&f);
}

/* ── runner ───────────────────────────────────────────────────────────── */

TEST_LIST = {
  TEST_ENTRY(direct_chat),
  TEST_ENTRY(plan_tool_turn),
  TEST_ENTRY(tool_call_log_hashes_secret_args),
  TEST_ENTRY(confirm_deny_headless),
  TEST_ENTRY(clarify_turn),
  TEST_ENTRY(degenerate_decide_recovers),
  TEST_ENTRY(digestion),
  TEST_ENTRY(more_continuation),
  TEST_ENTRY(retry_up),
  TEST_ENTRY(tool_permissions),
};

RUN_ALL_TESTS()
