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
  asmodel_generation_info info;
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
static int generate(void *ud, const char *sys, const char *user, const char *grammar,
                    const asmodel_generate_params *p, asmodel_token_fn fn, void *fn_ud,
                    volatile int *cancel, char **out, int *in, int *gen) {
  backend *b = ud;
  token_bridge bridge = {fn, fn_ud};
  asngn_gen_params params;
  asngn_operation op;
  int ti = 0, to = 0;
  int64_t started = asngn_clock_mono_ms(&b->ctx->clock);
  asngn_err e;
  memset(&b->info, 0, sizeof b->info);
  memset(&params, 0, sizeof params);
  params.temp = p->temperature; params.top_p = p->top_p;
  params.repeat_penalty = p->repeat_penalty; params.max_tokens = p->max_tokens;
  params.reasoning = p->reasoning; params.reasoning_budget = p->reasoning_budget;
  params.require_constraint = p->require_constraint != 0;
  params.deadline_ms = p->deadline_ms;
  int prompt = b->iface.count_prompt_tokens ?
      b->iface.count_prompt_tokens(b->iface.ud, sys, user) :
      asngn_token_heuristic(sys) + asngn_token_heuristic(user) + 16;
  e = asngn_operation_begin(b->ctx, b->id, "generate", (int64_t)prompt + p->max_tokens, &op);
  if (e != ASNGN_OK) return model_error(e);
  if (p->deadline_ms > 0) params.deadline_ms -= asngn_clock_mono_ms(&b->ctx->clock) - started;
  bool invoked = false;
  if (cancel && *cancel) e = ASNGN_ERR_CANCELLED;
  else if (p->deadline_ms > 0 && params.deadline_ms <= 0) e = ASNGN_ERR_TIMEOUT;
  else {
    invoked = true;
    e = b->iface.generate(b->iface.ud, sys, user, grammar, &params,
                          fn ? token : NULL, &bridge, cancel, out, &ti, &to);
  }
  if (invoked && b->iface.last_generation_info)
    (void)b->iface.last_generation_info(b->iface.ud, &b->info);
  else {
    b->info.input_tokens = ti; b->info.output_tokens = to;
    b->info.usage_known = !invoked || e == ASNGN_OK || e == ASNGN_ERR_LIMIT;
    b->info.finish_reason = e == ASNGN_OK ? ASMODEL_FINISH_STOP :
        e == ASNGN_ERR_LIMIT ? ASMODEL_FINISH_LENGTH :
        e == ASNGN_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
  }
  asngn_err saved = asngn_operation_end(b->ctx, &op, ti, to, b->info.usage_known != 0, e);
  if (in) *in = ti;
  if (gen) *gen = to;
  return model_error(saved == ASNGN_OK ? e : saved);
}
static int embed(void *ud, const char *text, int is_query, float *out) {
  backend *b = ud;
  asngn_operation op;
  int n = b->iface.count_tokens ? b->iface.count_tokens(b->iface.ud, text) : asngn_token_heuristic(text);
  asngn_err e = asngn_operation_begin(b->ctx, b->id, is_query ? "embed-query" : "embed-document", n, &op);
  if (e != ASNGN_OK) return model_error(e);
  e = b->iface.embed(b->iface.ud, text, is_query, out);
  asngn_err saved = asngn_operation_end(b->ctx, &op, n, 0, false, e);
  return model_error(saved == ASNGN_OK ? e : saved);
}
static int count(void *ud, const char *text) {
  backend *b = ud;
  return b->iface.count_tokens ? b->iface.count_tokens(b->iface.ud, text) : -1;
}
static int count_prompt(void *ud, const char *sys, const char *user) {
  backend *b = ud;
  return b->iface.count_prompt_tokens ? b->iface.count_prompt_tokens(b->iface.ud, sys, user) : -1;
}
static const char *last_error(void *ud) {
  backend *b = ud;
  return b->iface.last_error ? b->iface.last_error(b->iface.ud) : asngn_last_error(b->ctx);
}
static int info(void *ud, asmodel_generation_info *out) { *out = ((backend *)ud)->info; return 0; }
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
  out->last_error = last_error; out->last_generation_info = info; out->destroy = destroy;
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
    s.embedding_dim=p->dim; s.kv_cache=p->kv_cache; s.warm=p->warm;
    s.ram_mb=c->models[i].injected ? 0 : p->ram_mb;
    s.vram_mb=c->models[i].injected ? 0 : p->vram_mb;
    e = asmodel_manager_register(c->shared_models, &s);
    if (e != ASMODEL_OK) return asngn_from_model_error(e);
  }
  return ASNGN_OK;
}
void asngn_shared_models_shutdown(asngn_ctx *c) {
  if (!c) return;
  if (!c->owner) asmodel_manager_destroy(c->shared_models);
  c->shared_models = NULL;
}
