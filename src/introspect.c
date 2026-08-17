/*
 * introspect.c — the introspection surface of the public API: session
 * and sibling statistics, direct recall, projects, redaction toggle,
 * session export, telemetry tail. The TUI panes and asngn-mcp
 * are built on these.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

#include "asper.h"
#include "astools.h"

/* ── session stats ────────────────────────────────────────────────────── */

asngn_err asngn_session_get_stats(asngn_session *s,
                                  asngn_session_stats *out) {
  size_t i;
  if (s == NULL || out == NULL) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof *out);
  os_rwlock_rdlock(&s->lock);
  out->turns = s->led_n;
  for (i = 0; i < s->led_n; i++) {
    const asngn_ledger_entry *e = &s->led[i];
    out->tokens_prompt += e->pt_system + e->pt_memory + e->pt_catalog +
                          e->pt_summary + e->pt_verbatim + e->pt_working;
    out->tokens_memory += e->pt_memory;
    out->tokens_gen += e->gt_decision + e->gt_answer + e->gt_aux;
    out->tokens_saved += e->sv_cache + e->sv_digest;
    out->escalations += (size_t)(e->escalations > 0 ? e->escalations : 0);
    if (strcmp(e->cache, "hit") == 0) out->cache_hits++;
    else if (strcmp(e->cache, "adapt") == 0) out->cache_adapts++;
    else if (strcmp(e->cache, "miss") == 0) out->cache_misses++;
    if (strcmp(e->klass, "clarify") == 0) out->clarifies++;
    if (e->capped) out->capped++;
  }
  out->qpt_rolling = asngn_session_qpt(s);
  out->world_epoch = s->world_epoch;
  out->spent_tokens = s->spent_tokens;
  os_rwlock_rdunlock(&s->lock);
  return ASNGN_OK;
}

/* ── model pool ───────────────────────────────────────────────────────── */

/* Append `role` to `dst` (space-separated) when `id` serves it. */
static void model_role_cat(char *dst, size_t cap, const char *assigned,
                           const char *id, const char *role) {
  if (strcmp(assigned, id) != 0) return;
  if (dst[0] != '\0') strncat(dst, " ", cap - strlen(dst) - 1);
  strncat(dst, role, cap - strlen(dst) - 1);
}

asngn_err asngn_get_models(asngn_ctx *c, asngn_model_info *out,
                           size_t cap, size_t *out_n) {
  size_t i;
  if (c == NULL || out_n == NULL || (out == NULL && cap > 0))
    return ASNGN_ERR_INVALID;
  *out_n = c->cfg.pool_n;
  for (i = 0; i < c->cfg.pool_n && i < cap; i++) {
    const asngn_pool_entry *p = &c->cfg.pool[i];
    asngn_model_info *m = &out[i];
    char *real;
    const char *base;
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "%s", p->id);
    /* resolve symlinks so a pool alias shows the actual weights file */
    real = os_realpath(p->path != NULL ? p->path : "");
    base = real != NULL ? real : (p->path != NULL ? p->path : "");
    {
      const char *slash = strrchr(base, '/');
      snprintf(m->file, sizeof m->file, "%s",
               slash != NULL ? slash + 1 : base);
    }
    free(real);
    m->embedding = p->embedding ? 1 : 0;
    model_role_cat(m->roles, sizeof m->roles, c->cfg.role_router, p->id,
                   "router");
    model_role_cat(m->roles, sizeof m->roles, c->cfg.role_planner, p->id,
                   "planner");
    model_role_cat(m->roles, sizeof m->roles, c->cfg.role_generator,
                   p->id, "generator");
    model_role_cat(m->roles, sizeof m->roles, c->cfg.role_compressor,
                   p->id, "compressor");
    model_role_cat(m->roles, sizeof m->roles, c->cfg.role_adapter, p->id,
                   "adapter");
    model_role_cat(m->roles, sizeof m->roles, c->cfg.role_judge, p->id,
                   "judge");
    model_role_cat(m->roles, sizeof m->roles, c->cfg.role_embedder,
                   p->id, "embedder");
    os_mutex_lock(&c->models_mu);
    m->resident = (i < c->models_n &&
                   (c->models[i].loaded || c->models[i].injected))
                      ? 1
                      : 0;
    os_mutex_unlock(&c->models_mu);
  }
  return ASNGN_OK;
}

/* ── sibling stats ────────────────────────────────────────────────────── */

asngn_err asngn_get_sibling_stats(asngn_ctx *c, asngn_sibling_stats *out) {
  if (c == NULL || out == NULL) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof *out);
  out->asper_ok = c->asper_ok ? 1 : 0;
  out->astools_ok = c->astools_ok ? 1 : 0;
  if (c->asper_ok) {
    asper_stats st;
    char *slug = NULL;
    if (asper_get_stats(c->asper, &st) == ASPER_OK) {
      out->mem_identity = st.records_identity;
      out->mem_context = st.records_context;
      out->mem_project = st.records_project;
      out->mem_deprecated = st.records_deprecated;
      out->mem_last_cycle_at = st.last_cycle_at;
    }
    if (asper_project_active(c->asper, &slug) == ASPER_OK &&
        slug != NULL) {
      snprintf(out->project, sizeof out->project, "%s", slug);
      asper_free(slug);
    }
  }
  if (c->astools_ok) {
    astools_stats st;
    if (astools_get_stats(c->astools, &st) == ASTOOLS_OK) {
      out->tools_total = st.tools_total;
      out->tools_enabled = st.tools_enabled;
      out->tools_unavailable = st.tools_unavailable;
      out->tool_invocations = st.invocations;
      out->tool_ok = st.ok;
      out->tool_failed = st.failed;
      out->tool_denied = st.denied;
    }
  }
  return ASNGN_OK;
}

/* ── recall / projects / redaction ────────────────────────────────────── */

asngn_err asngn_recall(asngn_ctx *c, const char *question, char **out) {
  if (c == NULL || question == NULL || out == NULL)
    return ASNGN_ERR_INVALID;
  return asngn_siblings_recall(c, question, out);
}

asngn_err asngn_session_project(asngn_session *s, const char *slug) {
  asngn_ctx *c;
  asngn_err e;
  if (s == NULL) return ASNGN_ERR_INVALID;
  c = s->ctx;
  if (slug != NULL && !asngn_slug_valid(slug))
    return asngn_seterr(c, ASNGN_ERR_INVALID, "invalid project slug");
  e = asngn_siblings_project(c, slug);
  if (e != ASNGN_OK && e != ASNGN_ERR_UNSUPPORTED) return e;
  os_rwlock_wrlock(&s->lock);
  free(s->project);
  s->project = slug != NULL ? asngn_strdup(slug) : NULL;
  os_rwlock_wrunlock(&s->lock);
  return asngn_session_save_manifest(s);
}

asngn_err asngn_project_list(asngn_ctx *c, char ***out_slugs,
                             size_t *out_n) {
  char **slugs = NULL;
  size_t n = 0, i;
  char **copy;
  if (c == NULL || out_slugs == NULL || out_n == NULL)
    return ASNGN_ERR_INVALID;
  *out_slugs = NULL;
  *out_n = 0;
  if (!c->asper_ok) return ASNGN_OK;
  if (asper_project_list(c->asper, &slugs, &n) != ASPER_OK)
    return asngn_seterr(c, ASNGN_ERR_SIBLING, "asper: %s",
                        asper_last_error(c->asper));
  /* copy at the allocator boundary */
  copy = calloc(n > 0 ? n : 1, sizeof *copy);
  if (copy == NULL) {
    asper_strings_free(slugs, n);
    return ASNGN_ERR_NOMEM;
  }
  for (i = 0; i < n; i++) {
    copy[i] = asngn_strdup(slugs[i]);
    if (copy[i] == NULL) {
      asngn_strings_free(copy, i);
      asper_strings_free(slugs, n);
      return ASNGN_ERR_NOMEM;
    }
  }
  asper_strings_free(slugs, n);
  *out_slugs = copy;
  *out_n = n;
  return ASNGN_OK;
}

asngn_err asngn_session_redact(asngn_session *s, int on) {
  if (s == NULL) return ASNGN_ERR_INVALID;
  os_rwlock_wrlock(&s->lock);
  s->redact_context = on != 0;
  os_rwlock_wrunlock(&s->lock);
  if (!on)
    asngn_log(s->ctx, ASNGN_LOG_WARN, "safety",
              "%s: context redaction disabled by the operator", s->slug);
  return asngn_session_save_manifest(s);
}

/* ── telemetry tail ───────────────────────────────────────────────────── */

asngn_err asngn_telemetry_tail(asngn_ctx *c, size_t n, char ***out,
                               size_t *out_n) {
  if (c == NULL || out == NULL || out_n == NULL) return ASNGN_ERR_INVALID;
  return asngn_tele_tail(c, n, out, out_n);
}

/* ── session export ──────────────────────────────────────────────────── */

asngn_err asngn_session_export(asngn_session *s, char **out_text) {
  asngn_ctx *c;
  asngn_session_stats st;
  asngn_buf txt, xc;
  asngn_err e;
  size_t i;
  char *path = NULL;

  if (s == NULL || out_text == NULL) return ASNGN_ERR_INVALID;
  *out_text = NULL;
  c = s->ctx;
  e = asngn_session_get_stats(s, &st);
  if (e != ASNGN_OK) return e;

  asngn_buf_init(&txt);
  asngn_buf_init(&xc);

  e = asngn_buf_printf(&txt,
      "asngn session report \xE2\x9C\xA6 %s\n"
      "======================================================\n"
      "turns ............ %zu\n"
      "tokens prompt .... %zu\n"
      "tokens gen ....... %zu\n"
      "tokens saved ..... %zu  (cache + digest)\n"
      "cache ............ %zu hit / %zu adapt / %zu miss\n"
      "escalations ...... %zu\n"
      "capped answers ... %zu\n"
      "clarifications ... %zu\n"
      "rolling QpT ...... %.2f\n"
      "world epoch ...... %llu\n"
      "\n"
      "turn  class     detail  mode    tier      cache  tokens  quality\n"
      "----  --------  ------  ------  --------  -----  ------  -------\n",
      s->slug, st.turns, st.tokens_prompt, st.tokens_gen,
      st.tokens_saved, st.cache_hits, st.cache_adapts, st.cache_misses,
      st.escalations, st.capped, st.clarifies, st.qpt_rolling,
      (unsigned long long)st.world_epoch);
  if (e != ASNGN_OK) goto out;

  e = asngn_buf_printf(&xc,
                       "#asngn_report {\n  session: \"%s\",\n"
                       "  turns: %zu,\n  tokens_prompt: %zu,\n"
                       "  tokens_gen: %zu,\n  tokens_saved: %zu,\n"
                       "  cache: { hit: %zu, adapt: %zu, miss: %zu },\n"
                       "  escalations: %zu,\n  capped: %zu,\n"
                       "  clarifies: %zu,\n  qpt_rolling: %.4f,\n"
                       "  world_epoch: %llu,\n  entries: [\n",
                       s->slug, st.turns, st.tokens_prompt, st.tokens_gen,
                       st.tokens_saved, st.cache_hits, st.cache_adapts,
                       st.cache_misses, st.escalations, st.capped,
                       st.clarifies, st.qpt_rolling,
                       (unsigned long long)st.world_epoch);
  if (e != ASNGN_OK) goto out;

  os_rwlock_rdlock(&s->lock);
  for (i = 0; i < s->led_n && e == ASNGN_OK; i++) {
    const asngn_ledger_entry *le = &s->led[i];
    char q[16] = "-";
    if (le->has_judge) snprintf(q, sizeof q, "%.1f", le->judge);
    e = asngn_buf_printf(&txt,
                         "%4zu  %-8s  %-6s  %-6s  %-8s  %-5s  %6zu  %7s\n",
                         le->turn, le->klass, le->detail, le->mode,
                         le->tier, le->cache,
                         asngn_ledger_total_tokens(le), q);
    if (e == ASNGN_OK)
      e = asngn_buf_printf(&xc,
                           "    { turn: %zu, class: \"%s\", cache: "
                           "\"%s\", tokens: %zu },\n",
                           le->turn, le->klass, le->cache,
                           asngn_ledger_total_tokens(le));
  }
  os_rwlock_rdunlock(&s->lock);
  if (e != ASNGN_OK) goto out;
  e = asngn_buf_appends(&xc, "  ],\n}\n");
  if (e != ASNGN_OK) goto out;

  path = os_path_join(s->dir, "report.xcdn");
  if (path == NULL) { e = ASNGN_ERR_NOMEM; goto out; }
  e = asngn_write_atomic(c, path, xc.data, xc.len);
  free(path);
  path = NULL;
  if (e != ASNGN_OK) goto out;
  path = os_path_join(s->dir, "report.txt");
  if (path == NULL) { e = ASNGN_ERR_NOMEM; goto out; }
  e = asngn_write_atomic(c, path, txt.data, txt.len);
  if (e != ASNGN_OK) goto out;

  *out_text = asngn_buf_detach(&txt);
  if (*out_text == NULL) e = ASNGN_ERR_NOMEM;

out:
  free(path);
  asngn_buf_free(&txt);
  asngn_buf_free(&xc);
  return e;
}
