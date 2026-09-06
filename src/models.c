/* Engine role policy and embedding identity. asmodel exclusively owns backend
 * residency, request serialization and eviction across all session lanes. */

#include "asngn_internal.h"
#include "context.h"

#include <stdlib.h>
#include <string.h>

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

    s->cfg = *src; /* Configuration strings are immutable and outlive the lanes. */
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

  if (c->owner) {
    memcpy(c->embedding_hash, c->owner->embedding_hash, 32);
    return ASNGN_OK;
  }

  return ASNGN_OK;
}

void asngn_models_shutdown(asngn_ctx *c) {
  size_t i;
  if (c == NULL) return;
  for (i = 0; i < c->models_n; i++) {
    asngn_model_slot *s = &c->models[i];
    if (s->injected && s->iface.destroy != NULL)
      s->iface.destroy(s->iface.ud); /* fakes carry no-op destroys */
    memset(&s->iface, 0, sizeof s->iface);
    s->injected = false;
  }
  c->models_n = 0;

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
asngn_err asngn_models_generate_input(asngn_ctx *c, int slot, asngn_task_kind task,
    const asmodel_input *input, const char *grammar, const char *schema,
    const asmodel_tools *tools, int max_tokens,
    int64_t deadline, asngn_token_fn fn, void *ud, volatile int *cancel,
    char **out, int *in, int *gen, const asngn_turn_state *turn) {
  asmodel_generate_params p = {0};
  asmodel_generation_info info = {0};
  generation_stream stream = {fn, ud};
  asngn_err e;
  if (tools) asmodel_tool_calls_clear(tools->output);
  if (out) *out = NULL;
  if (in) *in = 0;
  if (gen) *gen = 0;
  if (!c || slot < 0 || (size_t)slot >= c->models_n) return ASNGN_ERR_MODEL;
  const asngn_sampling *sp = task_sampling(&c->cfg, task);
  p.temperature=sp->temp; p.top_p=sp->top_p; p.repeat_penalty=sp->repeat_penalty;
  p.max_tokens=max_tokens > 0 ? max_tokens : sp->max_tokens;
  p.reasoning=!tools && (task==ASNGN_TASK_DECIDE || task==ASNGN_TASK_CLASSIFY || task==ASNGN_TASK_JUDGE)
      ? ASMODEL_REASONING_REQUIRED_OFF : ASMODEL_REASONING_DEFAULT;
  p.output_schema = schema ? schema : tools ? NULL : asngn_protocol_scalar_schema(task);
  p.tools = tools;
  p.require_constraint=grammar != NULL || p.output_schema != NULL; p.result_info=&info;
  size_t extra = p.output_schema ? strlen(p.output_schema) : 0;
  if (tools) {
    if (!tools->schemas || tools->count > 64) return ASNGN_ERR_INVALID;
    for (size_t i = 0; i < tools->count; i++) {
      const asmodel_tool_schema *v = &tools->schemas[i];
      if (!v->name || !v->description || !v->parameters) return ASNGN_ERR_INVALID;
      extra += strlen(v->name)+strlen(v->description)+strlen(v->parameters)+128;
    }
  }
  asngn_context_diagnostics budget = {0};
  char request[37];
  asngn_uuid_v4(request);
  e=asngn_context_validate_input(c,slot,input,p.max_tokens,extra,&budget);
  char *trace = asngn_request_trace(c,turn,slot,task,input,grammar,&p,&budget,e);
  if (trace) asngn_tele_emit(c,"request_context",request,turn ? turn->led.turn_id : NULL,
      turn && turn->s ? turn->s->slug : NULL,turn ? turn->led.turn : 0,trace);
  free(trace);
  if (deadline > 0) {
    p.deadline_ms=deadline-asngn_clock_mono_ms(&c->clock);
    if (e==ASNGN_OK && p.deadline_ms<=0)
      e=asngn_seterr(c,ASNGN_ERR_TIMEOUT,"deadline expired before inference");
  }
  if (e!=ASNGN_OK) {
    info.usage_known=1;
    asngn_request_result(c,turn,request,slot,task,&info,e,0,false);
    return e;
  }
  char *text = NULL;
  int ti=0, to=0;
  int64_t started=asngn_clock_mono_ms(&c->clock);
  e=asngn_from_model_error(asmodel_generate(c->shared_models,c->models[slot].cfg.id,
      input,grammar,&p,fn ? generation_token : NULL,&stream,cancel,&text,&ti,&to));
  if (e == ASNGN_OK && info.json_output) {
    e = asngn_protocol_decode(task, p.output_schema, &text);
    if (e != ASNGN_OK) snprintf(info.error, sizeof info.error,
        "model output violates the %s JSON contract", asngn_task_name(task));
  }
  if (in) *in=ti;
  if (gen) *gen=to;
  if (out) *out=text; else free(text);
  info.input_tokens=ti; info.output_tokens=to;
  asngn_request_result(c,turn,request,slot,task,&info,e,
      asngn_clock_mono_ms(&c->clock)-started,true);
  if (e!=ASNGN_OK) return asngn_seterr(c,e,"%s: %s",
      e==ASNGN_ERR_TIMEOUT ? "deadline expired" : asngn_err_name(e), info.error);
  return ASNGN_OK;
}

/* Small engine phases deliberately use two text messages. */
asngn_err asngn_models_generate(asngn_ctx *c, int slot, asngn_task_kind task,
    const char *sys, const char *user, const char *grammar, const char *schema, int max_tokens,
    int64_t deadline, asngn_token_fn fn, void *ud, volatile int *cancel,
    char **out, int *in, int *gen) {
  asmodel_text_input pair; asmodel_input_pair(&pair,sys,user);
  return asngn_models_generate_input(c,slot,task,&pair.input,grammar,schema,NULL,max_tokens,
      deadline,fn,ud,cancel,out,in,gen,NULL);
}

int asngn_models_count_tokens(asngn_ctx *c, int slot, const char *text) {
  int n = c && c->shared_models && slot >= 0 && (size_t)slot < c->models_n ?
      asmodel_count_tokens(c->shared_models,c->models[slot].cfg.id,text ? text : "") : -1;
  return n >= 0 ? n : asngn_token_heuristic(text);
}
int asngn_models_count_input(asngn_ctx *c, int slot, const asmodel_input *input) {
  if (c && c->shared_models && slot >= 0 && (size_t)slot < c->models_n) {
    int n = asmodel_count_prompt_tokens(c->shared_models,c->models[slot].cfg.id,input);
    if (n >= 0) return n;
  }
  asmodel_provider unavailable = {0};
  return asmodel_provider_measure_prompt(&unavailable,input).admission_tokens;
}
int asngn_models_count_prompt(asngn_ctx *c, int slot, const char *sys, const char *user) {
  asmodel_text_input pair; asmodel_input_pair(&pair,sys,user);
  return asngn_models_count_input(c,slot,&pair.input);
}

/* ---- embedding ---------------------------------------------------------- */

asngn_err asngn_models_embed_many(asngn_ctx *c, const char *const *texts, size_t count,
    int is_query, float *out, asmodel_embedding_info *info) {
  if (!c || !out) return ASNGN_ERR_INVALID;
  int slot=c->role_slot[ASNGN_ROLE_EMBEDDER];
  if (slot<0 || (size_t)slot>=c->models_n) return ASNGN_ERR_MODEL;
  asmodel_embed_params params = {.result_info = info};
  if (info) memset(info,0,sizeof *info);
  if (!count || c->models[slot].cfg.dim <= 0 ||
      (size_t)c->models[slot].cfg.dim > SIZE_MAX/count/sizeof(float)) return ASNGN_ERR_INVALID;
  asngn_turn_state *turn = c->active_task ? c->active_task->turn : NULL;
  if (turn) {
    params.cancel = &turn->cancel;
    if (turn->deadline_mono > 0) {
      params.deadline_ms = turn->deadline_mono-asngn_clock_mono_ms(&c->clock);
      if (params.deadline_ms <= 0) return ASNGN_ERR_TIMEOUT;
    }
  }
  return asngn_from_model_error(asmodel_embed(c->shared_models,c->models[slot].cfg.id,
      texts,count,is_query,&params,out,count*(size_t)c->models[slot].cfg.dim));
}
asngn_err asngn_models_embed(asngn_ctx *c, const char *text, float *out) {
  return asngn_models_embed_many(c,&text,1,1,out,NULL);
}

int asngn_models_embed_dim(asngn_ctx *c) {
  int slot;
  if (c == NULL) return 0;
  slot = c->role_slot[ASNGN_ROLE_EMBEDDER];
  if (slot < 0 || (size_t)slot >= c->models_n) return 0;
  return c->models[slot].cfg.dim;
}

void asngn_models_embed_hash(asngn_ctx *c, uint8_t out[32]) {
  if (c) memcpy(out, c->embedding_hash, 32);
  else memset(out, 0, 32);
}
