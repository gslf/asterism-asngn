/* Conservative model admission and zone diagnostics. */
#include "context.h"
#include <string.h>

static asngn_err context_check(asngn_ctx *c, int slot, const asmodel_input *input,
                               int output_reserve, const asngn_prompt *zones, size_t extra_tokens,
                               asngn_context_diagnostics *observed) {
  asngn_context_diagnostics d;
  size_t attributed;
  int measured;
  if (observed) memset(observed, 0, sizeof *observed);
  if (c == NULL || slot < 0 || (size_t)slot >= c->models_n) return ASNGN_ERR_INVALID;
  memset(&d, 0, sizeof d);
  d.n_ctx = c->models[slot].cfg.ctx > 0 ? (size_t)c->models[slot].cfg.ctx : 32768u;
  d.output_reserve = output_reserve > 0 ? (size_t)output_reserve : (size_t)c->cfg.rich_tokens;
  d.safety_margin = (size_t)c->cfg.safety_margin;
  d.prompt_budget = d.output_reserve + d.safety_margin < d.n_ctx
                        ? d.n_ctx - d.output_reserve - d.safety_margin
                        : 0;
  measured = asngn_models_count_input(c, slot, input);
  if (measured < 0 || extra_tokens > SIZE_MAX - (size_t)measured) return ASNGN_ERR_INVALID;
  d.prompt_total = (size_t)measured + extra_tokens;
  if (zones != NULL) {
    d.system = zones->tok_system;
    d.memory = zones->tok_memory;
    d.catalog = zones->tok_catalog;
    d.summary = zones->tok_summary;
    d.verbatim = zones->tok_verbatim;
    d.working = zones->tok_working;
  } else {
    for (size_t i = 0; i < input->count; i++) {
      const asmodel_message *m = &input->messages[i];
      for (size_t j = 0; j < m->count; j++) {
        int n = asngn_models_count_tokens(c, slot, m->blocks[j].text);
        size_t *zone = m->role == ASMODEL_ROLE_SYSTEM || m->role == ASMODEL_ROLE_DEVELOPER
                           ? &d.system
                           : &d.working;
        *zone += n > 0 ? (size_t)n : 0;
      }
    }
  }
  attributed = d.system + d.memory + d.catalog + d.summary + d.verbatim + d.working;
  d.overhead = d.prompt_total > attributed ? d.prompt_total - attributed : 0;
  if (observed) *observed = d;
  if (d.prompt_total <= d.prompt_budget) return ASNGN_OK;

  os_mutex_lock(&c->err_mu);
  c->context_diag = d;
  snprintf(c->errbuf, sizeof c->errbuf,
           "context budget exceeded: prompt=%zu budget=%zu "
           "(n_ctx=%zu output=%zu safety=%zu; system=%zu memory=%zu "
           "catalog=%zu summary=%zu verbatim=%zu working=%zu overhead=%zu)",
           d.prompt_total, d.prompt_budget, d.n_ctx, d.output_reserve, d.safety_margin, d.system,
           d.memory, d.catalog, d.summary, d.verbatim, d.working, d.overhead);
  os_mutex_unlock(&c->err_mu);
  return ASNGN_ERR_CONTEXT;
}

asngn_err asngn_context_validate(asngn_ctx *c, int count_slot, const asngn_prompt *prompt,
                                 int output_reserve) {
  if (prompt == NULL) return ASNGN_ERR_INVALID;
  asmodel_text_input pair;
  asmodel_input_pair(&pair, prompt->system_text, prompt->user_text);
  return context_check(c, count_slot, &pair.input, output_reserve, prompt, 0, NULL);
}

asngn_err asngn_context_validate_text(asngn_ctx *c, int count_slot, const char *system_text,
                                      const char *user_text, int output_reserve) {
  asmodel_text_input pair;
  asmodel_input_pair(&pair, system_text, user_text);
  return context_check(c, count_slot, &pair.input, output_reserve, NULL, 0, NULL);
}

asngn_err asngn_context_validate_input(asngn_ctx *c, int slot, const asmodel_input *input,
                                       int output_reserve, size_t extra_tokens,
                                       asngn_context_diagnostics *observed) {
  return context_check(c, slot, input, output_reserve, NULL, extra_tokens, observed);
}

/* Count tokens of a zone snippet; empty text is zero. */
size_t asngn_context_tokens(asngn_ctx *c, int slot, const char *text) {
  int n;
  if (text == NULL || text[0] == '\0') return 0;
  n = asngn_models_count_tokens(c, slot, text);
  return n > 0 ? (size_t)n : 0;
}
