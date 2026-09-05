#include "asngn_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { asmodel_provider provider; } openai_ud;
typedef struct { asngn_token_fn fn; void *ud; } token_bridge;

static void remote_token(const char *utf8, size_t len, void *ud) {
  token_bridge *b = (token_bridge *)ud;
  (void)len;
  if (b->fn) b->fn(utf8, b->ud);
}

static asngn_err remote_generate(void *ud, const asmodel_input *input,
                                 const char *grammar,
                                 const asngn_gen_params *params,
                                 asngn_token_fn token_fn, void *token_ud,
                                 volatile int *cancel, char **out,
                                 int *out_in, int *out_gen) {
  openai_ud *u = (openai_ud *)ud;
  asmodel_generate_params p;
  token_bridge bridge;
  int rc;
  memset(&p, 0, sizeof p);
  p.temperature = params->temp; p.top_p = params->top_p;
  p.repeat_penalty = params->repeat_penalty;
  p.max_tokens = params->max_tokens;
  p.reasoning = params->reasoning;
  p.reasoning_budget = params->reasoning_budget;
  p.output_schema = params->output_schema; p.tools = params->tools;
  p.require_constraint = params->require_constraint ? 1 : 0;
  p.deadline_ms = params->deadline_ms; p.result_info = params->result_info;
  bridge.fn = token_fn; bridge.ud = token_ud;
  rc = u->provider.generate(u->provider.userdata, input, grammar, &p,
                            token_fn ? remote_token : NULL, &bridge, cancel,
                            out, out_in, out_gen);
  return asngn_from_model_error((asmodel_err)rc);
}

static asngn_err remote_embed(void *ud, const char *const *texts, size_t count,
                               int is_query, const asmodel_embed_params *params, float *out) {
  openai_ud *u = ud;
  return u->provider.embed ? asngn_from_model_error((asmodel_err)
      u->provider.embed(u->provider.userdata,texts,count,is_query,params,out)) : ASNGN_ERR_UNSUPPORTED;
}

static int remote_count(void *ud, const char *text) {
  openai_ud *u = (openai_ud *)ud;
  return u->provider.count_tokens ?
      u->provider.count_tokens(u->provider.userdata, text) : -1;
}

static int remote_count_prompt(void *ud, const asmodel_input *input) {
  openai_ud *u = (openai_ud *)ud;
  return asmodel_provider_measure_prompt(&u->provider, input).admission_tokens;
}

static void remote_destroy(void *ud) {
  openai_ud *u = (openai_ud *)ud;
  if (!u) return;
  if (u->provider.destroy) u->provider.destroy(u->provider.userdata);
  free(u);
}

asngn_err asngn_model_openai_create(asngn_ctx *c,
                                    const asngn_pool_entry *e,
                                    asngn_model_iface *out) {
  asmodel_spec spec;
  openai_ud *u;
  char error[256] = {0};
  memset(out, 0, sizeof *out);
  memset(&spec, 0, sizeof spec);
  spec.id = e->id; spec.backend = ASMODEL_BACKEND_OPENAI;
  spec.base_url = e->base_url; spec.remote_model = e->remote_model;
  spec.api_key_env = e->api_key_env;
  spec.remote_provider = e->remote_provider;
  spec.context_tokens = e->ctx; spec.embedding = e->embedding;
  spec.embedding_dim = e->dim;
  u = (openai_ud *)calloc(1, sizeof *u);
  if (!u) return ASNGN_ERR_NOMEM;
  if (asmodel_openai_provider_create(&spec, &u->provider, error,
                                     sizeof error) != 0) {
    free(u);
    return asngn_seterr(c, ASNGN_ERR_MODEL, "OpenAI backend '%s': %s",
                        e->id, error[0] ? error : "initialization failed");
  }
  out->ud = u;
  out->generate = e->embedding ? NULL : remote_generate;
  out->embed = e->embedding ? remote_embed : NULL;
  out->count_tokens = remote_count;
  out->count_prompt_tokens = remote_count_prompt;
  out->destroy = remote_destroy;
  asngn_log(c, ASNGN_LOG_INFO, "model",
            "configured OpenAI-compatible '%s' -> %s (%s)", e->id,
            e->remote_model, e->base_url);
  return ASNGN_OK;
}
