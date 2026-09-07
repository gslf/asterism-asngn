/* Startup diagnostics never load a model, run a tool, or create engine state. */
#include "asngn_internal.h"
#include "asper.h"
#include "astools.h"
#include <stdlib.h>
#include <string.h>

asngn_err asngn_diagnose(const asngn_open_params *params, char **out) {
  asngn_ctx *c;
  asngn_buf report;
  char *root = NULL, *config = NULL;
  asngn_err e;
  bool ready = true;
  if (!params || !out) return ASNGN_ERR_INVALID;
  *out = NULL;
  c = calloc(1, sizeof *c);
  if (!c) return ASNGN_ERR_NOMEM;
  os_mutex_init(&c->err_mu); os_mutex_init(&c->log_mu);
  c->clock = asngn_clock_system();
  asngn_config_defaults(&c->cfg);
  asngn_buf_init(&report);
  root = params->engine_root ? asngn_strdup(params->engine_root) : asngn_default_engine_root();
  if (!root) { e = ASNGN_ERR_NOMEM; goto done; }
  config = params->config_path ? asngn_strdup(params->config_path) : os_path_join(root, "config.xcdn");
  if (!config) { e = ASNGN_ERR_NOMEM; goto done; }
  if (!params->engine_root) {
    free(c->cfg.root); c->cfg.root = asngn_strdup(root);
    if (!c->cfg.root) { e = ASNGN_ERR_NOMEM; goto done; }
  }
  bool configured = os_file_exists(config);
  e = asngn_config_load(c, &c->cfg, configured ? config : NULL);
  if (e != ASNGN_OK) {
    (void)asngn_buf_printf(&report, "Configuration invalid: %s\n", asngn_last_error(c));
    goto done;
  }
  if (!params->engine_root && configured) {
    free(root); root = asngn_strdup(c->cfg.root);
    if (!root) { e = ASNGN_ERR_NOMEM; goto done; }
  }
  e = asngn_buf_printf(&report, "asngn %s\nConfiguration: %s (%s)\nEngine root: %s\n",
                       asngn_version(), config, configured ? "loaded" : "missing; defaults", root);
  if (params->config_path && !configured) ready = false;
  if (asmodel_abi_version() != ASMODEL_ABI_VERSION || asper_abi_version() != ASPER_ABI_VERSION ||
      astools_abi_version() != ASTOOLS_ABI_VERSION) {
    ready = false;
    if (e == ASNGN_OK) e = asngn_buf_appends(&report, "Component ABI mismatch: rebuild the pinned release.\n");
  }
  for (size_t i = 0; e == ASNGN_OK && i < c->cfg.pool_n; i++) {
    asngn_pool_entry *m = &c->cfg.pool[i];
    if (m->backend == ASMODEL_BACKEND_OPENAI) {
      bool credential = !m->api_key_env || !m->api_key_env[0] ||
                         (getenv(m->api_key_env) && getenv(m->api_key_env)[0]);
      if (!credential) ready = false;
      e = asngn_buf_printf(&report, "Model %s: remote; token count estimated; endpoint not probed%s\n",
                           m->id, credential ? "" : "; configured credential environment variable missing");
    } else {
      char *path = os_path_is_abs(m->path) ? asngn_strdup(m->path) : os_path_join(root, m->path);
      uint64_t bytes = 0;
      bool exists = path && os_file_size(path, &bytes) == ASNGN_OK && bytes > 0;
      if (!exists) ready = false;
      e = asngn_buf_printf(&report, "Model %s: %s (%s)\n", m->id, exists ? "file present; load not probed" : "weights missing", path ? path : "allocation failed");
      free(path);
#ifndef ASNGN_WITH_LLAMA
      ready = false;
      if (e == ASNGN_OK) e = asngn_buf_appends(&report, "Embedded backend absent: rebuild with ASNGN_WITH_LLAMA=ON or configure a remote model.\n");
#endif
    }
  }
  if (e == ASNGN_OK) e = asngn_buf_appends(&report, ready ? "Preflight: ready for runtime probes.\n" : "Preflight: action required.\n");
  if (e == ASNGN_OK && !ready) e = ASNGN_ERR_CONFIG;
done:
  *out = report.data; report.data = NULL;
  asngn_buf_free(&report);
  free(root); free(config); asngn_config_free(&c->cfg);
  os_mutex_destroy(&c->err_mu); os_mutex_destroy(&c->log_mu); free(c);
  return e;
}
