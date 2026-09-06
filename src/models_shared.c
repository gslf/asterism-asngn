/* asmodel owns residency and serialization. These adapters own one backend
 * instance; engine lanes and Asper borrow the same manager. */
#include "asngn_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  asngn_ctx *ctx;
  asngn_model_iface iface;
  const char *id;
  bool borrowed;
  bool estimated_tokens;
} backend;
typedef struct { asmodel_token_fn fn; void *ud; } token_bridge;

asngn_err asngn_from_model_error(asmodel_err e) {
  switch (e) {
    case ASMODEL_OK: return ASNGN_OK;
    case ASMODEL_ERR_INVALID: return ASNGN_ERR_INVALID;
    case ASMODEL_ERR_NOMEM: return ASNGN_ERR_NOMEM;
    case ASMODEL_ERR_BUSY: return ASNGN_ERR_BUSY;
    case ASMODEL_ERR_LIMIT: return ASNGN_ERR_LIMIT;
    case ASMODEL_ERR_CANCELLED: return ASNGN_ERR_CANCELLED;
    case ASMODEL_ERR_UNSUPPORTED: return ASNGN_ERR_UNSUPPORTED;
    case ASMODEL_ERR_TIMEOUT: return ASNGN_ERR_TIMEOUT;
    default: return ASNGN_ERR_MODEL;
  }
}
static int model_error(asngn_err e) {
  switch (e) {
    case ASNGN_OK: return ASMODEL_OK;
    case ASNGN_ERR_INVALID: return ASMODEL_ERR_INVALID;
    case ASNGN_ERR_NOMEM: return ASMODEL_ERR_NOMEM;
    case ASNGN_ERR_BUSY: return ASMODEL_ERR_BUSY;
    case ASNGN_ERR_LIMIT: return ASMODEL_ERR_LIMIT;
    case ASNGN_ERR_CANCELLED: return ASMODEL_ERR_CANCELLED;
    case ASNGN_ERR_UNSUPPORTED: return ASMODEL_ERR_UNSUPPORTED;
    case ASNGN_ERR_TIMEOUT: return ASMODEL_ERR_TIMEOUT;
    default: return ASMODEL_ERR_BACKEND;
  }
}
static void token(const char *text, void *ud) {
  token_bridge *b = ud;
  if (b->fn) b->fn(text, strlen(text), b->ud);
}
static int generate(void *ud, const asmodel_input *input, const char *grammar,
                    const asmodel_generate_params *p, asmodel_token_fn fn, void *fn_ud,
                    volatile int *cancel, char **out, int *in, int *gen) {
  backend *b = ud;
  token_bridge bridge = {fn, fn_ud};
  asngn_gen_params params;
  asngn_operation op;
  int ti = 0, to = 0;
  int64_t started = asngn_clock_mono_ms(&b->ctx->clock);
  asngn_err e;
  asmodel_generation_info local = {0};
  asmodel_generation_info *info = p->result_info ? p->result_info : &local;
  memset(info,0,sizeof *info); info->usage_known = 1;
  memset(&params, 0, sizeof params);
  params.temp = p->temperature; params.top_p = p->top_p;
  params.repeat_penalty = p->repeat_penalty; params.max_tokens = p->max_tokens;
  params.reasoning = p->reasoning; params.reasoning_budget = p->reasoning_budget;
  params.output_schema = p->output_schema; params.tools = p->tools; params.result_info = info;
  params.require_constraint = p->require_constraint != 0;
  params.deadline_ms = p->deadline_ms;
  asmodel_provider counter = {0};
  counter.userdata = b->iface.ud; counter.count_prompt_tokens = b->iface.count_prompt_tokens;
  int prompt = asmodel_provider_measure_prompt(&counter,input).admission_tokens;
  int64_t reserve = (int64_t)prompt + p->max_tokens;
  if (p->output_schema) reserve += (int64_t)strlen(p->output_schema);
  if (p->tools) for (size_t i = 0; i < p->tools->count; i++) {
    const asmodel_tool_schema *schema = &p->tools->schemas[i];
    reserve += (int64_t)(strlen(schema->name)+strlen(schema->description)+strlen(schema->parameters)+128);
  }
  e = asngn_operation_begin(b->ctx, b->id, "generate", p->request_id, reserve, &op);
  if (e != ASNGN_OK) {
    info->finish_reason = ASMODEL_FINISH_ERROR;
    snprintf(info->error,sizeof info->error,"%s while reserving inference budget; provider not invoked",
             asngn_err_name(e));
    return model_error(e);
  }
  if (p->deadline_ms > 0) params.deadline_ms -= asngn_clock_mono_ms(&b->ctx->clock) - started;
  bool invoked = false;
  if (cancel && *cancel) e = ASNGN_ERR_CANCELLED;
  else if (p->deadline_ms > 0 && params.deadline_ms <= 0) e = ASNGN_ERR_TIMEOUT;
  else {
    invoked = true; info->usage_known = 0;
    e = b->iface.generate(b->iface.ud, input, grammar, &params,
                          fn ? token : NULL, &bridge, cancel, out, &ti, &to);
  }
  if (!invoked) info->usage_known = 1;
  if (info->finish_reason == ASMODEL_FINISH_UNKNOWN)
    info->finish_reason = e == ASNGN_OK ? ASMODEL_FINISH_STOP :
        e == ASNGN_ERR_LIMIT ? ASMODEL_FINISH_LENGTH :
        e == ASNGN_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
  asngn_err saved = asngn_operation_end(b->ctx, &op, ti, to, info->usage_known != 0, e);
  if (in) *in = ti;
  if (gen) *gen = to;
  return model_error(saved == ASNGN_OK ? e : saved);
}
static int embed(void *ud, const char *const *texts, size_t count, int is_query,
                 const asmodel_embed_params *params, float *out) {
  backend *b = ud;
  asngn_operation op;
  asmodel_embedding_info local = {0};
  asmodel_embed_params request = *params;
  asmodel_embedding_info *info = request.result_info ? request.result_info : &local;
  request.result_info = info;
  memset(info,0,sizeof *info); info->usage_known = 1;
  int64_t reserve = 0, started = asngn_clock_mono_ms(&b->ctx->clock);
  for (size_t i = 0; i < count; i++) {
    int n = b->iface.count_tokens ? b->iface.count_tokens(b->iface.ud,texts[i]) : -1;
    /* A byte estimate plus overhead is conservative, not a calibrated bound. */
    if (n < 0 || b->estimated_tokens) {
      size_t bytes = strlen(texts[i]);
      n = bytes > INT32_MAX-16 ? INT32_MAX : (int)bytes+16;
    }
    reserve += n;
  }
  asngn_err e = asngn_operation_begin(b->ctx,b->id,is_query ? "embed-query" : "embed-document",
      request.request_id,reserve,&op);
  if (e != ASNGN_OK) {
    snprintf(info->error,sizeof info->error,"%s while reserving inference budget; provider not invoked",
             asngn_err_name(e));
    return model_error(e);
  }
  bool invoked = false;
  if (request.cancel && *request.cancel) e = ASNGN_ERR_CANCELLED;
  else if (request.deadline_ms > 0 &&
      (request.deadline_ms -= asngn_clock_mono_ms(&b->ctx->clock)-started) <= 0) e = ASNGN_ERR_TIMEOUT;
  else { invoked = true; e = b->iface.embed(b->iface.ud,texts,count,is_query,&request,out); }
  if (!invoked) { memset(info,0,sizeof *info); info->usage_known = 1; }
  asngn_err saved = asngn_operation_end(b->ctx,&op,info->input_tokens,0,info->usage_known != 0,e);
  return model_error(saved == ASNGN_OK ? e : saved);
}

static int count(void *ud, const char *text) {
  backend *b = ud;
  return b->iface.count_tokens ? b->iface.count_tokens(b->iface.ud, text) : -1;
}
static int count_prompt(void *ud, const asmodel_input *input) {
  backend *b = ud;
  return b->iface.count_prompt_tokens ? b->iface.count_prompt_tokens(b->iface.ud, input) : -1;
}
static void destroy(void *ud) {
  backend *b = ud;
  if (!b->borrowed && b->iface.destroy) b->iface.destroy(b->iface.ud);
  free(b);
}
static int loader(void *ud, const asmodel_spec *spec, asmodel_provider *out,
                   char *error, size_t error_size) {
  asngn_ctx *c = ud;
  int slot = asngn_models_slot_for_id(c, spec->id);
  asngn_err e = ASNGN_OK;
  if (slot < 0) return ASMODEL_ERR_NOT_FOUND;
  backend *b = calloc(1, sizeof *b);
  if (!b) return ASMODEL_ERR_NOMEM;
  b->ctx = c; b->id = c->models[slot].cfg.id;
  b->borrowed = c->models[slot].injected;
  b->estimated_tokens = spec->backend == ASMODEL_BACKEND_OPENAI;
  if (b->borrowed) b->iface = c->models[slot].iface;
  else e = spec->backend == ASMODEL_BACKEND_OPENAI ?
      asngn_model_openai_create(c, &c->models[slot].cfg, &b->iface) :
      asngn_model_llama_create(c, &c->models[slot].cfg, &b->iface);
  if (e != ASNGN_OK) { snprintf(error, error_size, "%s", asngn_last_error(c)); free(b); return model_error(e); }
  memset(out, 0, sizeof *out);
  out->userdata = b;
  out->generate = b->iface.generate ? generate : NULL;
  out->embed = b->iface.embed ? embed : NULL;
  out->count_tokens = count; out->count_prompt_tokens = count_prompt;
  /* Embedded counts already include the template. Remote admission callbacks
   * include a conservative margin and must retain their estimated label. */
  out->token_quality = spec->backend == ASMODEL_BACKEND_EMBEDDED ? ASMODEL_TOKENS_EXACT : ASMODEL_TOKENS_ESTIMATED;
  out->tokenizer_id = spec->backend == ASMODEL_BACKEND_EMBEDDED ? b->id : NULL;
  out->chat_template_id = spec->backend == ASMODEL_BACKEND_EMBEDDED ? "embedded-model-template" : NULL;
  out->destroy = destroy;
  return ASMODEL_OK;
}
asngn_err asngn_shared_models_init(asngn_ctx *c) {
  if (asmodel_abi_version() != ASMODEL_ABI_VERSION) return ASNGN_ERR_CONFIG;
  if (c->owner) { c->shared_models = c->owner->shared_models; return ASNGN_OK; }
  asmodel_limits limits = {(size_t)c->cfg.max_resident, (size_t)c->cfg.max_ram_mb, (size_t)c->cfg.max_vram_mb};
  asmodel_err e = asmodel_manager_create(&limits, loader, c, &c->shared_models);
  if (e != ASMODEL_OK) return asngn_from_model_error(e);
  for (size_t i = 0; i < c->models_n; i++) {
    asngn_pool_entry *p = &c->models[i].cfg;
    asmodel_spec s = {0};
    s.id=p->id; s.backend=p->backend; s.path=p->path; s.base_url=p->base_url;
    s.remote_model=p->remote_model; s.api_key_env=p->api_key_env;
    s.remote_provider=p->remote_provider; s.context_tokens=p->ctx;
    s.threads=p->threads; s.gpu_layers=p->gpu_layers; s.embedding=p->embedding;
    s.pipeline = p->pipeline;
    if (p->embedding && p->backend == ASMODEL_BACKEND_EMBEDDED) {
      uint8_t hash[32];
      if (c->models[i].injected) memset(hash,0x11,sizeof hash);
      else if (asngn_sha256_file(p->path,hash) != ASNGN_OK) memset(hash,0,sizeof hash);
      uint8_t zero[32] = {0};
      if (memcmp(hash,zero,sizeof hash)) {
        asngn_sha256_hex(hash,32,s.pipeline.revision);
        strcpy(s.pipeline.tokenizer,s.pipeline.revision);
      } else { s.pipeline.revision[0] = 0; s.pipeline.tokenizer[0] = 0; }
      strcpy(s.pipeline.pooling,c->models[i].injected ? "fake-bow-v1" : "llama-mean-v1");
    }
    s.embedding_dim=p->dim; s.kv_cache=p->kv_cache; s.warm=p->warm;
    s.ram_mb=c->models[i].injected ? 0 : p->ram_mb;
    s.vram_mb=c->models[i].injected ? 0 : p->vram_mb;
    e = asmodel_manager_register(c->shared_models, &s);
    if (e != ASMODEL_OK) return asngn_from_model_error(e);
  }
  int slot = c->role_slot[ASNGN_ROLE_EMBEDDER];
  if (slot >= 0) {
    char *key = NULL;
    asmodel_err key_error = asmodel_manager_embedding_key(c->shared_models,c->models[slot].cfg.id,&key);
    if (key_error == ASMODEL_OK) asngn_sha256(key,strlen(key),c->embedding_hash);
    else if (key_error == ASMODEL_ERR_UNSUPPORTED) {
      char nonce[37]; asngn_uuid_v4(nonce);
      asngn_sha256(nonce,strlen(nonce),c->embedding_hash);
      asngn_log(c,ASNGN_LOG_WARN,"model","embedding pipeline revision is unknown; persistent vectors will be rebuilt on restart");
    } else return asngn_from_model_error(key_error);
    free(key);
  }
  return ASNGN_OK;
}
void asngn_shared_models_shutdown(asngn_ctx *c) {
  if (!c) return;
  if (!c->owner) asmodel_manager_destroy(c->shared_models);
  c->shared_models = NULL;
}
