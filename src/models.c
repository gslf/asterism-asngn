/* Engine role policy and embedding identity. asmodel exclusively owns backend
 * residency, request serialization and eviction across all session lanes. */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- per-context auxiliary state ---------------------------------------- */
/* Per-context residency counters and embedding identity. Guarded by models_mu. */

typedef struct {
  uint8_t    embed_hash[32];      /* sha256 of embedder weights (or 0)  */
} models_aux;

static models_aux *aux_find(asngn_ctx *c) { return c ? c->model_aux : NULL; }
static models_aux *aux_claim(asngn_ctx *c) {
  if (!c->model_aux) c->model_aux=calloc(1,sizeof(models_aux));
  return c->model_aux;
}
static void aux_release(asngn_ctx *c) { free(c->model_aux);c->model_aux=NULL; }

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
    if ((src->path != NULL && s->cfg.path == NULL) ||
        (src->base_url && !s->cfg.base_url) ||
        (src->remote_model && !s->cfg.remote_model) ||
        (src->api_key_env && !s->cfg.api_key_env)) {
      size_t j;
      free(s->cfg.path); free(s->cfg.base_url); free(s->cfg.remote_model);
      free(s->cfg.api_key_env);
      for (j = 0; j < i; j++) {
        free(c->models[j].cfg.path);
        free(c->models[j].cfg.base_url);
        free(c->models[j].cfg.remote_model);
        free(c->models[j].cfg.api_key_env);
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

void asngn_models_warm(asngn_ctx *c) {
  if (c && c->shared_models && !c->bg_cancel)
    (void)asmodel_manager_warm(c->shared_models);
}

int asngn_models_slot_for_id(asngn_ctx *c, const char *id) {
  size_t i;
  if (c == NULL || id == NULL) return -1;
  for (i = 0; i < c->models_n; i++)
    if (strcmp(c->models[i].cfg.id, id) == 0) return (int)i;
  return -1;
}

/* Request policy belongs to the engine; model lifetime belongs to asmodel. */
typedef struct { asngn_token_fn fn; void *ud; } generation_stream;
static void generation_token(const char *text, size_t len, void *ud) {
  generation_stream *s = ud;
  (void)len;
  if (s->fn) s->fn(text, s->ud);
}
asngn_err asngn_models_generate(asngn_ctx *c, int slot, asngn_task_kind task,
    const char *sys, const char *user, const char *grammar, int max_tokens,
    int64_t deadline, asngn_token_fn fn, void *ud, volatile int *cancel,
    char **out, int *in, int *gen) {
  asmodel_generate_params p = {0};
  asmodel_generation_info info = {0};
  generation_stream stream = {fn, ud};
  asngn_err e;
  if (out) *out = NULL;
  if (in) *in = 0;
  if (gen) *gen = 0;
  if (!c || slot < 0 || (size_t)slot >= c->models_n) return ASNGN_ERR_MODEL;
  const asngn_sampling *sp = task_sampling(&c->cfg, task);
  p.temperature=sp->temp; p.top_p=sp->top_p; p.repeat_penalty=sp->repeat_penalty;
  p.max_tokens=max_tokens > 0 ? max_tokens : sp->max_tokens;
  p.reasoning=(task==ASNGN_TASK_DECIDE || task==ASNGN_TASK_CLASSIFY || task==ASNGN_TASK_JUDGE)
      ? ASMODEL_REASONING_REQUIRED_OFF : ASMODEL_REASONING_DEFAULT;
  p.require_constraint=grammar != NULL; p.result_info=&info;
  e=asngn_context_validate_text(c,slot,sys,user,p.max_tokens);
  if (e!=ASNGN_OK) return e;
  if (deadline > 0) {
    p.deadline_ms=deadline-asngn_clock_mono_ms(&c->clock);
    if (p.deadline_ms<=0) return asngn_seterr(c,ASNGN_ERR_TIMEOUT,"deadline expired before inference");
  }
  char *text = NULL;
  int ti=0, to=0;
  int64_t started=asngn_clock_mono_ms(&c->clock);
  e=asngn_from_model_error(asmodel_generate(c->shared_models,c->models[slot].cfg.id,
      sys,user,grammar,&p,fn ? generation_token : NULL,&stream,cancel,&text,&ti,&to));
  if (in) *in=ti;
  if (gen) *gen=to;
  if (out) *out=text; else free(text);
  char data[256];
  snprintf(data,sizeof data,"{model: \"%s\", task: \"%s\", tokens_in: %d, tokens_out: %d, ms: %lld, usage_known: %s}",
      c->models[slot].cfg.id,asngn_task_name(task),ti,to,
      (long long)(asngn_clock_mono_ms(&c->clock)-started),info.usage_known ? "true" : "false");
  asngn_tele_emit(c,"model_call",NULL,NULL,NULL,0,data);
  if (e!=ASNGN_OK) return asngn_seterr(c,e,"%s: %s",
      e==ASNGN_ERR_TIMEOUT ? "deadline expired" : asngn_err_name(e), info.error);
  return ASNGN_OK;
}

int asngn_models_count_tokens(asngn_ctx *c, int slot, const char *text) {
  int n = c && c->shared_models && slot >= 0 && (size_t)slot < c->models_n ?
      asmodel_count_tokens(c->shared_models,c->models[slot].cfg.id,text ? text : "") : -1;
  return n >= 0 ? n : asngn_token_heuristic(text);
}
int asngn_models_count_prompt(asngn_ctx *c, int slot, const char *sys, const char *user) {
  if (c && c->shared_models && slot >= 0 && (size_t)slot < c->models_n) {
    int n=asmodel_count_prompt_tokens(c->shared_models,c->models[slot].cfg.id,sys,user);
    if (n>=0) return n;
  }
  asmodel_provider unavailable = {0};
  return asmodel_provider_measure_prompt(&unavailable,sys,user).admission_tokens;
}

/* ---- embedding ---------------------------------------------------------- */

asngn_err asngn_models_embed_kind(asngn_ctx *c, const char *text, int is_query, float *out) {
  if (!c || !out) return ASNGN_ERR_INVALID;
  int slot=c->role_slot[ASNGN_ROLE_EMBEDDER];
  if (slot<0 || (size_t)slot>=c->models_n) return ASNGN_ERR_MODEL;
  return asngn_from_model_error(asmodel_embed(c->shared_models,c->models[slot].cfg.id,text,is_query,out));
}
asngn_err asngn_models_embed(asngn_ctx *c, const char *text, float *out) {
  return asngn_models_embed_kind(c,text,1,out);
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
