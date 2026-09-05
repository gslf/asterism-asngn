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
 *   when digestion triggers] -> std (answer); SIMPLE+DIRECT turns with
 *   clean evidence start the answer one tier down (std -> light, never
 *   the router slot)
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

typedef struct {
  char output[256];
  char reasoning[256];
  char notice[256];
  size_t output_len, reasoning_len, notice_len;
  int first_kind;
} rich_sink;

static void rich_sink_append(char *dst, size_t cap, size_t *len,
                             const char *text) {
  size_t n = strlen(text);
  if (n > cap - *len - 1) n = cap - *len - 1;
  memcpy(dst + *len, text, n);
  *len += n;
  dst[*len] = '\0';
}

static void rich_sink_cb(const asngn_stream_event *event, void *ud) {
  rich_sink *sink = (rich_sink *)ud;
  if (sink->first_kind < 0) sink->first_kind = (int)event->kind;
  if (event->kind == ASNGN_STREAM_OUTPUT)
    rich_sink_append(sink->output, sizeof sink->output, &sink->output_len,
                     event->text);
  else if (event->kind == ASNGN_STREAM_REASONING)
    rich_sink_append(sink->reasoning, sizeof sink->reasoning,
                     &sink->reasoning_len, event->text);
  else if (event->kind == ASNGN_STREAM_NOTICE)
    rich_sink_append(sink->notice, sizeof sink->notice, &sink->notice_len,
                     event->text);
}

static asngn_err rich_turn(eng_fx *f, const char *message, rich_sink *sink,
                           asngn_turn_result *out) {
  asngn_task *task = NULL;
  asngn_err e = asngn_submit_stream(f->s, message, NULL, rich_sink_cb, sink,
                                    &task);
  if (e == ASNGN_OK) e = asngn_task_wait(task, 60000, out);
  asngn_task_free(task);
  return e;
}

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
  /* SIMPLE+DIRECT with clean evidence starts the answer one tier down
   * (std -> light); the router slot is never used for answers */
  ASSERT_TRUE(fake_model_push(&f.light, "Hello! How can I help you?\n"));

  ASSERT_OK(eng_turn(&f, "hi", NULL, &sink, &r));
  ASSERT_EQ_STR(r.answer, "Hello! How can I help you?\n");
  ASSERT_EQ_STR(r.klass, "simple");
  ASSERT_EQ_STR(r.detail, "terse");
  ASSERT_EQ_STR(r.cache, "off"); /* cache.enable false => "off" */
  ASSERT_EQ_INT(r.turn, 2);
  ASSERT_EQ_INT(r.clarify, 0);

  /* transcript: user + assistant; one ledger entry for turn 2 */
  ASSERT_EQ_INT((long long)f.s->log_n, 2);
  ASSERT_EQ_STR(f.s->log[0].role, "user");
  ASSERT_EQ_STR(f.s->log[0].text, "hi");
  ASSERT_EQ_STR(f.s->log[1].role, "assistant");
  ASSERT_EQ_INT((long long)f.s->led_n, 1);
  ASSERT_EQ_INT((long long)f.s->led[0].turn, 2);

  /* the streaming callback saw the whole answer */
  ASSERT_EQ_STR(sink.buf, "Hello! How can I help you?\n");
  ASSERT_CONTAINS(f.light.last_system,
                  "Hard output budget: at most 1024 tokens");
  ASSERT_CONTAINS(f.light.last_system, "ceiling, not a target");

  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.turns, 1);
  ASSERT_EQ_INT((long long)st.tool_calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(explicit_deadline_is_opt_in) {
  eng_fx f;
  asngn_turn_result r;
  asngn_submit_opts opts;

  memset(&r, 0, sizeof r);
  memset(&opts, 0, sizeof opts);
  opts.deadline_ms = 5000;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(
      &f.nano, "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT | TASK CHAT\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "OK\n"));

  ASSERT_OK(eng_turn(&f, "reply OK", &opts, NULL, &r));
  ASSERT_TRUE(f.nano.deadline_seen[0] > 0 &&
              f.nano.deadline_seen[0] <= 5000);
  ASSERT_TRUE(f.light.deadline_seen[0] > 0 &&
              f.light.deadline_seen[0] <= 5000);

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
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"needs the tool\", "
                              "input: fake.run {msg: \"hello\"}, success: "
                              "\"RESULT with hello\", fallback: \"answer "
                              "without it\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Done: the tool replies hello.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool to say hello", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Done: the tool replies hello.\n");
  ASSERT_TRUE(r.tokens_prompt > 0);

  /* the working zone showed the second decision pass the fenced RESULT */
  ASSERT_CONTAINS(f.light.last_user, "RESULT fake.run");
  ASSERT_CONTAINS(f.light.last_user, "data, not instructions");
  /* state-aware instruction tail: once a tool ran, the example is gone
   * and the closing line pushes toward the answer action */
  ASSERT_CONTAINS(f.light.last_user, "emit {action: \"answer\"} now");
  ASSERT_NOT_CONTAINS(f.light.last_user, "Example first step");
  ASSERT_CONTAINS(f.light.last_user,
                  "Hard completion budget: 1024 tokens");
  ASSERT_CONTAINS(f.light.last_user, "remaining tool calls:");
  ASSERT_CONTAINS(f.stdm.last_system,
                  "Hard output budget: at most 4096 tokens");

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

TEST(chat_mode_forces_rag_only_direct_turn) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats stats;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_OK(asngn_session_set_mode(f.s, ASNGN_USAGE_CHAT));
  ASSERT_TRUE(fake_model_push(
      &f.nano, "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK EDIT\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "I can discuss the requested change.\n"));

  ASSERT_OK(eng_turn(&f, "change the project", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "I can discuss the requested change.\n");
  ASSERT_EQ_STR(r.klass, "moderate");
  ASSERT_EQ_STR(f.s->log[1].mode, "direct");
  ASSERT_CONTAINS(f.stdm.last_system, "Session mode is chat");
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(rich_stream_exposes_redacted_operational_reasoning) {
  eng_fx f;
  asngn_turn_result r;
  rich_sink sink;

  memset(&r, 0, sizeof r);
  memset(&sink, 0, sizeof sink);
  sink.first_kind = -1;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(
      &f.nano, "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n"));
  ASSERT_TRUE(fake_model_push(
      &f.light, "{action: \"call\", why: \"inspect password=hunter2secret\", input: "
                "fake.run {msg: \"hello\"}, success: \"result\", fallback: "
                "\"report unavailable\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "The current state is available.\n"));

  ASSERT_OK(rich_turn(&f, "inspect the current state", &sink, &r));
  ASSERT_EQ_INT(sink.first_kind, ASNGN_STREAM_REASONING);
  ASSERT_CONTAINS(sink.reasoning, "redacted");
  ASSERT_NOT_CONTAINS(sink.reasoning, "hunter2secret");
  ASSERT_EQ_STR(sink.output, r.answer);
  ASSERT_EQ_STR(sink.notice, "");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(readonly_profile_notifies_without_aborting_turn) {
  eng_fx f;
  asngn_turn_result r;
  rich_sink sink;
  asngn_stats stats;

  memset(&r, 0, sizeof r);
  memset(&sink, 0, sizeof sink);
  sink.first_kind = -1;
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_OK(asngn_session_set_security_profile(
      f.s, ASNGN_SECURITY_CODING_READONLY));
  ASSERT_TRUE(fake_model_push(
      &f.nano, "CLASS COMPLEX | DETAIL NORMAL | MODE PLAN | TASK EDIT\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm, "{action: \"call\", why: \"apply requested edit\", input: "
               "fake.mut {msg: \"change\"}, success: \"updated\", fallback: "
               "\"report permission\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm, "The edit needs a writable security profile.\n"));

  ASSERT_OK(rich_turn(&f, "edit the project", &sink, &r));
  ASSERT_CONTAINS(sink.notice, "coding-readonly");
  ASSERT_CONTAINS(r.answer, "writable security profile");
  ASSERT_CONTAINS(f.stdm.last_system, "lacked authorization");
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── b2. generate turns are outcome-gated ─────────────────────────────── */

/* A generation ask whose planner tries to answer before writing an artifact
 * fails closed. Re-running DECIDE against unchanged state wastes the complete
 * prompt and can repeat until the step budget. */
TEST(generate_outcome_gate) {
  eng_fx f;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL RICH | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action: \"answer\"}\n"));
  ASSERT_EQ_INT(eng_turn(&f, "write a calculator in c++", NULL, NULL, &r),
                ASNGN_ERR_PROTOCOL);
  ASSERT_EQ_INT(f.light.calls, 0);
  ASSERT_EQ_INT(f.stdm.calls, 1);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* A path/status follow-up refers to the completed prior turn.  Even a noisy
 * semantic classifier must not replay the earlier mutation or enter the tool
 * loop, and its prompt must include enough recent dialogue to resolve the
 * reference. */
TEST(reference_followup_stays_direct) {
  eng_fx f;
  asngn_turn_result first, second;

  memset(&first, 0, sizeof first);
  memset(&second, 0, sizeof second);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(
      &f.nano, "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT | TASK CHAT\n"));
  ASSERT_TRUE(fake_model_push(
      &f.light, "The generated file is C:/repo/text_adventure.cpp.\n"));
  ASSERT_OK(eng_turn(&f, "hello", NULL, NULL, &first));

  ASSERT_TRUE(fake_model_push(
      &f.nano, "CLASS COMPLEX | DETAIL TERSE | MODE PLAN | TASK GENERATE\n"));
  ASSERT_TRUE(fake_model_push(
      &f.light, "C:/repo/text_adventure.cpp\n"));
  ASSERT_OK(eng_turn(&f,
                     "i simply need the full path of text_adventure.cpp",
                     NULL, NULL, &second));

  ASSERT_EQ_STR(second.answer, "C:/repo/text_adventure.cpp\n");
  ASSERT_EQ_INT(f.light.calls, 2);
  ASSERT_EQ_INT(f.stdm.calls, 0);
  ASSERT_EQ_INT((long long)f.s->log_n, 4);
  ASSERT_EQ_STR(f.s->log[3].mode, "direct");
  ASSERT_CONTAINS(f.nano.last_user, "conversation_context:");
  ASSERT_CONTAINS(f.nano.last_user,
                  "assistant: The generated file is "
                  "C:/repo/text_adventure.cpp.");
  ASSERT_CONTAINS(f.nano.last_user, "current_message:");

  asngn_turn_result_free(&first);
  asngn_turn_result_free(&second);
  eng_drop(&f);
}

/* The semantic classifier is useful for multilingual intent, but it is not
 * allowed to erase deterministic complexity/detail evidence. */
TEST(model_classifier_cannot_downgrade_quality) {
  static const char SMALL_CTX[] =
      "models: { pool: ["
      "{ id: \"nano\", path: \"fake-nano.gguf\", ctx: 4096 },"
      "{ id: \"light\", path: \"fake-light.gguf\", ctx: 4096 },"
      "{ id: \"std\", path: \"fake-std.gguf\", ctx: 4096 },"
      "{ id: \"embed\", path: \"fake-embed.gguf\", ctx: 512, "
      "embedding: true, dim: 16 }], roles: { router: \"nano\", "
      "planner: \"light\", generator: \"std\", compressor: \"light\", "
      "adapter: \"light\", judge: \"light\", embedder: \"embed\" } },"
      "context: { safety_margin: 256 },";
  eng_fx f;
  asngn_turn_result r;
  char msg[702];
  const char *budget;
  int announced;

  memset(&r, 0, sizeof r);
  memset(msg, 'a', 700);
  msg[700] = '.';
  msg[701] = '\0';
  ASSERT_TRUE(eng_setup(&f, "echo", SMALL_CTX));
  ASSERT_TRUE(fake_model_push(
      &f.nano, "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT | TASK CHAT\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "Complete answer.\n"));

  ASSERT_OK(eng_turn(&f, msg, NULL, NULL, &r));
  ASSERT_EQ_STR(r.detail, "rich");
  budget = strstr(f.light.last_system, "Hard output budget: at most ");
  ASSERT_TRUE(budget != NULL);
  budget += strlen("Hard output budget: at most ");
  announced = atoi(budget);
  ASSERT_TRUE(announced > 0 && announced < 4096);
  ASSERT_NOT_CONTAINS(f.light.last_system, "at most 10240 tokens");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* Regression for session s-a0ba4048: source payload generation and tool
 * selection are separate model phases.  The decision pass emits a
 * short marker call; the generator drafts the complete payload privately;
 * only the clean response is delivered to the user. OpenAI-compatible
 * models may spell the configured workspace as C:/workspace; that virtual
 * alias is normalized without widening the filesystem grant. */
TEST(generate_draft_then_write) {
  eng_fx f;
  eng_sink sink;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  memset(&sink, 0, sizeof sink);
  ASSERT_TRUE(eng_setup_tool(&f, "fs", "echo",
                             "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL RICH | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"create calculator source\", input: "
      "fs.write {path: \"C:/workspace/calculator.cpp\", content: "
      "\"@asngn:draft\"}, "
      "success: \"bytes written\", fallback: \"report failure\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "#include <iostream>\n\nint main() { std::cout << 2 + 2 << '\\n'; }\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              "Created calculator.cpp in the workspace.\n"));

  ASSERT_OK(eng_turn(&f, "write a calculator in c++", NULL, &sink, &r));
  ASSERT_EQ_STR(r.answer, "Created calculator.cpp in the workspace.\n");
  ASSERT_EQ_STR(sink.buf, r.answer);
  ASSERT_NOT_CONTAINS(r.answer, "fs.write");
  ASSERT_NOT_CONTAINS(r.answer, "CALL ");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 1);
  ASSERT_EQ_INT(f.light.calls, 0);
  ASSERT_EQ_INT(f.stdm.calls, 4); /* decision + draft + decision + response */
  ASSERT_NOT_CONTAINS(f.stdm.last_system, "## Tools");
  ASSERT_CONTAINS(f.stdm.last_system, "trusted engine metadata");
  ASSERT_CONTAINS(f.stdm.last_user, "write succeeded");

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* Regression for s-2a54b929: Qwen wrapped valid C++ in an outer Markdown
 * fence twice.  DRAFT normalizes that transport wrapper before xCDN
 * escaping; neither the fence nor any draft bytes reach chat. */
TEST(generate_fenced_draft_then_write) {
  eng_fx f;
  eng_sink sink;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  memset(&sink, 0, sizeof sink);
  ASSERT_TRUE(eng_setup_tool(&f, "fs", "echo",
                             "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL RICH | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"create calculator source\", input: "
      "fs.write {path: \"calculator.cpp\", content: \"@asngn:draft\"}, "
      "success: \"bytes written\", fallback: \"report failure\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "```cpp\n#include <iostream>\n\nint main() { return 0; }\n```\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              "Created calculator.cpp in the workspace.\n"));

  ASSERT_OK(eng_turn(&f, "create a calculator in c++", NULL, &sink, &r));
  ASSERT_EQ_STR(r.answer, "Created calculator.cpp in the workspace.\n");
  ASSERT_EQ_STR(sink.buf, r.answer);
  ASSERT_EQ_INT(f.light.calls, 0);
  ASSERT_EQ_INT(f.stdm.calls, 4);
  ASSERT_NOT_CONTAINS(sink.buf, "#include");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 1);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* A private draft can consume its full output allowance without losing the
 * bytes already paid for. The next call receives an exact suffix/hash resume
 * contract and only generates the remainder; the tool sees one complete
 * artifact and the turn terminates normally. */
TEST(generate_draft_limit_continues_losslessly) {
  eng_fx f;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup_tool(&f, "fs", "echo",
                             "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL RICH | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"create source\", input: "
      "fs.write {path: \"main.c\", content: \"@asngn:draft\"}, "
      "success: \"bytes written\", fallback: \"report failure\"}\n"));
  ASSERT_TRUE(fake_model_push_partial_limit(
      &f.stdm, "#include <stdio.h>\nint main(void) {"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              " puts(\"ok\"); return 0; }\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "Created main.c.\n"));

  ASSERT_OK(eng_turn(&f, "create main.c", NULL, NULL, &r));
  ASSERT_EQ_STR(r.answer, "Created main.c.\n");
  ASSERT_EQ_INT(f.stdm.calls, 5);
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 1);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* The marker is a reserved capability token for fs.write.content only.
 * Putting it in path must fail before DRAFT, confirmation, or dispatch;
 * the engine then escalates the decision tier instead of killing the turn. */
TEST(generate_draft_marker_rejected_outside_content) {
  eng_fx f;
  eng_sink sink;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  memset(&sink, 0, sizeof sink);
  ASSERT_TRUE(eng_setup_tool(&f, "fs", "echo",
                             "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL RICH | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"call\", why: \"create calculator source\", input: "
      "fs.write {path: \"@asngn:draft\", content: \"source\"}, success: "
      "\"bytes written\", fallback: \"report failure\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "{action: \"clarify\", why: \"the requested path was invalid\", "
      "input: \"Which workspace-relative path should I use?\"}\n"));

  ASSERT_OK(eng_turn(&f, "create a calculator in c++", NULL, &sink, &r));
  ASSERT_CONTAINS(r.answer, "workspace-relative path");
  ASSERT_EQ_STR(sink.buf, "");
  ASSERT_EQ_INT(f.stdm.calls, 2);
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* A response-shaped pseudo-call is rejected before streaming.  The retry is
 * the only text that becomes visible, so a tool invocation can never be
 * printed into chat and mistaken for an executed action. */
TEST(response_tool_protocol_never_streams) {
  eng_fx f;
  eng_sink sink;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  memset(&sink, 0, sizeof sink);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"read data\", "
                              "input: fake.run {msg: \"a\"}, success: "
                              "\"RESULT\", fallback: \"report failure\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(
      &f.stdm,
      "I will run it now.\nCALL fake.run {msg: \"a\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "The operation completed.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool", NULL, &sink, &r));
  ASSERT_EQ_STR(r.answer, "The operation completed.\n");
  ASSERT_EQ_STR(sink.buf, r.answer);
  ASSERT_NOT_CONTAINS(sink.buf, "CALL ");
  ASSERT_EQ_INT(f.stdm.calls, 2);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* The exact failure shape from s-a0ba4048 must fail closed: malformed
 * planner passes, repeated THINKs and a final protocol failure never open the
 * response phase and therefore cannot print a fabricated fs.write call. */
TEST(generate_decision_failure_never_answers) {
  eng_fx f;
  eng_sink sink;
  asngn_turn_result r;
  asngn_stats st;

  memset(&r, 0, sizeof r);
  memset(&sink, 0, sizeof sink);
  ASSERT_TRUE(eng_setup(&f, "echo",
                        "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL RICH | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "unfinished planner output"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "still not an action"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              "{action: \"think\", input: \"draft code\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              "{action: \"think\", input: \"draft code\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              "{action: \"think\", input: \"draft code\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "unfinished generator output"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "CALL fs.write {path: \"x\"}"));

  ASSERT_ERR(eng_turn(&f, "write a calculator in c++", NULL, &sink, &r),
             ASNGN_ERR_PROTOCOL);
  ASSERT_EQ_STR(sink.buf, "");
  ASSERT_OK(asngn_get_stats(f.c, &st));
  ASSERT_EQ_INT((long long)st.tool_calls, 0);

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
      &f.light,
      "{action: \"call\", why: \"needs the tool\", input: fake.run "
      "{msg: \"password=hunter2secret\"}, success: \"RESULT ok\", "
      "fallback: \"stop here\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
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
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"need to edit "
                              "x\", input: fake.mut {msg: \"x\"}, success: "
                              "\"RESULT ok\", fallback: \"ask for "
                              "confirmation\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm,
                              "I cannot: that command must be confirmed.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool to edit x", NULL, NULL,
                     &r));
  ASSERT_EQ_STR(r.answer, "I cannot: that command must be confirmed.\n");

  /* the model saw the deny as an ERROR line; nothing was dispatched.
   * Its own declared fallback was echoed to steer the recovery pass. */
  ASSERT_CONTAINS(f.light.last_user, "asngn/confirm-required");
  ASSERT_CONTAINS(f.light.last_user,
                  "your declared fallback: ask for confirmation");
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
                              "{action: \"clarify\", why: \"the file name "
                              "is missing\", input: \"Which file should I "
                              "edit?\"}\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool on the right file", NULL, NULL,
                     &r));
  ASSERT_EQ_INT(r.clarify, 1);
  ASSERT_EQ_STR(r.answer, "Which file should I edit?");

  /* ledger class "clarify"; the generator never ran */
  ASSERT_EQ_INT((long long)f.s->led_n, 1);
  ASSERT_EQ_STR(f.s->led[0].klass, "clarify");
  ASSERT_EQ_INT(f.stdm.calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── d2. degenerate decisions fail closed without regeneration ───────── */

TEST(degenerate_decide_fails_without_regeneration) {
  /* Regression for session s-d569146b: the light planner rambled inside
   * a clarify object, the decide cap truncated the output (no trailing
   * newline), and the junk shipped verbatim as the answer. Now pass 1
   * (truncated: no newline) and pass 2 (complete but in the retired
   * line protocol) must not ship.  A malformed constrained result now fails
   * closed; silently regenerating it would spend another full prompt and
   * hide a provider-contract violation. */
  eng_fx f;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"clarify\", why: \"Do you need "
                              "me to perform any specific action with "
                              "these files? Do you need me to perform "
                              "any specific"));
  ASSERT_EQ_INT(eng_turn(&f, "list the files in the folder", NULL, NULL, &r),
                ASNGN_ERR_PROTOCOL);
  ASSERT_EQ_INT(f.light.calls, 1);
  ASSERT_EQ_INT(f.stdm.calls, 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(decide_token_limit_fails_without_regeneration) {
  eng_fx f;
  asngn_turn_result r;
  asngn_err e;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL NORMAL | MODE PLAN | "
                              "TASK DEBUG\n"));
  ASSERT_TRUE(fake_model_push_error(&f.stdm, ASNGN_ERR_LIMIT));

  e = eng_turn(&f, "debug the reported failure", NULL, NULL, &r);
  ASSERT_EQ_INT(e, ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(f.stdm.calls, 1);
  ASSERT_EQ_INT(f.stdm.max_tokens_seen[0], 1024);
  ASSERT_EQ_INT(f.stdm.reasoning_seen[0],
                ASMODEL_REASONING_REQUIRED_OFF);
  ASSERT_EQ_INT(f.stdm.require_constraint_seen[0], 1);
  ASSERT_EQ_INT(f.stdm.deadline_seen[0], 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(decide_deadline_fails_without_regeneration) {
  eng_fx f;
  asngn_turn_result r;
  asngn_err e;

  memset(&r, 0, sizeof r);
  ASSERT_TRUE(eng_setup(&f, "echo", NULL));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS COMPLEX | DETAIL NORMAL | MODE PLAN | "
                              "TASK DEBUG\n"));
  ASSERT_TRUE(fake_model_push_error(&f.stdm, ASNGN_ERR_TIMEOUT));

  e = eng_turn(&f, "debug the deadline failure", NULL, NULL, &r);
  ASSERT_EQ_INT(e, ASNGN_ERR_TIMEOUT);
  ASSERT_EQ_INT(f.stdm.calls, 1);
  ASSERT_TRUE(strstr(asngn_last_error(f.c), "deadline expired") != NULL);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── e. oversized tool result is digested into a blob ────────────────── */

TEST(digestion) {
  eng_fx f;
  asngn_turn_result r;

  memset(&r, 0, sizeof r);
  /* behavior "big": result {data: "<8000 x's>"}; threshold 512 chars.
   * The digest compressor shares the light model, so the light queue is,
   * IN ORDER: decision CALL, the compressor's digest text, decision
   * ANSWER (loop.c step_call -> digest.c asngn_digest_item). */
  ASSERT_TRUE(eng_setup_asper(
      &f, "big", "context: { digest_threshold_chars: 512, "
                 "digest_tokens: 64 },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS MODERATE | DETAIL NORMAL | MODE PLAN\n"));
  ASSERT_TRUE(fake_model_push(&f.light,
                              "{action: \"call\", why: \"read everything\", "
                              "input: fake.run {msg: \"hello\"}, success: "
                              "\"RESULT complete\", fallback: \"answer "
                              "without it\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "digest of the result"));
  ASSERT_TRUE(fake_model_push(&f.light, "{action: \"answer\"}\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "The result was huge.\n"));

  ASSERT_OK(eng_turn(&f, "use the fake tool to read everything", NULL, NULL,
                     &r));
  ASSERT_EQ_STR(r.answer, "The result was huge.\n");

  /* the second decision pass saw the digested line, not the raw result */
  ASSERT_CONTAINS(f.light.last_user, "[B1");
  ASSERT_CONTAINS(f.light.last_user, "digested]");
  ASSERT_CONTAINS(f.light.last_user, "digest of the result");
  ASSERT_NOT_CONTAINS(f.light.last_user, "xxxxxxxxxxxxxxxx");

  /* The exact payload is an Asper content-addressed object, not an
   * ASNGN-owned session blob file. */
  ASSERT_EQ_INT((long long)f.s->blobs_n, 1);
  ASSERT_CONTAINS(f.s->blobs[0].object_ref, "sha256:");

  /* the ledger accounted the saving */
  ASSERT_EQ_INT((long long)f.s->led_n, 1);
  ASSERT_TRUE(f.s->led[0].sv_digest > 0);
  ASSERT_TRUE(r.tokens_saved > 0);

  asngn_turn_result_free(&r);
  eng_drop(&f);
}

/* ── f. capped terse answer + /more continuation ─────────────────────── */

/* 72 bytes per sentence x 8 = 576 bytes => 144 heuristic tokens. This test
 * deliberately overrides the professional default with a tiny 128-token
 * cap so it can exercise continuation without generating a huge fixture. */
#define ENG_SENT \
  "This sentence fills the answer with many extra words to exceed the " \
  "cap. "

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
  ASSERT_TRUE(eng_setup(&f, "echo", "detail: { terse_tokens: 128 },"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  /* both turns are SIMPLE+DIRECT: the answers run on light (start-down) */
  ASSERT_TRUE(fake_model_push(&f.light, LONG_ANSWER));
  ASSERT_TRUE(fake_model_push(&f.light, "continuing here.\n"));

  ASSERT_OK(eng_turn(&f, "tell me something nice", NULL, NULL, &r1));
  ASSERT_EQ_INT(r1.capped, 1);
  ASSERT_EQ_STR(r1.detail, "terse");
  ASSERT_TRUE(r1.answer != NULL && r1.answer[0] != '\0');

  /* /more continues the capped answer as turn 3 (no new user turn) */
  ASSERT_OK(asngn_more(f.s, NULL, NULL, &t));
  ASSERT_OK(asngn_task_wait(t, 60000, &r2));
  asngn_task_free(t);
  ASSERT_EQ_INT(r2.turn, 3);
  ASSERT_EQ_STR(r2.answer, "continuing here.\n");
  ASSERT_EQ_INT((long long)f.s->log_n, 3);
  ASSERT_EQ_STR(f.s->log[2].role, "assistant");
  ASSERT_EQ_STR(f.s->log[2].text, "continuing here.\n");

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
  ASSERT_TRUE(fake_model_push(&f.light, "first answer.\n"));
  ASSERT_TRUE(fake_model_push(&f.stdm, "a better answer.\n"));

  ASSERT_OK(eng_turn(&f, "give me an answer", NULL, NULL, &r1));
  ASSERT_EQ_STR(r1.answer, "first answer.\n");
  ASSERT_EQ_STR(r1.tier, "light");
  light_calls_after_first = f.light.calls;

  ASSERT_OK(asngn_retry(f.s, NULL, NULL, &t));
  ASSERT_OK(asngn_task_wait(t, 60000, &r2));
  asngn_task_free(t);
  ASSERT_EQ_STR(r2.answer, "a better answer.\n");
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

/* Asper is the authoritative transcript/context path in normal operation:
 * exact source events feed the next turn, survive a session reopen, and no
 * ASNGN-owned transcript or rolling-summary file is created. */
TEST(asper_source_context_and_reopen) {
  eng_fx f;
  asngn_turn_result r1, r2;
  char legacy_transcript[512], legacy_summary[512], event_log[512];

  memset(&r1, 0, sizeof r1);
  memset(&r2, 0, sizeof r2);
  ASSERT_TRUE(eng_setup_asper(&f, "echo", NULL));
  ASSERT_TRUE(f.c->asper_ok);
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.nano,
                              "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "alpha exact answer.\n"));
  ASSERT_TRUE(fake_model_push(&f.light, "beta answer.\n"));

  ASSERT_OK(eng_turn(&f, "remember alpha exactly", NULL, NULL, &r1));
  ASSERT_OK(eng_turn(&f, "what came before?", NULL, NULL, &r2));
  ASSERT_CONTAINS(f.light.last_user, "alpha exact answer.");

  snprintf(legacy_transcript, sizeof legacy_transcript,
           "%s/sessions/s1/transcript.xcdn", f.root_raw);
  snprintf(legacy_summary, sizeof legacy_summary,
           "%s/sessions/s1/summary.xcdn", f.root_raw);
  snprintf(event_log, sizeof event_log,
           "%s/memory/scopes/s1/events.log", f.root_raw);
  ASSERT_TRUE(!os_file_exists(legacy_transcript));
  ASSERT_TRUE(!os_file_exists(legacy_summary));
  ASSERT_TRUE(os_file_exists(event_log));

  asngn_session_close(f.s);
  f.s = NULL;
  ASSERT_OK(asngn_session_open(f.c, "s1", &f.s));
  ASSERT_EQ_INT(f.s->log_n, 4);
  ASSERT_EQ_STR(f.s->log[0].text, "remember alpha exactly");
  ASSERT_EQ_STR(f.s->log[1].text, "alpha exact answer.\n");
  ASSERT_EQ_STR(f.s->log[2].text, "what came before?");
  ASSERT_EQ_STR(f.s->log[3].text, "beta answer.\n");

  asngn_turn_result_free(&r1);
  asngn_turn_result_free(&r2);
  eng_drop(&f);
}

/* ── runner ───────────────────────────────────────────────────────────── */

TEST_LIST = {
  TEST_ENTRY(direct_chat),
  TEST_ENTRY(explicit_deadline_is_opt_in),
  TEST_ENTRY(plan_tool_turn),
  TEST_ENTRY(chat_mode_forces_rag_only_direct_turn),
  TEST_ENTRY(rich_stream_exposes_redacted_operational_reasoning),
  TEST_ENTRY(readonly_profile_notifies_without_aborting_turn),
  TEST_ENTRY(reference_followup_stays_direct),
  TEST_ENTRY(model_classifier_cannot_downgrade_quality),
  TEST_ENTRY(generate_outcome_gate),
  TEST_ENTRY(generate_draft_then_write),
  TEST_ENTRY(generate_fenced_draft_then_write),
  TEST_ENTRY(generate_draft_limit_continues_losslessly),
  TEST_ENTRY(generate_draft_marker_rejected_outside_content),
  TEST_ENTRY(response_tool_protocol_never_streams),
  TEST_ENTRY(generate_decision_failure_never_answers),
  TEST_ENTRY(tool_call_log_hashes_secret_args),
  TEST_ENTRY(confirm_deny_headless),
  TEST_ENTRY(clarify_turn),
  TEST_ENTRY(degenerate_decide_fails_without_regeneration),
  TEST_ENTRY(decide_token_limit_fails_without_regeneration),
  TEST_ENTRY(decide_deadline_fails_without_regeneration),
  TEST_ENTRY(digestion),
  TEST_ENTRY(more_continuation),
  TEST_ENTRY(retry_up),
  TEST_ENTRY(tool_permissions),
  TEST_ENTRY(asper_source_context_and_reopen),
};

RUN_ALL_TESTS()
