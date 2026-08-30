#include "asngn_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { asngn_ctx *ctx; int slot; int embedding; } shared_ref;
typedef struct { asmodel_token_fn fn; void *ud; } shared_token_bridge;

static void shared_token(const char *text, void *ud) {
  shared_token_bridge *b = (shared_token_bridge *)ud;
  if (b->fn) b->fn(text, strlen(text), b->ud);
}

static int shared_generate(void *ud, const char *sys, const char *user,
                           const char *grammar,
                           const asmodel_generate_params *params,
                           asmodel_token_fn token_fn, void *token_ud,
                           volatile int *cancel, char **out,
                           int *out_in, int *out_gen) {
  shared_ref *r = (shared_ref *)ud;
  shared_token_bridge bridge;
  int64_t deadline_mono = params->deadline_ms > 0
                              ? asngn_clock_mono_ms(&r->ctx->clock) +
                                    params->deadline_ms
                              : 0;
  (void)cancel;
  bridge.fn = token_fn; bridge.ud = token_ud;
  return asngn_models_generate(r->ctx, r->slot, ASNGN_TASK_JUDGE,
                               sys, user, grammar, params->max_tokens,
                               deadline_mono,
                               token_fn ? shared_token : NULL, &bridge,
                               &r->ctx->call_cancel, out, out_in, out_gen) ==
                 ASNGN_OK ? 0 : -1;
}

static int shared_embed(void *ud, const char *text, int is_query, float *out) {
  shared_ref *r = (shared_ref *)ud;
  (void)is_query;
  if (r->slot != r->ctx->role_slot[ASNGN_ROLE_EMBEDDER]) return -1;
  return asngn_models_embed(r->ctx, text, out) == ASNGN_OK ? 0 : -1;
}

static int shared_count(void *ud, const char *text) {
  shared_ref *r = (shared_ref *)ud;
  return asngn_models_count_tokens(r->ctx, r->slot, text);
}

static int shared_count_prompt(void *ud, const char *sys, const char *user) {
  shared_ref *r = (shared_ref *)ud;
  return asngn_models_count_prompt(r->ctx, r->slot, sys, user);
}

static void shared_destroy(void *ud) { free(ud); }

static int shared_loader(void *ud, const asmodel_spec *spec,
                         asmodel_provider *out, char *error,
                         size_t error_size) {
  asngn_ctx *c = (asngn_ctx *)ud;
  int slot = asngn_models_slot_for_id(c, spec->id);
  shared_ref *r;
  if (slot < 0) {
    snprintf(error, error_size, "unknown asngn slot '%s'", spec->id);
    return -1;
  }
  r = (shared_ref *)calloc(1, sizeof *r);
  if (!r) return -1;
  r->ctx = c; r->slot = slot; r->embedding = spec->embedding;
  memset(out, 0, sizeof *out);
  out->userdata = r;
  out->generate = spec->embedding ? NULL : shared_generate;
  out->embed = spec->embedding ? shared_embed : NULL;
  out->count_tokens = shared_count;
  out->count_prompt_tokens = shared_count_prompt;
  out->destroy = shared_destroy;
  return 0;
}

asngn_err asngn_shared_models_init(asngn_ctx *c) {
  asmodel_limits limits;
  size_t i;
  asmodel_err me;
  memset(&limits, 0, sizeof limits);
  /* The underlying asngn pool enforces the real residency and memory limits;
   * facade slots have zero cost and only lend calls to sibling components. */
  me = asmodel_manager_create(&limits, shared_loader, c, &c->shared_models);
  if (me != ASMODEL_OK) return ASNGN_ERR_NOMEM;
  for (i = 0; i < c->models_n; ++i) {
    asmodel_spec spec;
    asngn_pool_entry *e = &c->models[i].cfg;
    memset(&spec, 0, sizeof spec);
    spec.id = e->id; spec.backend = e->backend; spec.path = e->path;
    spec.base_url = e->base_url; spec.remote_model = e->remote_model;
    spec.api_key_env = e->api_key_env;
    spec.remote_provider = e->remote_provider;
    spec.context_tokens = e->ctx; spec.threads = e->threads;
    spec.gpu_layers = e->gpu_layers; spec.embedding = e->embedding;
    spec.embedding_dim = e->dim; spec.kv_cache = e->kv_cache;
    me = asmodel_manager_register(c->shared_models, &spec);
    if (me != ASMODEL_OK) {
      asngn_shared_models_shutdown(c);
      return asngn_seterr(c, ASNGN_ERR_MODEL, "shared model manager: %s",
                          asmodel_err_name(me));
    }
  }
  return ASNGN_OK;
}

void asngn_shared_models_shutdown(asngn_ctx *c) {
  if (!c) return;
  asmodel_manager_destroy(c->shared_models);
  c->shared_models = NULL;
}
