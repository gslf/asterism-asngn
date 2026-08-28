/*
 * models.c — model pool and role runtime.
 *
 * The pool copies cfg.pool into per-slot state at init; models load
 * lazily on first use and at most cfg.max_resident non-injected
 * instances stay resident (LRU-unloaded, never mid-call). Injected
 * slots (test fakes wired by asngn_open_with) are permanently loaded
 * and never destroyed before shutdown.
 *
 * Locking: c->models_mu guards slot load/unload state, role
 * resolution results and the in-use markers; each slot's own mutex is
 * held while a backend call runs on that instance. models_mu is never
 * held across a backend call.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- per-context auxiliary state ---------------------------------------- */
/* The public slot struct (asngn_internal.h) carries no in-use marker and
 * the os shim has no trylock, so "is a call running on this instance" is
 * tracked here: a models.c-scope table keyed by context, with the in_use
 * flags guarded by that context's models_mu. The table also caches the
 * embedder weights hash (computed once at init) and the one-time
 * heuristic-fallback warning latch. Registration happens in
 * asngn_models_init and release in asngn_models_shutdown; like
 * asngn_open/asngn_close themselves, concurrent open/close of *different*
 * contexts is not synchronized here. */

#define MODELS_AUX_MAX 8

typedef struct {
  asngn_ctx *ctx;                 /* NULL = free entry                  */
  int        in_use[ASNGN_MAX_POOL]; /* guarded by ctx->models_mu       */
  uint8_t    embed_hash[32];      /* sha256 of embedder weights (or 0)  */
  int        heuristic_warned;    /* one-time fallback warning          */
} models_aux;

static models_aux g_aux[MODELS_AUX_MAX];

static models_aux *aux_find(asngn_ctx *c) {
  int i;
  for (i = 0; i < MODELS_AUX_MAX; i++)
    if (g_aux[i].ctx == c) return &g_aux[i];
  return NULL;
}

static models_aux *aux_claim(asngn_ctx *c) {
  models_aux *a = aux_find(c);
  if (a == NULL) a = aux_find(NULL);
  if (a == NULL) return NULL;
  memset(a, 0, sizeof *a);
  a->ctx = c;
  return a;
}

static void aux_release(asngn_ctx *c) {
  models_aux *a = aux_find(c);
  if (a != NULL) memset(a, 0, sizeof *a);
}

/* ---- names -------------------------------------------------------------- */

const char *asngn_role_name(asngn_role r) {
  switch (r) {
    case ASNGN_ROLE_ROUTER:     return "router";
    case ASNGN_ROLE_PLANNER:    return "planner";
    case ASNGN_ROLE_GENERATOR:  return "generator";
    case ASNGN_ROLE_COMPRESSOR: return "compressor";
    case ASNGN_ROLE_ADAPTER:    return "adapter";
    case ASNGN_ROLE_JUDGE:      return "judge";
    case ASNGN_ROLE_EMBEDDER:   return "embedder";
    default:                    return "?";
  }
}

const char *asngn_task_name(asngn_task_kind t) {
  switch (t) {
    case ASNGN_TASK_CLASSIFY: return "classify";
    case ASNGN_TASK_DECIDE:   return "decide";
    case ASNGN_TASK_DRAFT:    return "draft";
    case ASNGN_TASK_ANSWER:   return "answer";
    case ASNGN_TASK_COMPRESS: return "compress";
    case ASNGN_TASK_ADAPT:    return "adapt";
    case ASNGN_TASK_JUDGE:    return "judge";
    default:                  return "?";
  }
}

/* Default sampling for one task kind. */
static const asngn_sampling *task_sampling(const asngn_config *cfg,
                                           asngn_task_kind t) {
  switch (t) {
    case ASNGN_TASK_CLASSIFY: return &cfg->s_classify;
    case ASNGN_TASK_DECIDE:   return &cfg->s_decide;
    case ASNGN_TASK_DRAFT:    return &cfg->s_draft;
    case ASNGN_TASK_ANSWER:   return &cfg->s_answer;
    case ASNGN_TASK_COMPRESS: return &cfg->s_compress;
    case ASNGN_TASK_ADAPT:    return &cfg->s_adapt;
    default:                  return &cfg->s_judge;
  }
}

/* ---- lifecycle ---------------------------------------------------------- */

asngn_err asngn_models_init(asngn_ctx *c) {
  models_aux *aux;
  size_t i;
  int r;

  if (c == NULL) return ASNGN_ERR_INVALID;
  c->models_n = c->cfg.pool_n;

  for (i = 0; i < c->models_n; i++) {
    asngn_model_slot *s = &c->models[i];
    const asngn_pool_entry *src = &c->cfg.pool[i];
    /* asngn_open_with may have wired a fake into this slot before init
     * runs: preserve iface for injected slots (permanently loaded). */
    bool injected = s->injected;
    asngn_model_iface saved = s->iface;

    memset(s, 0, sizeof *s);
    s->injected = injected;
    if (injected) s->iface = saved;

    s->cfg = *src;
    s->cfg.path = asngn_strdup(src->path); /* slot owns its copy */
    s->cfg.base_url = src->base_url ? asngn_strdup(src->base_url) : NULL;
    s->cfg.remote_model = src->remote_model ? asngn_strdup(src->remote_model) : NULL;
    s->cfg.api_key_env = src->api_key_env ? asngn_strdup(src->api_key_env) : NULL;
    s->cfg.api_grammar = src->api_grammar ? asngn_strdup(src->api_grammar) : NULL;
    s->cfg.reasoning_effort = src->reasoning_effort
                                  ? asngn_strdup(src->reasoning_effort) : NULL;
    if ((src->path != NULL && s->cfg.path == NULL) ||
        (src->base_url && !s->cfg.base_url) ||
        (src->remote_model && !s->cfg.remote_model) ||
        (src->api_key_env && !s->cfg.api_key_env) ||
        (src->api_grammar && !s->cfg.api_grammar) ||
        (src->reasoning_effort && !s->cfg.reasoning_effort)) {
      size_t j;
      free(s->cfg.path); free(s->cfg.base_url); free(s->cfg.remote_model);
      free(s->cfg.api_key_env); free(s->cfg.api_grammar);
      free(s->cfg.reasoning_effort);
      for (j = 0; j < i; j++) {
        free(c->models[j].cfg.path);
        free(c->models[j].cfg.base_url);
        free(c->models[j].cfg.remote_model);
        free(c->models[j].cfg.api_key_env);
        free(c->models[j].cfg.api_grammar);
        free(c->models[j].cfg.reasoning_effort);
        c->models[j].cfg.path = NULL;
        os_mutex_destroy(&c->models[j].mu);
      }
      c->models_n = 0;
      return asngn_seterr(c, ASNGN_ERR_NOMEM, "out of memory");
    }
    os_mutex_init(&s->mu);
    if (s->cfg.backend == ASMODEL_BACKEND_EMBEDDED &&
        s->cfg.ram_mb == 0 && s->cfg.path && s->cfg.path[0]) {
      uint64_t bytes = 0;
      if (os_file_size(s->cfg.path, &bytes) == ASNGN_OK)
        s->cfg.ram_mb = (size_t)((bytes + 1024 * 1024 - 1) /
                                 (1024 * 1024));
    }
  }

  /* Resolve roles onto pool slots by id. */
  {
    const char *names[ASNGN_ROLE_COUNT];
    names[ASNGN_ROLE_ROUTER]     = c->cfg.role_router;
    names[ASNGN_ROLE_PLANNER]    = c->cfg.role_planner;
    names[ASNGN_ROLE_GENERATOR]  = c->cfg.role_generator;
    names[ASNGN_ROLE_COMPRESSOR] = c->cfg.role_compressor;
    names[ASNGN_ROLE_ADAPTER]    = c->cfg.role_adapter;
    names[ASNGN_ROLE_JUDGE]      = c->cfg.role_judge;
    names[ASNGN_ROLE_EMBEDDER]   = c->cfg.role_embedder;
    for (r = 0; r < (int)ASNGN_ROLE_COUNT; r++) {
      c->role_slot[r] = -1;
      if (names[r][0] == '\0') continue;
      for (i = 0; i < c->models_n; i++) {
        if (strcmp(c->models[i].cfg.id, names[r]) == 0) {
          c->role_slot[r] = (int)i;
          break;
        }
      }
      if (c->role_slot[r] < 0)
        asngn_log(c, ASNGN_LOG_WARN, "model",
                  "role %s: pool id '%s' not found; role unavailable",
                  asngn_role_name((asngn_role)r), names[r]);
    }
  }

  aux = aux_claim(c);
  if (aux == NULL) {
    asngn_log(c, ASNGN_LOG_WARN, "model",
              "model aux table exhausted; LRU unloading disabled for this "
              "context");
    return ASNGN_OK;
  }

  /* Embedder weights hash, computed once. Injected fakes hash to
   * 32 bytes of 0x11 (the test-fake convention); a missing or unreadable
   * file leaves zeroes. */
  {
    int es = c->role_slot[ASNGN_ROLE_EMBEDDER];
    if (es >= 0) {
      asngn_model_slot *s = &c->models[es];
      if (s->injected) {
        memset(aux->embed_hash, 0x11, sizeof aux->embed_hash);
      } else if (s->cfg.backend == ASMODEL_BACKEND_OPENAI) {
        asngn_sha256_ctx sh;
        asngn_sha256_init(&sh);
        if (s->cfg.base_url)
          asngn_sha256_update(&sh, s->cfg.base_url, strlen(s->cfg.base_url));
        if (s->cfg.remote_model)
          asngn_sha256_update(&sh, s->cfg.remote_model,
                              strlen(s->cfg.remote_model));
        asngn_sha256_final(&sh, aux->embed_hash);
      } else if (s->cfg.path != NULL && s->cfg.path[0] != '\0') {
        if (asngn_sha256_file(s->cfg.path, aux->embed_hash) != ASNGN_OK) {
          memset(aux->embed_hash, 0, sizeof aux->embed_hash);
          asngn_log(c, ASNGN_LOG_WARN, "model",
                    "cannot hash embedder weights '%s'", s->cfg.path);
        }
      }
    }
  }
  return ASNGN_OK;
}

void asngn_models_shutdown(asngn_ctx *c) {
  size_t i;
  if (c == NULL) return;
  for (i = 0; i < c->models_n; i++) {
    asngn_model_slot *s = &c->models[i];
    if ((s->loaded || s->injected) && s->iface.destroy != NULL)
      s->iface.destroy(s->iface.ud); /* fakes carry no-op destroys */
    memset(&s->iface, 0, sizeof s->iface);
    s->loaded = false;
    s->injected = false;
    os_mutex_destroy(&s->mu);
    free(s->cfg.path);
    free(s->cfg.base_url);
    free(s->cfg.remote_model);
    free(s->cfg.api_key_env);
    free(s->cfg.api_grammar);
    free(s->cfg.reasoning_effort);
    s->cfg.path = NULL;
  }
  c->models_n = 0;
  aux_release(c);
}

/* ---- lookups ------------------------------------------------------------ */

int asngn_models_slot_for_role(asngn_ctx *c, asngn_role role) {
  if (c == NULL || (int)role < 0 || (int)role >= (int)ASNGN_ROLE_COUNT)
    return -1;
  return c->role_slot[role];
}

static asngn_err ensure_loaded_locked(asngn_ctx *c, int slot);

void asngn_models_warm(asngn_ctx *c) {
  /* Turn order: the router classifies first, the generator answers,
   * the embedder probes the cache — warming in the same order means a
   * turn racing the warm-up waits on the model it needs next, never on
   * one it does not. Failures stay silent here; first use reports. */
  static const asngn_role ORDER[] = {
    ASNGN_ROLE_ROUTER,     ASNGN_ROLE_GENERATOR, ASNGN_ROLE_EMBEDDER,
    ASNGN_ROLE_PLANNER,    ASNGN_ROLE_COMPRESSOR, ASNGN_ROLE_ADAPTER,
    ASNGN_ROLE_JUDGE,
  };
  int warmed[ASNGN_ROLE_COUNT];
  int warmed_n = 0;
  size_t i;
  int j;
  if (c == NULL) return;
  for (i = 0; i < sizeof ORDER / sizeof ORDER[0]; i++) {
    int slot = asngn_models_slot_for_role(c, ORDER[i]);
    bool seen = false;
    if (slot < 0) continue;
    if (!c->models[slot].cfg.warm) continue;
    for (j = 0; j < warmed_n; j++)
      if (warmed[j] == slot) seen = true;
    if (seen) continue;
    /* never warm past the residency cap: the LRU would evict what was
     * just loaded — churn instead of a warm-up */
    if (c->cfg.max_resident > 0 && warmed_n >= c->cfg.max_resident)
      break;
    if (c->bg_cancel) return; /* shutting down mid-warm */
    os_mutex_lock(&c->models_mu);
    if (ensure_loaded_locked(c, slot) == ASNGN_OK)
      warmed[warmed_n++] = slot;
    os_mutex_unlock(&c->models_mu);
  }
  /* the load phases set the live "now" marker; nothing follows them */
  asngn_tele_emit(c, "phase", NULL, NULL, NULL, 0, "{what: \"idle\"}");
}

int asngn_models_slot_for_id(asngn_ctx *c, const char *id) {
  size_t i;
  if (c == NULL || id == NULL) return -1;
  for (i = 0; i < c->models_n; i++)
    if (strcmp(c->models[i].cfg.id, id) == 0) return (int)i;
  return -1;
}

/* ---- lazy load and residency -------------------------------------------- */

/* Unload loaded non-injected slots (LRU by last_used_ms) until at most
 * cfg.max_resident remain. Never unloads `keep`, an in-use slot, or an
 * injected slot; without aux in-use tracking unloading is skipped
 * entirely (never destroy an instance a call might be running on).
 * c->models_mu held. */
static void enforce_resident_locked(asngn_ctx *c, int keep) {
  models_aux *aux = aux_find(c);
  int max = c->cfg.max_resident;

  if (aux == NULL) return;
  for (;;) {
    int loaded_n = 0, victim = -1;
    size_t ram_mb = 0, vram_mb = 0;
    int64_t oldest = 0;
    size_t i;
    for (i = 0; i < c->models_n; i++) {
      asngn_model_slot *s = &c->models[i];
      if (!s->loaded || s->injected) continue;
      loaded_n++;
      ram_mb += s->cfg.ram_mb;
      vram_mb += s->cfg.vram_mb;
      if ((int)i == keep || aux->in_use[i]) continue;
      if (victim < 0 || s->last_used_ms < oldest) {
        victim = (int)i;
        oldest = s->last_used_ms;
      }
    }
    if ((max <= 0 || loaded_n <= max) &&
        (c->cfg.max_ram_mb <= 0 || ram_mb <= (size_t)c->cfg.max_ram_mb) &&
        (c->cfg.max_vram_mb <= 0 ||
         vram_mb <= (size_t)c->cfg.max_vram_mb)) return;
    if (victim < 0) return;
    {
      asngn_model_slot *v = &c->models[victim];
      if (v->iface.destroy != NULL) v->iface.destroy(v->iface.ud);
      memset(&v->iface, 0, sizeof v->iface);
      v->loaded = false;
      asngn_log(c, ASNGN_LOG_INFO, "model",
                "unloaded '%s' (LRU resource budget)", v->cfg.id);
    }
  }
}

/* Evict idle residents before a cold load so hard budgets are not exceeded
 * even transiently. c->models_mu held. */
static asngn_err prepare_resident_locked(asngn_ctx *c, int incoming) {
  models_aux *aux = aux_find(c);
  asngn_model_slot *want = &c->models[incoming];
  if (aux == NULL) return ASNGN_OK;
  for (;;) {
    size_t ram_mb = want->cfg.ram_mb, vram_mb = want->cfg.vram_mb;
    int resident = 1, victim = -1;
    int64_t oldest = 0;
    size_t i;
    for (i = 0; i < c->models_n; ++i) {
      asngn_model_slot *s = &c->models[i];
      if ((int)i == incoming || !s->loaded || s->injected) continue;
      resident++;
      ram_mb += s->cfg.ram_mb;
      vram_mb += s->cfg.vram_mb;
      if (!aux->in_use[i] &&
          (victim < 0 || s->last_used_ms < oldest)) {
        victim = (int)i;
        oldest = s->last_used_ms;
      }
    }
    if ((c->cfg.max_resident <= 0 || resident <= c->cfg.max_resident) &&
        (c->cfg.max_ram_mb <= 0 ||
         ram_mb <= (size_t)c->cfg.max_ram_mb) &&
        (c->cfg.max_vram_mb <= 0 ||
         vram_mb <= (size_t)c->cfg.max_vram_mb))
      return ASNGN_OK;
    if (victim < 0)
      return asngn_seterr(c, ASNGN_ERR_MODEL,
                          "model '%s' exceeds resident/RAM/VRAM limits",
                          want->cfg.id);
    {
      asngn_model_slot *v = &c->models[victim];
      if (v->iface.destroy) v->iface.destroy(v->iface.ud);
      memset(&v->iface, 0, sizeof v->iface);
      v->loaded = false;
      asngn_log(c, ASNGN_LOG_INFO, "model",
                "unloaded '%s' (LRU resource budget)", v->cfg.id);
    }
  }
}

/* c->models_mu held. On success the slot is loaded (or injected). */
static asngn_err ensure_loaded_locked(asngn_ctx *c, int slot) {
  asngn_model_slot *s = &c->models[slot];
  asngn_err e;

  if (s->loaded || s->injected) return ASNGN_OK;
  e = prepare_resident_locked(c, slot);
  if (e != ASNGN_OK) return e;
  {
    /* phase marker: loads take seconds to tens of seconds on cold
     * cache; without it the trace is silent exactly when the user
     * wonders what the engine is doing */
    char data[96];
    snprintf(data, sizeof data, "{what: \"load\", model: \"%s\"}",
             s->cfg.id);
    asngn_tele_emit(c, "phase", NULL, NULL, NULL, 0, data);
  }
  e = s->cfg.backend == ASMODEL_BACKEND_OPENAI
          ? asngn_model_openai_create(c, &s->cfg, &s->iface)
          : asngn_model_llama_create(c, &s->cfg, &s->iface);
  if (e != ASNGN_OK) return e;
  s->loaded = true;
  s->last_used_ms = asngn_clock_mono_ms(&c->clock);
  enforce_resident_locked(c, slot);
  return ASNGN_OK;
}

/* Mark / clear the in-use flag for one slot (guards LRU unloading). */
static void slot_set_in_use(asngn_ctx *c, int slot, int on) {
  models_aux *aux = aux_find(c);
  if (aux != NULL) aux->in_use[slot] = on;
}

/* ---- generation --------------------------------------------------------- */

asngn_err asngn_models_generate(asngn_ctx *c, int slot, asngn_task_kind task,
                                const char *system_prompt,
                                const char *user_prompt, const char *gbnf,
                                int max_tokens_override,
                                asngn_token_fn token_cb, void *token_ud,
                                volatile int *cancel,
                                char **out_text, int *out_tokens_in,
                                int *out_tokens_out) {
  asngn_model_slot *s;
  const asngn_sampling *sp;
  asngn_gen_params p;
  char *text = NULL;
  int ti = 0, to = 0;
  int64_t t0, t1, ms;
  asngn_err e;

  if (out_text != NULL) *out_text = NULL;
  if (out_tokens_in != NULL) *out_tokens_in = 0;
  if (out_tokens_out != NULL) *out_tokens_out = 0;
  if (c == NULL) return ASNGN_ERR_INVALID;
  if (slot < 0 || (size_t)slot >= c->models_n)
    return asngn_seterr(c, ASNGN_ERR_MODEL, "role unavailable");
  s = &c->models[slot];

  sp = task_sampling(&c->cfg, task);
  p.temp = sp->temp;
  p.top_p = sp->top_p;
  p.max_tokens = max_tokens_override > 0 ? max_tokens_override
                                         : sp->max_tokens;
  p.repeat_penalty = sp->repeat_penalty;

  /* Final gate for every call, including classifier, judge, compression
   * and cache adaptation calls that do not use the zoned assembler. */
  e = asngn_context_validate_text(
      c, slot, system_prompt, user_prompt,
      p.max_tokens > 0 ? p.max_tokens : c->cfg.rich_tokens);
  if (e != ASNGN_OK) return e;

  os_mutex_lock(&c->models_mu);
  e = ensure_loaded_locked(c, slot);
  if (e == ASNGN_OK && s->iface.generate == NULL)
    e = asngn_seterr(c, ASNGN_ERR_MODEL, "model '%s' cannot generate",
                     s->cfg.id);
  if (e != ASNGN_OK) {
    os_mutex_unlock(&c->models_mu);
    return e;
  }
  slot_set_in_use(c, slot, 1);
  os_mutex_unlock(&c->models_mu);

  {
    /* phase marker: the completion event below lands only when the
     * call is done; this one tells the TUI what runs right now */
    char data[128];
    snprintf(data, sizeof data,
             "{what: \"generate\", task: \"%s\", model: \"%s\"}",
             asngn_task_name(task), s->cfg.id);
    asngn_tele_emit(c, "phase", NULL, NULL, NULL, 0, data);
  }

  t0 = asngn_clock_mono_ms(&c->clock);
  os_mutex_lock(&s->mu);
  e = s->iface.generate(s->iface.ud, system_prompt, user_prompt, gbnf, &p,
                        token_cb, token_ud, cancel, &text, &ti, &to);
  os_mutex_unlock(&s->mu);
  t1 = asngn_clock_mono_ms(&c->clock);

  os_mutex_lock(&c->models_mu);
  slot_set_in_use(c, slot, 0);
  s->last_used_ms = t1;
  os_mutex_unlock(&c->models_mu);

  if (e != ASNGN_OK) {
    char backend_error[512] = {0};
    if (s->iface.last_error != NULL) {
      const char *detail = s->iface.last_error(s->iface.ud);
      if (detail != NULL && detail[0] != '\0')
        snprintf(backend_error, sizeof backend_error, "%s", detail);
    }
    free(text);
    if (e == ASNGN_ERR_CANCELLED) return e;
    if (backend_error[0] != '\0')
      return asngn_seterr(c, ASNGN_ERR_MODEL, "model '%s' %s failed: %s",
                          s->cfg.id, asngn_task_name(task), backend_error);
    return asngn_seterr(c, ASNGN_ERR_MODEL, "model '%s' %s failed (%s)",
                        s->cfg.id, asngn_task_name(task), asngn_err_name(e));
  }

  ms = t1 - t0;
  {
    char data[256];
    double tps = ms > 0 ? (double)to / ((double)ms / 1000.0) : 0.0;
    snprintf(data, sizeof data,
             "{model: \"%s\", task: \"%s\", tokens_in: %d, tokens_out: %d, "
             "ms: %lld, tps: %.1f}",
             s->cfg.id, asngn_task_name(task), ti, to, (long long)ms, tps);
    asngn_tele_emit(c, "model_call", NULL, NULL, NULL, 0, data);
  }

  if (out_tokens_in != NULL) *out_tokens_in = ti;
  if (out_tokens_out != NULL) *out_tokens_out = to;
  if (out_text != NULL) *out_text = text;
  else free(text);
  return ASNGN_OK;
}

/* ---- token counting ----------------------------------------------------- */

/* degraded fallback: byte/4 heuristic, WARN once per context. */
static int count_heuristic(asngn_ctx *c, const char *text) {
  models_aux *aux;
  int warn = 0;

  os_mutex_lock(&c->models_mu);
  aux = aux_find(c);
  if (aux != NULL && !aux->heuristic_warned) {
    aux->heuristic_warned = 1;
    warn = 1;
  }
  os_mutex_unlock(&c->models_mu);
  if (warn)
    asngn_log(c, ASNGN_LOG_WARN, "model",
              "no tokenizer available; falling back to the byte/4 "
              "heuristic ");
  return asngn_token_heuristic(text);
}

int asngn_models_count_tokens(asngn_ctx *c, int slot, const char *text) {
  asngn_model_slot *s;
  asngn_err e;
  int n;

  if (c == NULL) return asngn_token_heuristic(text);
  if (slot < 0 || (size_t)slot >= c->models_n)
    return count_heuristic(c, text);
  s = &c->models[slot];

  /* Exact accounting: counting is worth the load — a not-yet-loaded
   * slot is loaded here rather than estimated around. */
  os_mutex_lock(&c->models_mu);
  e = ensure_loaded_locked(c, slot);
  if (e != ASNGN_OK || s->iface.count_tokens == NULL) {
    os_mutex_unlock(&c->models_mu);
    return count_heuristic(c, text);
  }
  slot_set_in_use(c, slot, 1);
  os_mutex_unlock(&c->models_mu);

  os_mutex_lock(&s->mu);
  n = s->iface.count_tokens(s->iface.ud, text);
  os_mutex_unlock(&s->mu);

  os_mutex_lock(&c->models_mu);
  slot_set_in_use(c, slot, 0);
  s->last_used_ms = asngn_clock_mono_ms(&c->clock);
  os_mutex_unlock(&c->models_mu);

  if (n < 0) return count_heuristic(c, text);
  return n;
}

int asngn_models_count_prompt(asngn_ctx *c, int slot,
                              const char *system_prompt,
                              const char *user_prompt) {
  asngn_model_slot *s;
  asngn_err e;
  int n;
  if (c == NULL || slot < 0 || (size_t)slot >= c->models_n)
    return asngn_token_heuristic(system_prompt) +
           asngn_token_heuristic(user_prompt) + 16;
  s = &c->models[slot];
  os_mutex_lock(&c->models_mu);
  e = ensure_loaded_locked(c, slot);
  if (e != ASNGN_OK) {
    os_mutex_unlock(&c->models_mu);
    return asngn_token_heuristic(system_prompt) +
           asngn_token_heuristic(user_prompt) + 16;
  }
  slot_set_in_use(c, slot, 1);
  os_mutex_unlock(&c->models_mu);
  os_mutex_lock(&s->mu);
  if (s->iface.count_prompt_tokens != NULL)
    n = s->iface.count_prompt_tokens(s->iface.ud, system_prompt, user_prompt);
  else
    n = (s->iface.count_tokens != NULL
             ? s->iface.count_tokens(s->iface.ud, system_prompt) +
                   s->iface.count_tokens(s->iface.ud, user_prompt)
             : asngn_token_heuristic(system_prompt) +
                   asngn_token_heuristic(user_prompt)) + 16;
  os_mutex_unlock(&s->mu);
  os_mutex_lock(&c->models_mu);
  slot_set_in_use(c, slot, 0);
  os_mutex_unlock(&c->models_mu);
  return n;
}

/* ---- embedding ---------------------------------------------------------- */

asngn_err asngn_models_embed(asngn_ctx *c, const char *text, float *out) {
  asngn_model_slot *s;
  asngn_err e;
  int slot;

  if (c == NULL || out == NULL) return ASNGN_ERR_INVALID;
  slot = c->role_slot[ASNGN_ROLE_EMBEDDER];
  if (slot < 0 || (size_t)slot >= c->models_n)
    return asngn_seterr(c, ASNGN_ERR_MODEL, "embedder role unavailable");
  s = &c->models[slot];

  os_mutex_lock(&c->models_mu);
  e = ensure_loaded_locked(c, slot);
  if (e == ASNGN_OK && s->iface.embed == NULL)
    e = asngn_seterr(c, ASNGN_ERR_MODEL, "model '%s' cannot embed",
                     s->cfg.id);
  if (e != ASNGN_OK) {
    os_mutex_unlock(&c->models_mu);
    return e;
  }
  slot_set_in_use(c, slot, 1);
  os_mutex_unlock(&c->models_mu);

  os_mutex_lock(&s->mu);
  e = s->iface.embed(s->iface.ud, text, out);
  os_mutex_unlock(&s->mu);

  os_mutex_lock(&c->models_mu);
  slot_set_in_use(c, slot, 0);
  s->last_used_ms = asngn_clock_mono_ms(&c->clock);
  os_mutex_unlock(&c->models_mu);

  if (e != ASNGN_OK)
    return asngn_seterr(c,
                        e == ASNGN_ERR_NOMEM ? ASNGN_ERR_NOMEM
                                             : ASNGN_ERR_MODEL,
                        "model '%s' embed failed (%s)", s->cfg.id,
                        asngn_err_name(e));
  return ASNGN_OK;
}

int asngn_models_embed_dim(asngn_ctx *c) {
  int slot;
  if (c == NULL) return 0;
  slot = c->role_slot[ASNGN_ROLE_EMBEDDER];
  if (slot < 0 || (size_t)slot >= c->models_n) return 0;
  return c->models[slot].cfg.dim;
}

void asngn_models_embed_hash(asngn_ctx *c, uint8_t out[32]) {
  models_aux *aux;
  memset(out, 0, 32);
  if (c == NULL) return;
  os_mutex_lock(&c->models_mu);
  aux = aux_find(c);
  if (aux != NULL) memcpy(out, aux->embed_hash, 32);
  os_mutex_unlock(&c->models_mu);
}
