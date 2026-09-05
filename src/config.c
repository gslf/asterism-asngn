/*
 * config.c — asngn configuration: defaults + xCDN overlay.
 *
 * Same discipline as the siblings: every key optional; unknown keys WARN
 * and are skipped; wrong types / bad enums / out-of-range values are hard
 * ASNGN_ERR_CONFIG errors carrying the dotted key path, so a typo can
 * never silently weaken safety settings that do parse.
 *
 * MIT License — per aspera ad astra.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

/* ── defaults ────────────────────────────────────────────────────────── */

static char *cfg_dup(const char *s) { return asngn_strdup(s); }

static void cfg_pool_entry(asngn_pool_entry *e, const char *id,
                           const char *path, int ctx, int threads,
                           bool embedding, int dim, int gpu_layers) {
  memset(e, 0, sizeof *e);
  snprintf(e->id, sizeof e->id, "%s", id);
  e->path = cfg_dup(path);
  e->backend = ASMODEL_BACKEND_EMBEDDED;
  e->ctx = ctx;
  e->threads = threads;
  e->embedding = embedding;
  e->dim = dim;
  e->gpu_layers = gpu_layers;
  e->warm = true;
  e->kv_cache = true;
}

void asngn_config_defaults(asngn_config *cfg) {
  memset(cfg, 0, sizeof *cfg);

  cfg->root = cfg_dup(".");
  cfg->base_prompt = cfg_dup("You are a capable, honest local assistant.");
  cfg->profile = ASNGN_PROFILE_GENERAL;

  cfg_pool_entry(&cfg->pool[0], "nano",
                 "models/qwen2.5-0.5b-instruct-q4_k_m.gguf", 8192, 4,
                 false, 0, -1);
  cfg_pool_entry(&cfg->pool[1], "light",
                 "models/qwen2.5-1.5b-instruct-q4_k_m.gguf", 32768, 4,
                 false, 0, -1);
  cfg_pool_entry(&cfg->pool[2], "std",
                 "models/qwen2.5-7b-instruct-q4_k_m.gguf", 32768, 6,
                 false, 0, -1);
  cfg_pool_entry(&cfg->pool[3], "embed",
                 "models/multilingual-e5-small-q8_0.gguf", 512, 4,
                 true, 384, -1);
  cfg->pool_n = 4;

  snprintf(cfg->role_router, sizeof cfg->role_router, "nano");
  snprintf(cfg->role_planner, sizeof cfg->role_planner, "light");
  snprintf(cfg->role_generator, sizeof cfg->role_generator, "std");
  snprintf(cfg->role_compressor, sizeof cfg->role_compressor, "light");
  snprintf(cfg->role_adapter, sizeof cfg->role_adapter, "light");
  snprintf(cfg->role_judge, sizeof cfg->role_judge, "light");
  snprintf(cfg->role_embedder, sizeof cfg->role_embedder, "embed");
  cfg->max_resident = 3;

  cfg->s_classify.temp = 0.0; cfg->s_classify.top_p = 0.0;
  cfg->s_classify.max_tokens = 64; cfg->s_classify.repeat_penalty = 0.0;
  cfg->s_decide.temp = 0.0; cfg->s_decide.top_p = 0.0;
  cfg->s_decide.max_tokens = 1024; /* professional structured actions */
  /* greedy decode under a grammar can lock into repeating a legal
   * phrase; a mild recent-token penalty breaks the loop */
  cfg->s_decide.repeat_penalty = 1.15;
  cfg->s_draft.temp = 0.2; cfg->s_draft.top_p = 0.9;
  /* 0 means use every token physically available in the model context.
   * Artifact size must never inherit the user-facing detail cap. */
  cfg->s_draft.max_tokens = 0;
  cfg->s_draft.repeat_penalty = 0.0;
  cfg->s_answer.temp = 0.4; cfg->s_answer.top_p = 0.9;
  cfg->s_answer.max_tokens = 0; /* per detail level */
  cfg->s_answer.repeat_penalty = 0.0;
  cfg->s_compress.temp = 0.2; cfg->s_compress.top_p = 0.9;
  cfg->s_compress.max_tokens = 0; /* digest/Asper curator policy */
  cfg->s_compress.repeat_penalty = 0.0;
  cfg->s_adapt.temp = 0.3; cfg->s_adapt.top_p = 0.9;
  cfg->s_adapt.max_tokens = 0; /* per detail level */
  cfg->s_adapt.repeat_penalty = 0.0;
  cfg->s_judge.temp = 0.0; cfg->s_judge.top_p = 0.0;
  cfg->s_judge.max_tokens = 128; cfg->s_judge.repeat_penalty = 0.0;

  cfg->classifier = ASNGN_CLASSIFIER_HYBRID;
  cfg->max_escalations = 2;

  cfg->detail_default = ASNGN_DETAIL_AUTO;
  cfg->terse_tokens = 1024;
  cfg->normal_tokens = 4096;
  cfg->rich_tokens = 10240;

  cfg->memory_checkpoint_tokens = 3072;
  cfg->memory_history_tokens = 8192;
  cfg->working_tokens = 6144;
  cfg->safety_margin = 512;
  cfg->digest_threshold_chars = 32768;
  cfg->digest_tokens = 2048;
  cfg->pinned_max = 32;

  cfg->cache_enable = true;
  cfg->cache_scope = ASNGN_SCOPE_SESSION;
  cfg->hit_threshold = 0.95;
  cfg->adapt_threshold = 0.85;
  cfg->cache_ttl_s = 7 * 24 * 3600;         /* P7D  */
  cfg->cache_max_entries = 4096;
  cfg->tool_cache = true;
  cfg->tool_ttl_s = 5 * 60;                 /* PT5M */
  cfg->tool_max_entries = 512;

  cfg->scheduler_workers = 4;
  cfg->queue_capacity = 64;
  cfg->max_steps = 16;
  cfg->max_tool_calls = 8;
  cfg->think_limit = 2;
  /* Token budgets bound inference work. Time-based cancellation is opt-in:
   * zero lets a slow but progressing model finish under user control. */
  cfg->turn_deadline_s = 0;
  cfg->stall_timeout_s = 0;
  cfg->autoconfirm = ASNGN_CONFIRM_PROMPT;
  cfg->redact_context = true;

  cfg->judge = ASNGN_JUDGE_LIGHT;
  cfg->judge_threshold = 6;

  cfg->session_tokens = 0;
  cfg->daily_tokens = 0;
  cfg->warn_at = 0.80;

  cfg->tele_ring = 4096;
  cfg->tele_path = NULL;
  cfg->tele_rotate_kb = 5120;
  cfg->tele_max_files = 3;

  cfg->log_path = NULL;
  cfg->log_level = ASNGN_LOG_INFO;
  cfg->log_max_size_kb = 5120;
  cfg->log_max_files = 3;
  cfg->log_sync = false;

  cfg->theme = ASNGN_THEME_ASTERISM;
  cfg->truecolor = ASNGN_TRUECOLOR_AUTO;
  cfg->fps_cap = 60;
  snprintf(cfg->sidebar, sizeof cfg->sidebar, "trace");

  cfg->asper_enable = true;
  cfg->asper_root = cfg_dup("memory");
  cfg->asper_config = NULL;
  cfg->astools_enable = true;
  cfg->astools_root = cfg_dup("tools");
  cfg->astools_workspace = cfg_dup("session");
  cfg->astools_config = NULL;
  cfg->catalog_level = ASNGN_CATALOG_SUMMARY;
  cfg->catalog_chars = 24000;

  cfg->mcp_autoconfirm = ASNGN_CONFIRM_DENY;
}

void asngn_config_free(asngn_config *cfg) {
  size_t i;
  if (cfg == NULL) return;
  free(cfg->root);
  free(cfg->base_prompt);
  for (i = 0; i < ASNGN_MAX_POOL; i++) {
    free(cfg->pool[i].path);
    free(cfg->pool[i].base_url);
    free(cfg->pool[i].remote_model);
    free(cfg->pool[i].api_key_env);
  }
  free(cfg->tele_path);
  free(cfg->log_path);
  free(cfg->asper_root);
  free(cfg->asper_config);
  free(cfg->astools_root);
  free(cfg->astools_workspace);
  free(cfg->astools_config);
  memset(cfg, 0, sizeof *cfg);
}

/* ── overlay tables ───────────────────────────────────────────────────── */

typedef struct { const char *name; int value; } enum_map;

static const enum_map CLASSIFIER_MAP[] = {
  { "heuristic", ASNGN_CLASSIFIER_HEURISTIC },
  { "model", ASNGN_CLASSIFIER_MODEL },
  { "hybrid", ASNGN_CLASSIFIER_HYBRID }, { NULL, 0 } };
static const enum_map PROFILE_MAP[] = {
  { "general", ASNGN_PROFILE_GENERAL },
  { "coding", ASNGN_PROFILE_CODING }, { NULL, 0 } };
static const enum_map DETAIL_MAP[] = {
  { "auto", ASNGN_DETAIL_AUTO }, { "terse", ASNGN_DETAIL_TERSE },
  { "normal", ASNGN_DETAIL_NORMAL }, { "rich", ASNGN_DETAIL_RICH },
  { NULL, 0 } };
static const enum_map SCOPE_MAP[] = {
  { "session", ASNGN_SCOPE_SESSION }, { "global", ASNGN_SCOPE_GLOBAL },
  { NULL, 0 } };
static const enum_map CONFIRM_MAP[] = {
  { "prompt", ASNGN_CONFIRM_PROMPT }, { "deny", ASNGN_CONFIRM_DENY },
  { "allow", ASNGN_CONFIRM_ALLOW }, { NULL, 0 } };
static const enum_map JUDGE_MAP[] = {
  { "off", ASNGN_JUDGE_OFF }, { "light", ASNGN_JUDGE_LIGHT },
  { "full", ASNGN_JUDGE_FULL }, { NULL, 0 } };
static const enum_map LEVEL_MAP[] = {
  { "error", ASNGN_LOG_ERROR }, { "warn", ASNGN_LOG_WARN },
  { "info", ASNGN_LOG_INFO }, { "debug", ASNGN_LOG_DEBUG }, { NULL, 0 } };
static const enum_map THEME_MAP[] = {
  { "asterism", ASNGN_THEME_ASTERISM }, { "plain", ASNGN_THEME_PLAIN },
  { NULL, 0 } };
static const enum_map TRUECOLOR_MAP[] = {
  { "auto", ASNGN_TRUECOLOR_AUTO }, { "on", ASNGN_TRUECOLOR_ON },
  { "off", ASNGN_TRUECOLOR_OFF }, { NULL, 0 } };
static const enum_map CATALOG_MAP[] = {
  { "index", ASNGN_CATALOG_INDEX }, { "summary", ASNGN_CATALOG_SUMMARY },
  { "full", ASNGN_CATALOG_FULL }, { NULL, 0 } };
static const enum_map SIDEBAR_MAP[] = {
  { "trace", 0 }, { "stats", 1 }, { "memory", 2 }, { "tools", 3 },
  { "cache", 4 }, { NULL, 0 } };

typedef enum {
  K_INT,   /* int >= 1                                   */
  K_I64,   /* int64 >= 0                                 */
  K_DUR,   /* duration -> int64 seconds, positive        */
  K_ODUR,  /* optional duration -> seconds, 0 = disabled */
  K_F01,   /* double in [0,1]                            */
  K_BOOL,
  K_STR,   /* strdup-replace char*                       */
  K_NSTR,  /* nullable char* (xCDN null clears)          */
  K_ENUM,  /* int via enum_map                           */
  K_NAME   /* fixed char[] buffer via enum_map names     */
} cfg_kind;

typedef struct {
  const char     *group;
  const char     *key;
  cfg_kind        kind;
  size_t          off;
  size_t          bufsz;   /* K_NAME only */
  const enum_map *emap;    /* K_ENUM / K_NAME */
} cfg_key;

#define OFF(f) offsetof(asngn_config, f)

static const cfg_key CFG_KEYS[] = {
  { "engine", "workers", K_INT, OFF(scheduler_workers), 0, NULL },
  { "engine", "queue_capacity", K_INT, OFF(queue_capacity), 0, NULL },
  { "engine", "root",        K_STR, OFF(root), 0, NULL },
  { "engine", "base_prompt", K_STR, OFF(base_prompt), 0, NULL },
  { "engine", "profile",     K_ENUM, OFF(profile), 0, PROFILE_MAP },

  { "models", "max_resident", K_INT, OFF(max_resident), 0, NULL },
  { "models", "max_ram_mb", K_INT, OFF(max_ram_mb), 0, NULL },
  { "models", "max_vram_mb", K_INT, OFF(max_vram_mb), 0, NULL },

  { "routing", "classifier",      K_ENUM, OFF(classifier), 0,
    CLASSIFIER_MAP },
  { "routing", "max_escalations", K_I64, 0, 0, NULL }, /* custom: int>=0 */

  { "detail", "default",       K_ENUM, OFF(detail_default), 0, DETAIL_MAP },
  { "detail", "terse_tokens",  K_INT, OFF(terse_tokens), 0, NULL },
  { "detail", "normal_tokens", K_INT, OFF(normal_tokens), 0, NULL },
  { "detail", "rich_tokens",   K_INT, OFF(rich_tokens), 0, NULL },

  { "context", "memory_checkpoint_tokens", K_INT,
    OFF(memory_checkpoint_tokens), 0, NULL },
  { "context", "memory_history_tokens", K_INT,
    OFF(memory_history_tokens), 0, NULL },
  { "context", "working_tokens",   K_INT, OFF(working_tokens), 0, NULL },
  { "context", "safety_margin",    K_INT, OFF(safety_margin), 0, NULL },
  { "context", "digest_threshold_chars", K_INT,
    OFF(digest_threshold_chars), 0, NULL },
  { "context", "digest_tokens",    K_INT, OFF(digest_tokens), 0, NULL },
  { "context", "pinned_max",       K_INT, OFF(pinned_max), 0, NULL },

  { "cache", "enable",           K_BOOL, OFF(cache_enable), 0, NULL },
  { "cache", "scope",            K_ENUM, OFF(cache_scope), 0, SCOPE_MAP },
  { "cache", "hit_threshold",    K_F01, OFF(hit_threshold), 0, NULL },
  { "cache", "adapt_threshold",  K_F01, OFF(adapt_threshold), 0, NULL },
  { "cache", "ttl",              K_DUR, OFF(cache_ttl_s), 0, NULL },
  { "cache", "max_entries",      K_INT, OFF(cache_max_entries), 0, NULL },
  { "cache", "tool_cache",       K_BOOL, OFF(tool_cache), 0, NULL },
  { "cache", "tool_ttl",         K_DUR, OFF(tool_ttl_s), 0, NULL },
  { "cache", "tool_max_entries", K_INT, OFF(tool_max_entries), 0, NULL },

  { "safety", "max_steps",      K_INT, OFF(max_steps), 0, NULL },
  { "safety", "max_tool_calls", K_INT, OFF(max_tool_calls), 0, NULL },
  { "safety", "think_limit",    K_INT, OFF(think_limit), 0, NULL },
  { "safety", "turn_deadline",  K_ODUR, OFF(turn_deadline_s), 0, NULL },
  { "safety", "stall_timeout",  K_ODUR, OFF(stall_timeout_s), 0, NULL },
  { "safety", "autoconfirm",    K_ENUM, OFF(autoconfirm), 0, CONFIRM_MAP },
  { "safety", "redact_context", K_BOOL, OFF(redact_context), 0, NULL },

  { "validation", "judge",     K_ENUM, OFF(judge), 0, JUDGE_MAP },
  { "validation", "threshold", K_INT, OFF(judge_threshold), 0, NULL },

  { "budgets", "session_tokens", K_I64, OFF(session_tokens), 0, NULL },
  { "budgets", "daily_tokens",   K_I64, OFF(daily_tokens), 0, NULL },
  { "budgets", "warn_at",        K_F01, OFF(warn_at), 0, NULL },

  { "telemetry", "ring",      K_INT, OFF(tele_ring), 0, NULL },
  { "telemetry", "path",      K_NSTR, OFF(tele_path), 0, NULL },
  { "telemetry", "rotate_kb", K_INT, OFF(tele_rotate_kb), 0, NULL },
  { "telemetry", "max_files", K_INT, OFF(tele_max_files), 0, NULL },

  { "logging", "path",        K_NSTR, OFF(log_path), 0, NULL },
  { "logging", "level",       K_ENUM, OFF(log_level), 0, LEVEL_MAP },
  { "logging", "max_size_kb", K_INT, OFF(log_max_size_kb), 0, NULL },
  { "logging", "max_files",   K_INT, OFF(log_max_files), 0, NULL },
  { "logging", "sync",        K_BOOL, OFF(log_sync), 0, NULL },

  { "tui", "theme",     K_ENUM, OFF(theme), 0, THEME_MAP },
  { "tui", "truecolor", K_ENUM, OFF(truecolor), 0, TRUECOLOR_MAP },
  { "tui", "fps_cap",   K_INT, OFF(fps_cap), 0, NULL },
  { "tui", "sidebar",   K_NAME, OFF(sidebar), sizeof ((asngn_config *)0)->sidebar,
    SIDEBAR_MAP },

  { "mcp", "autoconfirm", K_ENUM, OFF(mcp_autoconfirm), 0, CONFIRM_MAP },
};

#define CFG_KEYS_N (sizeof CFG_KEYS / sizeof CFG_KEYS[0])

/* ── overlay helpers ──────────────────────────────────────────────────── */

static asngn_err cfg_type_err(asngn_ctx *c, const char *group,
                              const char *key, const char *want,
                              const xcdn_value_t *got) {
  return asngn_seterr(c, ASNGN_ERR_CONFIG,
                      "config: %s.%s: expected %s, got %s", group, key, want,
                      got != NULL ? xcdn_value_type_str(got->type) : "null");
}

static asngn_err cfg_apply(asngn_ctx *c, asngn_config *cfg, const cfg_key *k,
                           const xcdn_value_t *v) {
  void *dst = (char *)cfg + k->off;
  switch (k->kind) {
  case K_INT: {
    if (v == NULL || v->type != XCDN_VAL_INT)
      return cfg_type_err(c, k->group, k->key, "int", v);
    if (v->data.integer < 1 || v->data.integer > 1000000000)
      return asngn_seterr(c, ASNGN_ERR_CONFIG,
                          "config: %s.%s: out of range", k->group, k->key);
    *(int *)dst = (int)v->data.integer;
    return ASNGN_OK;
  }
  case K_I64: {
    if (v == NULL || v->type != XCDN_VAL_INT)
      return cfg_type_err(c, k->group, k->key, "int", v);
    if (v->data.integer < 0)
      return asngn_seterr(c, ASNGN_ERR_CONFIG,
                          "config: %s.%s: must be >= 0", k->group, k->key);
    *(int64_t *)dst = v->data.integer;
    return ASNGN_OK;
  }
  case K_DUR: {
    const char *s = NULL;
    int64_t secs = 0;
    if (v != NULL &&
        (v->type == XCDN_VAL_DURATION || v->type == XCDN_VAL_STRING))
      s = v->data.string;
    if (s == NULL || !asngn_duration_parse(s, &secs) || secs <= 0)
      return asngn_seterr(c, ASNGN_ERR_CONFIG,
                          "config: %s.%s: expected a positive ISO 8601 "
                          "duration", k->group, k->key);
    *(int64_t *)dst = secs;
    return ASNGN_OK;
  }
  case K_ODUR: {
    const char *s = NULL;
    int64_t secs = 0;
    if (v != NULL &&
        (v->type == XCDN_VAL_DURATION || v->type == XCDN_VAL_STRING))
      s = v->data.string;
    if (s == NULL || !asngn_duration_parse(s, &secs) || secs < 0)
      return asngn_seterr(c, ASNGN_ERR_CONFIG,
                          "config: %s.%s: expected a non-negative ISO 8601 "
                          "duration", k->group, k->key);
    *(int64_t *)dst = secs;
    return ASNGN_OK;
  }
  case K_F01: {
    double d;
    if (v != NULL && v->type == XCDN_VAL_FLOAT) d = v->data.floating;
    else if (v != NULL && v->type == XCDN_VAL_INT)
      d = (double)v->data.integer;
    else return cfg_type_err(c, k->group, k->key, "number", v);
    if (d < 0.0 || d > 1.0)
      return asngn_seterr(c, ASNGN_ERR_CONFIG,
                          "config: %s.%s: must be in [0,1]", k->group,
                          k->key);
    *(double *)dst = d;
    return ASNGN_OK;
  }
  case K_BOOL: {
    if (v == NULL || v->type != XCDN_VAL_BOOL)
      return cfg_type_err(c, k->group, k->key, "bool", v);
    *(bool *)dst = v->data.boolean;
    return ASNGN_OK;
  }
  case K_STR: {
    char *dup;
    if (v == NULL || v->type != XCDN_VAL_STRING || v->data.string == NULL)
      return cfg_type_err(c, k->group, k->key, "string", v);
    dup = asngn_strdup(v->data.string);
    if (dup == NULL) return ASNGN_ERR_NOMEM;
    free(*(char **)dst);
    *(char **)dst = dup;
    return ASNGN_OK;
  }
  case K_NSTR: {
    char *dup;
    if (v != NULL && v->type == XCDN_VAL_NULL) {
      free(*(char **)dst);
      *(char **)dst = NULL;
      return ASNGN_OK;
    }
    if (v == NULL || v->type != XCDN_VAL_STRING || v->data.string == NULL)
      return cfg_type_err(c, k->group, k->key, "string or null", v);
    dup = asngn_strdup(v->data.string);
    if (dup == NULL) return ASNGN_ERR_NOMEM;
    free(*(char **)dst);
    *(char **)dst = dup;
    return ASNGN_OK;
  }
  case K_ENUM: {
    const enum_map *m;
    const char *s;
    if (v == NULL || v->type != XCDN_VAL_STRING || v->data.string == NULL)
      return cfg_type_err(c, k->group, k->key, "string", v);
    s = v->data.string;
    for (m = k->emap; m->name != NULL; m++) {
      if (strcmp(m->name, s) == 0) {
        *(int *)dst = m->value;
        return ASNGN_OK;
      }
    }
    return asngn_seterr(c, ASNGN_ERR_CONFIG,
                        "config: %s.%s: unknown value \"%s\"", k->group,
                        k->key, s);
  }
  case K_NAME: {
    const enum_map *m;
    const char *s;
    if (v == NULL || v->type != XCDN_VAL_STRING || v->data.string == NULL)
      return cfg_type_err(c, k->group, k->key, "string", v);
    s = v->data.string;
    for (m = k->emap; m->name != NULL; m++) {
      if (strcmp(m->name, s) == 0) {
        snprintf((char *)dst, k->bufsz, "%s", s);
        return ASNGN_OK;
      }
    }
    return asngn_seterr(c, ASNGN_ERR_CONFIG,
                        "config: %s.%s: unknown value \"%s\"", k->group,
                        k->key, s);
  }
  }
  return ASNGN_ERR_INVALID;
}

/* routing.max_escalations is the one int allowed to be 0. */
static asngn_err cfg_apply_escalations(asngn_ctx *c, asngn_config *cfg,
                                       const xcdn_value_t *v) {
  if (v == NULL || v->type != XCDN_VAL_INT)
    return cfg_type_err(c, "routing", "max_escalations", "int", v);
  if (v->data.integer < 0 || v->data.integer > 8)
    return asngn_seterr(c, ASNGN_ERR_CONFIG,
                        "config: routing.max_escalations: out of range");
  cfg->max_escalations = (int)v->data.integer;
  return ASNGN_OK;
}

/* models.pool: replaces the whole default pool. */
static asngn_err cfg_apply_pool(asngn_ctx *c, asngn_config *cfg,
                                const xcdn_value_t *v) {
  size_t i, n;
  asngn_pool_entry fresh[ASNGN_MAX_POOL];
  if (v == NULL || v->type != XCDN_VAL_ARRAY)
    return cfg_type_err(c, "models", "pool", "array", v);
  n = xcdn_array_len(v);
  if (n == 0 || n > ASNGN_MAX_POOL)
    return asngn_seterr(c, ASNGN_ERR_CONFIG,
                        "config: models.pool: 1..%d entries required",
                        ASNGN_MAX_POOL);
  memset(fresh, 0, sizeof fresh);
  for (i = 0; i < n; i++) {
    const xcdn_node_t *en = xcdn_array_get(v, i);
    const xcdn_value_t *eo = en != NULL ? en->value : NULL;
    const char *id, *path, *backend_s, *base_url, *remote_model, *provider_s;
    const char *api_key_env;
    int64_t ctx = 0, threads = 4, dim = 0, gpu_layers = -1;
    int64_t ram_mb = 0, vram_mb = 0;
    bool embedding = false, warm = true, kv_cache = true;
    asmodel_backend backend = ASMODEL_BACKEND_EMBEDDED;
    asmodel_remote_provider remote_provider = ASMODEL_REMOTE_AUTO;
    const xcdn_value_t *f;
    size_t j;
    if (eo == NULL || eo->type != XCDN_VAL_OBJECT) goto bad_entry;
    id = asngn_xstr(asngn_xfield(eo, "id"));
    path = asngn_xstr(asngn_xfield(eo, "path"));
    backend_s = asngn_xstr(asngn_xfield(eo, "backend"));
    base_url = asngn_xstr(asngn_xfield(eo, "base_url"));
    remote_model = asngn_xstr(asngn_xfield(eo, "model"));
    provider_s = asngn_xstr(asngn_xfield(eo, "provider"));
    api_key_env = asngn_xstr(asngn_xfield(eo, "api_key_env"));
    if (provider_s != NULL) {
      if (strcmp(provider_s, "auto") == 0)
        remote_provider = ASMODEL_REMOTE_AUTO;
      else if (strcmp(provider_s, "generic") == 0)
        remote_provider = ASMODEL_REMOTE_GENERIC;
      else if (strcmp(provider_s, "llama-server") == 0 ||
               strcmp(provider_s, "llama") == 0)
        remote_provider = ASMODEL_REMOTE_LLAMA_SERVER;
      else if (strcmp(provider_s, "lmstudio") == 0)
        remote_provider = ASMODEL_REMOTE_LMSTUDIO;
      else if (strcmp(provider_s, "vllm") == 0)
        remote_provider = ASMODEL_REMOTE_VLLM;
      else goto bad_entry;
    }
    if (backend_s != NULL) {
      if (strcmp(backend_s, "openai") == 0) backend = ASMODEL_BACKEND_OPENAI;
      else if (strcmp(backend_s, "embedded") != 0) goto bad_entry;
    }
    if (id == NULL || id[0] == '\0' || strlen(id) >= 32 ||
        (backend == ASMODEL_BACKEND_EMBEDDED &&
         (path == NULL || path[0] == '\0')) ||
        (backend == ASMODEL_BACKEND_OPENAI &&
         (base_url == NULL || !base_url[0] || remote_model == NULL ||
          !remote_model[0])))
      goto bad_entry;
    for (j = 0; j < i; j++)
      if (strcmp(fresh[j].id, id) == 0) goto bad_entry;
    f = asngn_xfield(eo, "ctx");
    if (f != NULL && (!asngn_xint(f, &ctx) || ctx < 256)) goto bad_entry;
    f = asngn_xfield(eo, "threads");
    if (f != NULL && (!asngn_xint(f, &threads) || threads < 1))
      goto bad_entry;
    f = asngn_xfield(eo, "embedding");
    if (f != NULL && !asngn_xbool(f, &embedding)) goto bad_entry;
    if (ctx == 0) ctx = embedding ? 512 : 32768;
    f = asngn_xfield(eo, "dim");
    if (f != NULL && (!asngn_xint(f, &dim) || dim < 1)) goto bad_entry;
    if (embedding && dim == 0) goto bad_entry;
    f = asngn_xfield(eo, "gpu_layers");
    if (f != NULL && (!asngn_xint(f, &gpu_layers) || gpu_layers < -1))
      goto bad_entry;
    f = asngn_xfield(eo, "ram_mb");
    if (f != NULL && (!asngn_xint(f, &ram_mb) || ram_mb < 0)) goto bad_entry;
    f = asngn_xfield(eo, "vram_mb");
    if (f != NULL && (!asngn_xint(f, &vram_mb) || vram_mb < 0)) goto bad_entry;
    f = asngn_xfield(eo, "warm");
    if (f != NULL && !asngn_xbool(f, &warm)) goto bad_entry;
    f = asngn_xfield(eo, "kv_cache");
    if (f != NULL && !asngn_xbool(f, &kv_cache)) goto bad_entry;
    cfg_pool_entry(&fresh[i], id, path != NULL ? path : "", (int)ctx,
                   (int)threads, embedding,
                   (int)dim, (int)gpu_layers);
    fresh[i].backend = backend;
    fresh[i].base_url = base_url ? cfg_dup(base_url) : NULL;
    fresh[i].remote_model = remote_model ? cfg_dup(remote_model) : NULL;
    fresh[i].api_key_env = api_key_env ? cfg_dup(api_key_env) : NULL;
    fresh[i].remote_provider = remote_provider;
    fresh[i].ram_mb = (size_t)ram_mb; fresh[i].vram_mb = (size_t)vram_mb;
    fresh[i].warm = warm; fresh[i].kv_cache = kv_cache;
    if (fresh[i].path == NULL || (base_url && !fresh[i].base_url) ||
        (remote_model && !fresh[i].remote_model) ||
        (api_key_env && !fresh[i].api_key_env)) {
      for (j = 0; j <= i; j++) {
        free(fresh[j].path); free(fresh[j].base_url);
        free(fresh[j].remote_model); free(fresh[j].api_key_env);
      }
      return ASNGN_ERR_NOMEM;
    }
  }
  for (i = 0; i < ASNGN_MAX_POOL; i++) {
    free(cfg->pool[i].path); free(cfg->pool[i].base_url);
    free(cfg->pool[i].remote_model); free(cfg->pool[i].api_key_env);
  }
  memcpy(cfg->pool, fresh, sizeof fresh);
  cfg->pool_n = n;
  return ASNGN_OK;
bad_entry:
  for (i = 0; i < ASNGN_MAX_POOL; i++) {
    free(fresh[i].path); free(fresh[i].base_url);
    free(fresh[i].remote_model); free(fresh[i].api_key_env);
  }
  return asngn_seterr(c, ASNGN_ERR_CONFIG,
                      "config: models.pool: invalid entry (need unique id, "
                       "embedded path or OpenAI base_url/model)");
}

/* models.roles: role name -> pool id. */
static asngn_err cfg_apply_roles(asngn_ctx *c, asngn_config *cfg,
                                 const xcdn_value_t *v) {
  static const struct { const char *name; size_t off; } ROLES[] = {
    { "router", OFF(role_router) },     { "planner", OFF(role_planner) },
    { "generator", OFF(role_generator) },
    { "compressor", OFF(role_compressor) },
    { "adapter", OFF(role_adapter) },   { "judge", OFF(role_judge) },
    { "embedder", OFF(role_embedder) },
  };
  size_t i, r;
  if (v == NULL || v->type != XCDN_VAL_OBJECT)
    return cfg_type_err(c, "models", "roles", "object", v);
  for (i = 0; i < xcdn_object_len(v); i++) {
    const char *key = xcdn_object_key_at(v, i);
    const xcdn_node_t *n = xcdn_object_node_at(v, i);
    const char *id = n != NULL ? asngn_xstr(n->value) : NULL;
    bool known = false;
    if (id == NULL || strlen(id) >= 32)
      return asngn_seterr(c, ASNGN_ERR_CONFIG,
                          "config: models.roles.%s: expected a pool id",
                          key != NULL ? key : "?");
    for (r = 0; r < sizeof ROLES / sizeof ROLES[0]; r++) {
      if (key != NULL && strcmp(ROLES[r].name, key) == 0) {
        snprintf((char *)cfg + ROLES[r].off, 32, "%s", id);
        known = true;
        break;
      }
    }
    if (!known)
      asngn_log(c, ASNGN_LOG_WARN, "config",
                "unknown key models.roles.%s", key != NULL ? key : "?");
  }
  return ASNGN_OK;
}

/* models.sampling: task -> { temp, top_p, max_tokens, repeat_penalty }
 * overrides. */
static asngn_err cfg_apply_sampling(asngn_ctx *c, asngn_config *cfg,
                                    const xcdn_value_t *v) {
  static const struct { const char *name; size_t off; } TASKS[] = {
    { "classify", OFF(s_classify) }, { "decide", OFF(s_decide) },
    { "draft", OFF(s_draft) },       { "answer", OFF(s_answer) },
    { "compress", OFF(s_compress) }, { "adapt", OFF(s_adapt) },
    { "judge", OFF(s_judge) },
  };
  size_t i, t;
  if (v == NULL || v->type != XCDN_VAL_OBJECT)
    return cfg_type_err(c, "models", "sampling", "object", v);
  for (i = 0; i < xcdn_object_len(v); i++) {
    const char *key = xcdn_object_key_at(v, i);
    const xcdn_node_t *n = xcdn_object_node_at(v, i);
    const xcdn_value_t *o = n != NULL ? n->value : NULL;
    bool known = false;
    for (t = 0; t < sizeof TASKS / sizeof TASKS[0]; t++) {
      if (key != NULL && strcmp(TASKS[t].name, key) == 0) {
        asngn_sampling *s = (asngn_sampling *)((char *)cfg + TASKS[t].off);
        const xcdn_value_t *f;
        double d;
        int64_t iv;
        if (o == NULL || o->type != XCDN_VAL_OBJECT)
          return asngn_seterr(c, ASNGN_ERR_CONFIG,
                              "config: models.sampling.%s: expected an "
                              "object", key);
        f = asngn_xfield(o, "temp");
        if (f != NULL) {
          if (!asngn_xnum(f, &d) || d < 0.0 || d > 2.0)
            return asngn_seterr(c, ASNGN_ERR_CONFIG,
                                "config: models.sampling.%s.temp: out of "
                                "range", key);
          s->temp = d;
        }
        f = asngn_xfield(o, "top_p");
        if (f != NULL) {
          if (!asngn_xnum(f, &d) || d < 0.0 || d > 1.0)
            return asngn_seterr(c, ASNGN_ERR_CONFIG,
                                "config: models.sampling.%s.top_p: out of "
                                "range", key);
          s->top_p = d;
        }
        f = asngn_xfield(o, "max_tokens");
        if (f != NULL) {
          if (!asngn_xint(f, &iv) || iv < 1 || iv > 1000000)
            return asngn_seterr(c, ASNGN_ERR_CONFIG,
                                "config: models.sampling.%s.max_tokens: "
                                "out of range", key);
          s->max_tokens = (int)iv;
        }
        f = asngn_xfield(o, "repeat_penalty");
        if (f != NULL) {
          /* 0 (or 1) = off; values below 1 would reward repetition */
          if (!asngn_xnum(f, &d) || d < 0.0 || d > 2.0 ||
              (d > 0.0 && d < 1.0))
            return asngn_seterr(c, ASNGN_ERR_CONFIG,
                                "config: models.sampling.%s."
                                "repeat_penalty: out of range", key);
          s->repeat_penalty = d;
        }
        known = true;
        break;
      }
    }
    if (!known)
      asngn_log(c, ASNGN_LOG_WARN, "config",
                "unknown key models.sampling.%s", key != NULL ? key : "?");
  }
  return ASNGN_OK;
}

/* integration.asper / integration.astools (legacy alias: "astls"). */
static asngn_err cfg_apply_integration(asngn_ctx *c, asngn_config *cfg,
                                       const char *sub,
                                       const xcdn_value_t *v) {
  size_t i;
  bool is_asper = strcmp(sub, "asper") == 0;
  bool is_astools = strcmp(sub, "astls") == 0 || strcmp(sub, "astools") == 0;
  if (!is_asper && !is_astools) {
    asngn_log(c, ASNGN_LOG_WARN, "config", "unknown key integration.%s",
              sub);
    return ASNGN_OK;
  }
  if (strcmp(sub, "astls") == 0)
    asngn_log(c, ASNGN_LOG_WARN, "config",
              "integration.astls is deprecated; use integration.astools");
  if (v == NULL || v->type != XCDN_VAL_OBJECT)
    return cfg_type_err(c, "integration", sub, "object", v);
  for (i = 0; i < xcdn_object_len(v); i++) {
    const char *key = xcdn_object_key_at(v, i);
    const xcdn_node_t *n = xcdn_object_node_at(v, i);
    const xcdn_value_t *f = n != NULL ? n->value : NULL;
    cfg_key k;
    memset(&k, 0, sizeof k);
    k.group = "integration";
    k.key = key != NULL ? key : "?";
    if (key == NULL) continue;
    if (strcmp(key, "enable") == 0) {
      k.kind = K_BOOL;
      k.off = is_asper ? OFF(asper_enable) : OFF(astools_enable);
    } else if (strcmp(key, "root") == 0) {
      k.kind = K_STR;
      k.off = is_asper ? OFF(asper_root) : OFF(astools_root);
    } else if (strcmp(key, "config") == 0) {
      k.kind = K_NSTR;
      k.off = is_asper ? OFF(asper_config) : OFF(astools_config);
    } else if (is_astools && strcmp(key, "workspace") == 0) {
      k.kind = K_STR;
      k.off = OFF(astools_workspace);
    } else if (is_astools && strcmp(key, "catalog_level") == 0) {
      k.kind = K_ENUM;
      k.off = OFF(catalog_level);
      k.emap = CATALOG_MAP;
    } else if (is_astools && strcmp(key, "catalog_chars") == 0) {
      k.kind = K_INT;
      k.off = OFF(catalog_chars);
    } else {
      asngn_log(c, ASNGN_LOG_WARN, "config",
                "unknown key integration.%s.%s", sub, key);
      continue;
    }
    {
      asngn_err e = cfg_apply(c, cfg, &k, f);
      if (e != ASNGN_OK) return e;
    }
  }
  return ASNGN_OK;
}

/* ── load ─────────────────────────────────────────────────────────────── */

asngn_err asngn_config_load(asngn_ctx *c, asngn_config *cfg,
                            const char *path) {
  char *text = NULL;
  size_t len = 0;
  xcdn_document_t *doc = NULL;
  xcdn_error_t xe;
  const xcdn_value_t *root = NULL;
  size_t i;
  asngn_err e;

  if (cfg->root == NULL || cfg->base_prompt == NULL ||
      cfg->asper_root == NULL || cfg->astools_root == NULL ||
      cfg->astools_workspace == NULL)
    return ASNGN_ERR_NOMEM; /* defaults ran out of memory */
  if (path == NULL) return ASNGN_OK;

  e = os_read_file(path, &text, &len);
  if (e != ASNGN_OK)
    return asngn_seterr(c, e, "config: cannot read %s", path);
  doc = xcdn_parse_str(text, len, &xe);
  free(text);
  if (doc == NULL)
    return asngn_seterr(c, ASNGN_ERR_CONFIG, "config: %s: %s (line %zu)",
                        path, xe.message, xe.span.line);

  for (i = 0; i < doc->values_len; i++) {
    const xcdn_node_t *n = doc->values[i];
    if (n != NULL && n->value != NULL &&
        n->value->type == XCDN_VAL_OBJECT) {
      if (n->tags_len > 0 && !xcdn_node_has_tag(n, "asngn_config"))
        asngn_log(c, ASNGN_LOG_WARN, "config",
                  "%s: root tagged #%s, expected #asngn_config", path,
                  n->tags[0].name != NULL ? n->tags[0].name : "?");
      root = n->value;
      break;
    }
  }
  if (root == NULL) {
    xcdn_document_free(doc);
    return ASNGN_OK; /* empty document: defaults stand */
  }

  for (i = 0; i < xcdn_object_len(root); i++) {
    const char *group = xcdn_object_key_at(root, i);
    const xcdn_node_t *gn = xcdn_object_node_at(root, i);
    const xcdn_value_t *gv = gn != NULL ? gn->value : NULL;
    size_t j;
    bool group_known = false;
    if (group == NULL) continue;
    if (gv == NULL || gv->type != XCDN_VAL_OBJECT) {
      e = asngn_seterr(c, ASNGN_ERR_CONFIG,
                       "config: %s: expected an object", group);
      goto fail;
    }
    if (strcmp(group, "integration") == 0) {
      for (j = 0; j < xcdn_object_len(gv); j++) {
        const char *sub = xcdn_object_key_at(gv, j);
        const xcdn_node_t *sn = xcdn_object_node_at(gv, j);
        e = cfg_apply_integration(c, cfg, sub != NULL ? sub : "?",
                                  sn != NULL ? sn->value : NULL);
        if (e != ASNGN_OK) goto fail;
      }
      continue;
    }
    for (j = 0; j < xcdn_object_len(gv); j++) {
      const char *key = xcdn_object_key_at(gv, j);
      const xcdn_node_t *kn = xcdn_object_node_at(gv, j);
      const xcdn_value_t *kv = kn != NULL ? kn->value : NULL;
      size_t t;
      bool found = false;
      if (key == NULL) continue;
      if (strcmp(group, "models") == 0) {
        if (strcmp(key, "pool") == 0) {
          e = cfg_apply_pool(c, cfg, kv);
          if (e != ASNGN_OK) goto fail;
          continue;
        }
        if (strcmp(key, "roles") == 0) {
          e = cfg_apply_roles(c, cfg, kv);
          if (e != ASNGN_OK) goto fail;
          continue;
        }
        if (strcmp(key, "sampling") == 0) {
          e = cfg_apply_sampling(c, cfg, kv);
          if (e != ASNGN_OK) goto fail;
          continue;
        }
      }
      if (strcmp(group, "routing") == 0 &&
          strcmp(key, "max_escalations") == 0) {
        e = cfg_apply_escalations(c, cfg, kv);
        if (e != ASNGN_OK) goto fail;
        continue;
      }
      for (t = 0; t < CFG_KEYS_N; t++) {
        if (strcmp(CFG_KEYS[t].group, group) == 0 &&
            strcmp(CFG_KEYS[t].key, key) == 0) {
          e = cfg_apply(c, cfg, &CFG_KEYS[t], kv);
          if (e != ASNGN_OK) goto fail;
          found = true;
          break;
        }
      }
      if (!found)
        asngn_log(c, ASNGN_LOG_WARN, "config", "unknown key %s.%s", group,
                  key);
    }
    group_known = true;
    (void)group_known;
  }

  /* cross-checks */
  if (cfg->adapt_threshold > cfg->hit_threshold) {
    e = asngn_seterr(c, ASNGN_ERR_CONFIG,
                     "config: cache.adapt_threshold must not exceed "
                     "cache.hit_threshold");
    goto fail;
  }
  xcdn_document_free(doc);
  return ASNGN_OK;

fail:
  xcdn_document_free(doc);
  return e;
}
