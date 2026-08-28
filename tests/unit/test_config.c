/*
 * test_config.c — defaults, xCDN overlay, hard errors, WARN-skips.
 *
 * Uses a bare context (config_load needs err_mu and the NULL log path
 * only). Config files are written into a private tmpdir.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

static asngn_ctx *bare_ctx(void) {
  asngn_ctx *c = (asngn_ctx *)calloc(1, sizeof *c);
  if (c == NULL) return NULL;
  os_mutex_init(&c->err_mu);
  os_mutex_init(&c->log_mu);
  c->clock = asngn_clock_system();
  return c;
}

static void bare_ctx_free(asngn_ctx *c) {
  if (c == NULL) return;
  os_mutex_destroy(&c->err_mu);
  os_mutex_destroy(&c->log_mu);
  free(c);
}

static int write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  size_t len = strlen(text);
  if (f == NULL) return 0;
  if (fwrite(text, 1, len, f) != len) {
    fclose(f);
    return 0;
  }
  return fclose(f) == 0;
}

TEST(defaults_spot_check) {
  asngn_config cfg;
  asngn_config_defaults(&cfg);

  /* models */
  ASSERT_EQ_INT((long long)cfg.pool_n, 4);
  ASSERT_EQ_STR(cfg.pool[0].id, "nano");
  ASSERT_EQ_STR(cfg.pool[1].id, "light");
  ASSERT_EQ_STR(cfg.pool[2].id, "std");
  ASSERT_EQ_STR(cfg.pool[3].id, "embed");
  ASSERT_TRUE(cfg.pool[3].embedding);
  ASSERT_EQ_INT(cfg.pool[3].dim, 384);
  ASSERT_EQ_STR(cfg.role_router, "nano");
  ASSERT_EQ_STR(cfg.role_generator, "std");
  ASSERT_EQ_STR(cfg.role_embedder, "embed");
  ASSERT_EQ_INT(cfg.max_resident, 3);
  ASSERT_EQ_INT(cfg.pool[0].ctx, 8192);
  ASSERT_EQ_INT(cfg.pool[1].ctx, 32768);
  ASSERT_EQ_INT(cfg.pool[2].ctx, 32768);

  /* sampling defaults */
  ASSERT_EQ_DBL(cfg.s_classify.temp, 0.0, 1e-9);
  ASSERT_EQ_INT(cfg.s_classify.max_tokens, 64);
  ASSERT_EQ_DBL(cfg.s_classify.repeat_penalty, 0.0, 1e-9);
  ASSERT_EQ_DBL(cfg.s_decide.repeat_penalty, 1.15, 1e-9);
  ASSERT_EQ_DBL(cfg.s_draft.temp, 0.2, 1e-9);
  ASSERT_EQ_DBL(cfg.s_draft.top_p, 0.9, 1e-9);
  ASSERT_EQ_INT(cfg.s_draft.max_tokens, 0);          /* auto: free context */
  ASSERT_EQ_DBL(cfg.s_answer.temp, 0.4, 1e-9);
  ASSERT_EQ_DBL(cfg.s_answer.top_p, 0.9, 1e-9);
  ASSERT_EQ_DBL(cfg.s_answer.repeat_penalty, 0.0, 1e-9);
  ASSERT_EQ_INT(cfg.s_answer.max_tokens, 0);
  ASSERT_EQ_DBL(cfg.s_judge.temp, 0.0, 1e-9);
  ASSERT_EQ_INT(cfg.s_judge.max_tokens, 128);

  /* routing / detail */
  ASSERT_EQ_INT(cfg.classifier, ASNGN_CLASSIFIER_HYBRID);
  ASSERT_EQ_INT(cfg.max_escalations, 2);
  ASSERT_EQ_INT(cfg.detail_default, ASNGN_DETAIL_AUTO);
  ASSERT_EQ_INT(cfg.terse_tokens, 1024);
  ASSERT_EQ_INT(cfg.normal_tokens, 4096);
  ASSERT_EQ_INT(cfg.rich_tokens, 10240);

  /* context */
  ASSERT_EQ_INT(cfg.summary_tokens, 3072);
  ASSERT_EQ_INT(cfg.verbatim_tokens, 8192);
  ASSERT_EQ_INT(cfg.working_tokens, 6144);
  ASSERT_EQ_INT(cfg.safety_margin, 512);
  ASSERT_EQ_INT(cfg.fold_tokens, 1536);
  ASSERT_EQ_INT(cfg.digest_threshold_chars, 32768);
  ASSERT_EQ_INT(cfg.digest_tokens, 2048);
  ASSERT_EQ_INT(cfg.pinned_max, 32);

  /* cache thresholds and TTLs */
  ASSERT_TRUE(cfg.cache_enable);
  ASSERT_EQ_INT(cfg.cache_scope, ASNGN_SCOPE_SESSION);
  ASSERT_EQ_DBL(cfg.hit_threshold, 0.95, 1e-9);
  ASSERT_EQ_DBL(cfg.adapt_threshold, 0.85, 1e-9);
  ASSERT_EQ_INT(cfg.cache_ttl_s, 7 * 24 * 3600);
  ASSERT_EQ_INT(cfg.cache_max_entries, 4096);
  ASSERT_TRUE(cfg.tool_cache);
  ASSERT_EQ_INT(cfg.tool_ttl_s, 300);

  /* safety */
  ASSERT_EQ_INT(cfg.max_steps, 16);
  ASSERT_EQ_INT(cfg.max_tool_calls, 8);
  ASSERT_EQ_INT(cfg.turn_deadline_s, 120);
  ASSERT_EQ_INT(cfg.stall_timeout_s, 20);
  ASSERT_EQ_INT(cfg.autoconfirm, ASNGN_CONFIRM_PROMPT);
  ASSERT_TRUE(cfg.redact_context);

  /* validation / budgets / logging / tui / integration / mcp */
  ASSERT_EQ_INT(cfg.judge, ASNGN_JUDGE_LIGHT);
  ASSERT_EQ_INT(cfg.judge_threshold, 6);
  ASSERT_EQ_DBL(cfg.warn_at, 0.80, 1e-9);
  ASSERT_EQ_INT(cfg.log_level, ASNGN_LOG_INFO);
  ASSERT_TRUE(cfg.log_path == NULL);
  ASSERT_EQ_INT(cfg.theme, ASNGN_THEME_ASTERISM);
  ASSERT_EQ_STR(cfg.sidebar, "trace");
  ASSERT_EQ_INT(cfg.catalog_level, ASNGN_CATALOG_SUMMARY);
  ASSERT_EQ_INT(cfg.catalog_chars, 24000);
  ASSERT_EQ_INT(cfg.mcp_autoconfirm, ASNGN_CONFIRM_DENY);

  asngn_config_free(&cfg);
}

TEST(overlay_overrides) {
  static const char *body =
    "#asngn_config {\n"
    "  detail: { default: \"terse\" },\n"
    "  context: { summary_tokens: 300 },\n"
    "  cache: { ttl: r\"P1D\" },\n"
    "  safety: { autoconfirm: \"deny\" },\n"
    "  models: {\n"
    "    roles: { generator: \"light\" },\n"
    "    pool: [\n"
    "      { id: \"big\", path: \"models/big.gguf\", ctx: 8192, threads: 8 },\n"
    "      { id: \"vec\", path: \"models/vec.gguf\", embedding: true,"
    " dim: 128 },\n"
    "    ],\n"
    "    sampling: { draft: { max_tokens: 12000 }, answer: { temp: 0.7 },"
    " decide: { repeat_penalty: 1.3 } },\n"
    "  },\n"
    "  integration: { astls: { workspace: \"session\", "
    "catalog_chars: 1234 } },\n"
    "  routing: { max_escalations: 0 },\n"
    "}\n";
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  asngn_config cfg;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/config.xcdn", dir);
  ASSERT_TRUE(write_text(path, body));

  asngn_config_defaults(&cfg);
  ASSERT_OK(asngn_config_load(c, &cfg, path));

  ASSERT_EQ_INT(cfg.detail_default, ASNGN_DETAIL_TERSE);
  ASSERT_EQ_INT(cfg.summary_tokens, 300);
  ASSERT_EQ_INT(cfg.cache_ttl_s, 86400);            /* r"P1D"           */
  ASSERT_EQ_INT(cfg.autoconfirm, ASNGN_CONFIRM_DENY);
  ASSERT_EQ_STR(cfg.role_generator, "light");
  ASSERT_EQ_INT((long long)cfg.pool_n, 2);          /* pool replaced    */
  ASSERT_EQ_STR(cfg.pool[0].id, "big");
  ASSERT_EQ_STR(cfg.pool[0].path, "models/big.gguf");
  ASSERT_EQ_INT(cfg.pool[0].ctx, 8192);
  ASSERT_EQ_INT(cfg.pool[0].threads, 8);
  ASSERT_EQ_STR(cfg.pool[1].id, "vec");
  ASSERT_TRUE(cfg.pool[1].embedding);
  ASSERT_EQ_INT(cfg.pool[1].ctx, 512);              /* implicit embed ctx */
  ASSERT_EQ_INT(cfg.pool[1].dim, 128);
  ASSERT_EQ_DBL(cfg.s_answer.temp, 0.7, 1e-9);
  ASSERT_EQ_INT(cfg.s_draft.max_tokens, 12000);
  ASSERT_EQ_DBL(cfg.s_answer.top_p, 0.9, 1e-9);     /* untouched        */
  ASSERT_EQ_DBL(cfg.s_decide.repeat_penalty, 1.3, 1e-9);
  ASSERT_EQ_INT(cfg.s_decide.max_tokens, 1024);     /* untouched        */
  ASSERT_EQ_INT(cfg.catalog_chars, 1234);
  ASSERT_EQ_STR(cfg.astools_workspace, "session");
  ASSERT_EQ_INT(cfg.max_escalations, 0);            /* 0 is allowed     */

  asngn_config_free(&cfg);
  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(hard_errors_are_config) {
  static const char *bodies[] = {
    "#asngn_config { cache: { hit_threshold: 1.5 } }\n",
    "#asngn_config { safety: { turn_deadline: \"nope\" } }\n",
    /* adapt > hit fails the cross-check */
    "#asngn_config { cache: { hit_threshold: 0.9, adapt_threshold: 0.95 } }\n",
    "#asngn_config { detail: { default: \"gigantic\" } }\n",
    /* (0,1) would reward repetition */
    "#asngn_config { models: { sampling: { decide: "
    "{ repeat_penalty: 0.5 } } } }\n",
  };
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  size_t i;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  for (i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
    asngn_config cfg;
    snprintf(path, sizeof path, "%s/bad%d.xcdn", dir, (int)i);
    ASSERT_TRUE(write_text(path, bodies[i]));
    asngn_config_defaults(&cfg);
    ASSERT_ERR(asngn_config_load(c, &cfg, path), ASNGN_ERR_CONFIG);
    asngn_config_free(&cfg);
  }
  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(unknown_keys_warn_and_skip) {
  static const char *body =
    "#asngn_config {\n"
    "  context: { wibble: 1, summary_tokens: 275 },\n"
    "  wibble: { x: 1 },\n"
    "}\n";
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  asngn_config cfg;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/config.xcdn", dir);
  ASSERT_TRUE(write_text(path, body));
  asngn_config_defaults(&cfg);
  ASSERT_OK(asngn_config_load(c, &cfg, path));
  ASSERT_EQ_INT(cfg.summary_tokens, 275); /* known sibling key applied  */
  asngn_config_free(&cfg);
  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(openai_compatible_pool_entry) {
  static const char *body =
    "#asngn_config { models: { max_ram_mb: 12000, max_vram_mb: 8000, "
    "pool: [{ id: \"remote\", backend: \"openai\", "
    "base_url: \"http://127.0.0.1:1234/v1\", model: \"qwen-local\", "
    "api_key_env: \"LM_STUDIO_KEY\", api_grammar: \"llama\", "
    "reasoning_effort: \"none\", "
    "ctx: 16384, warm: false, kv_cache: true }] } }\n";
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  asngn_config cfg;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/config.xcdn", dir);
  ASSERT_TRUE(write_text(path, body));
  asngn_config_defaults(&cfg);
  ASSERT_OK(asngn_config_load(c, &cfg, path));
  ASSERT_EQ_INT((long long)cfg.pool_n, 1);
  ASSERT_EQ_INT(cfg.pool[0].backend, ASMODEL_BACKEND_OPENAI);
  ASSERT_EQ_STR(cfg.pool[0].base_url, "http://127.0.0.1:1234/v1");
  ASSERT_EQ_STR(cfg.pool[0].remote_model, "qwen-local");
  ASSERT_EQ_STR(cfg.pool[0].api_key_env, "LM_STUDIO_KEY");
  ASSERT_EQ_STR(cfg.pool[0].api_grammar, "llama");
  ASSERT_EQ_STR(cfg.pool[0].reasoning_effort, "none");
  ASSERT_EQ_INT(cfg.pool[0].ctx, 16384);
  ASSERT_TRUE(!cfg.pool[0].warm);
  ASSERT_TRUE(cfg.pool[0].kv_cache);
  ASSERT_EQ_INT(cfg.max_ram_mb, 12000);
  ASSERT_EQ_INT(cfg.max_vram_mb, 8000);
  asngn_config_free(&cfg);
  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(bare_object_form_accepted) {
  static const char *body = "{ context: { summary_tokens: 250 } }\n";
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  asngn_config cfg;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/config.xcdn", dir);
  ASSERT_TRUE(write_text(path, body));
  asngn_config_defaults(&cfg);
  ASSERT_OK(asngn_config_load(c, &cfg, path));
  ASSERT_EQ_INT(cfg.summary_tokens, 250);
  asngn_config_free(&cfg);
  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST(starter_configs_parse) {
  static const char *paths[] = {
    ASNGN_TEST_SOURCE_DIR "/examples/embedded.xcdn",
    ASNGN_TEST_SOURCE_DIR "/examples/lmstudio.xcdn",
  };
  size_t i;
  for (i = 0; i < sizeof paths / sizeof paths[0]; i++) {
    asngn_ctx *c = bare_ctx();
    asngn_config cfg;
    ASSERT_TRUE(c != NULL);
    asngn_config_defaults(&cfg);
    ASSERT_OK(asngn_config_load(c, &cfg, paths[i]));
    ASSERT_TRUE(cfg.pool_n >= 2);
    ASSERT_EQ_INT(cfg.profile, ASNGN_PROFILE_CODING);
    asngn_config_free(&cfg);
    bare_ctx_free(c);
  }
}

TEST(missing_file_errors) {
  char dir[256], path[300];
  asngn_ctx *c = bare_ctx();
  asngn_config cfg;
  ASSERT_TRUE(c != NULL);
  ASSERT_TRUE(asngn_test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/no-such-config.xcdn", dir);
  asngn_config_defaults(&cfg);
  /* os_read_file maps ENOENT to ASNGN_ERR_NOT_FOUND; config_load passes
   * the mapped code through asngn_seterr. */
  ASSERT_ERR(asngn_config_load(c, &cfg, path), ASNGN_ERR_NOT_FOUND);
  asngn_config_free(&cfg);
  asngn_test_rmtree(dir);
  bare_ctx_free(c);
}

TEST_LIST = {
  TEST_ENTRY(defaults_spot_check),
  TEST_ENTRY(overlay_overrides),
  TEST_ENTRY(hard_errors_are_config),
  TEST_ENTRY(unknown_keys_warn_and_skip),
  TEST_ENTRY(openai_compatible_pool_entry),
  TEST_ENTRY(bare_object_form_accepted),
  TEST_ENTRY(starter_configs_parse),
  TEST_ENTRY(missing_file_errors),
};

RUN_ALL_TESTS()
