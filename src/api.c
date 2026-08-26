/*
 * api.c — the public C API of libasngn and context lifecycle.
 *
 * The TUI and asngn-mcp are pure clients of this surface. All strings
 * are UTF-8; every out-allocation is released with the matching
 * asngn_free*; asngn_open/asngn_close are not thread-safe with respect
 * to the same context, everything else is.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

/* ── version and error names ──────────────────────────────────────────── */

const char *asngn_version(void) { return "0.1.0"; }

const char *asngn_err_name(asngn_err e) {
  switch (e) {
  case ASNGN_OK:              return "ASNGN_OK";
  case ASNGN_ERR_IO:          return "ASNGN_ERR_IO";
  case ASNGN_ERR_PARSE:       return "ASNGN_ERR_PARSE";
  case ASNGN_ERR_CONFIG:      return "ASNGN_ERR_CONFIG";
  case ASNGN_ERR_MODEL:       return "ASNGN_ERR_MODEL";
  case ASNGN_ERR_NOT_FOUND:   return "ASNGN_ERR_NOT_FOUND";
  case ASNGN_ERR_INVALID:     return "ASNGN_ERR_INVALID";
  case ASNGN_ERR_DENIED:      return "ASNGN_ERR_DENIED";
  case ASNGN_ERR_TIMEOUT:     return "ASNGN_ERR_TIMEOUT";
  case ASNGN_ERR_CANCELLED:   return "ASNGN_ERR_CANCELLED";
  case ASNGN_ERR_BUSY:        return "ASNGN_ERR_BUSY";
  case ASNGN_ERR_PROTOCOL:    return "ASNGN_ERR_PROTOCOL";
  case ASNGN_ERR_CONTEXT:     return "ASNGN_ERR_CONTEXT";
  case ASNGN_ERR_UNSUPPORTED: return "ASNGN_ERR_UNSUPPORTED";
  case ASNGN_ERR_SIBLING:     return "ASNGN_ERR_SIBLING";
  case ASNGN_ERR_NOMEM:       return "ASNGN_ERR_NOMEM";
  }
  return "ASNGN_ERR_?";
}

asngn_err asngn_seterr(asngn_ctx *c, asngn_err e, const char *fmt, ...) {
  if (c != NULL && fmt != NULL) {
    va_list ap;
    os_mutex_lock(&c->err_mu);
    va_start(ap, fmt);
    vsnprintf(c->errbuf, sizeof c->errbuf, fmt, ap);
    va_end(ap);
    os_mutex_unlock(&c->err_mu);
  }
  return e;
}

const char *asngn_last_error(const asngn_ctx *c) {
  return c != NULL ? c->errbuf : "";
}

asngn_err asngn_last_context_diagnostics(
    asngn_ctx *c, asngn_context_diagnostics *out) {
  if (c == NULL || out == NULL) return ASNGN_ERR_INVALID;
  os_mutex_lock(&c->err_mu);
  *out = c->context_diag;
  os_mutex_unlock(&c->err_mu);
  return ASNGN_OK;
}

/* ── memory management ────────────────────────────────────────────────── */

void asngn_free(void *p) { free(p); }

void asngn_strings_free(char **s, size_t n) {
  size_t i;
  if (s == NULL) return;
  for (i = 0; i < n; i++) free(s[i]);
  free(s);
}

void asngn_turn_result_free(asngn_turn_result *r) {
  if (r == NULL) return;
  free(r->answer);
  memset(r, 0, sizeof *r);
}

/* ── logging callback ─────────────────────────────────────────────────── */

static void default_log_cb(int level, const char *msg, void *ud) {
  (void)ud;
  if (level <= ASNGN_LOG_WARN) fprintf(stderr, "%s\n", msg);
}

void asngn_set_logger(asngn_ctx *c, asngn_log_fn fn, void *ud) {
  if (c == NULL) return;
  os_mutex_lock(&c->log_mu);
  c->log_cb = fn;
  c->log_ud = ud;
  os_mutex_unlock(&c->log_mu);
}

void asngn_set_event_sink(asngn_ctx *c, asngn_event_fn fn, void *ud) {
  if (c == NULL) return;
  os_mutex_lock(&c->tele_mu);
  c->event_cb = fn;
  c->event_ud = ud;
  os_mutex_unlock(&c->tele_mu);
}

/* ── budget pressure ─────────────────────────────────────────────────── */

double asngn_pressure(asngn_ctx *c, asngn_session *s) {
  double p = 0.0;
  if (c->cfg.session_tokens > 0 && s != NULL) {
    double ps = (double)s->spent_tokens / (double)c->cfg.session_tokens;
    if (ps > p) p = ps;
  }
  if (c->cfg.daily_tokens > 0) {
    double pd;
    os_rwlock_rdlock(&c->lock);
    pd = (double)c->daily_spent / (double)c->cfg.daily_tokens;
    os_rwlock_rdunlock(&c->lock);
    if (pd > p) p = pd;
  }
  return p;
}

/* ── open / close ─────────────────────────────────────────────────────── */

static void ctx_locks_init(asngn_ctx *c) {
  os_rwlock_init(&c->lock);
  os_mutex_init(&c->err_mu);
  os_mutex_init(&c->log_mu);
  os_mutex_init(&c->tele_mu);
  os_mutex_init(&c->models_mu);
  os_mutex_init(&c->sib_mu);
  os_rwlock_init(&c->cache_mu);
  os_mutex_init(&c->q_mu);
  os_cond_init(&c->q_cv);
  os_mutex_init(&c->confirm.mu);
  os_cond_init(&c->confirm.cv);
}

static void ctx_locks_destroy(asngn_ctx *c) {
  os_cond_destroy(&c->confirm.cv);
  os_mutex_destroy(&c->confirm.mu);
  os_cond_destroy(&c->q_cv);
  os_mutex_destroy(&c->q_mu);
  os_rwlock_destroy(&c->cache_mu);
  os_mutex_destroy(&c->sib_mu);
  os_mutex_destroy(&c->models_mu);
  os_mutex_destroy(&c->tele_mu);
  os_mutex_destroy(&c->log_mu);
  os_mutex_destroy(&c->err_mu);
  os_rwlock_destroy(&c->lock);
}

/* Defined with the turn machinery below; used by pin/compact too. */
static void session_wait_fold_locked(asngn_ctx *c, asngn_session *s);
static asngn_err startup_readiness(asngn_ctx *c);

/* Join a config path to the engine root when relative. Returns an owned
 * string replacing *slot. */
static asngn_err root_join(asngn_ctx *c, char **slot) {
  char *joined;
  if (*slot == NULL || os_path_is_abs(*slot))
    return ASNGN_OK;
  joined = os_path_join(c->root, *slot);
  if (joined == NULL) return ASNGN_ERR_NOMEM;
  free(*slot);
  *slot = joined;
  return ASNGN_OK;
}

asngn_err asngn_open_with(const asngn_open_params *p,
                          const asngn_model_iface *fakes, size_t fakes_n,
                          const char *const *fake_ids,
                          const asngn_clock *clk, asngn_ctx **out) {
  asngn_ctx *c;
  asngn_err e;
  const char *root_in;
  size_t i;

  if (out == NULL || p == NULL) return ASNGN_ERR_INVALID;
  *out = NULL;
  c = calloc(1, sizeof *c);
  if (c == NULL) return ASNGN_ERR_NOMEM;
  ctx_locks_init(c);
  c->clock = clk != NULL ? *clk : asngn_clock_system();
  c->log_cb = default_log_cb;
  c->daily_day = asngn_clock_now(&c->clock) / 86400;
  c->allow_degraded = p->allow_degraded != 0;

  asngn_config_defaults(&c->cfg);
  {
    /* layout: <engine_root>/config.xcdn is discovered when no
     * explicit config path is given */
    const char *cfg_path = p->config_path;
    char *discovered = NULL;
    if (cfg_path == NULL) {
      const char *base = p->engine_root != NULL ? p->engine_root : ".";
      discovered = os_path_join(base, "config.xcdn");
      if (discovered != NULL && os_file_exists(discovered))
        cfg_path = discovered;
    }
    e = asngn_config_load(c, &c->cfg, cfg_path);
    free(discovered);
  }
  if (e != ASNGN_OK) goto fail;

  /* engine root: open param wins over engine.root */
  root_in = p->engine_root != NULL ? p->engine_root : c->cfg.root;
  e = os_mkdir_p(root_in);
  if (e != ASNGN_OK) {
    e = asngn_seterr(c, e, "engine root %s: cannot create", root_in);
    goto fail;
  }

  c->root = os_realpath(root_in);
  if (c->root == NULL) {
    e = asngn_seterr(c, ASNGN_ERR_IO, "engine root %s: cannot resolve",
                     root_in);
    goto fail;
  }
  e = asngn_workspace_init(c, p);
  if (e != ASNGN_OK) goto fail;
  c->sessions_dir = os_path_join(c->root, "sessions");
  c->cache_dir = os_path_join(c->root, "cache");
  c->tele_dir = os_path_join(c->root, "telemetry");
  if (c->sessions_dir == NULL || c->cache_dir == NULL ||
      c->tele_dir == NULL) {
    e = ASNGN_ERR_NOMEM;
    goto fail;
  }
  e = os_mkdir_p(c->sessions_dir);
  if (e == ASNGN_OK) e = os_mkdir_p(c->cache_dir);
  if (e == ASNGN_OK) e = os_mkdir_p(c->tele_dir);
  if (e != ASNGN_OK) {
    e = asngn_seterr(c, e, "engine root %s: cannot create layout",
                     c->root);
    goto fail;
  }

  /* file sinks: relative paths resolve under the engine root */
  e = root_join(c, &c->cfg.log_path);
  if (e != ASNGN_OK) goto fail;
  e = asngn_log_open(c);
  if (e != ASNGN_OK) goto fail;
  e = asngn_tele_init(c);
  if (e != ASNGN_OK) goto fail;

  /* model weights: relative paths resolve under the engine root, like
   * every other engine-root artifact (the process cwd is arbitrary) */
  for (i = 0; i < c->cfg.pool_n; i++) {
    e = root_join(c, &c->cfg.pool[i].path);
    if (e != ASNGN_OK) goto fail;
  }

  /* model pool, with injected fakes taking their slots up front */
  for (i = 0; i < c->cfg.pool_n; i++) {
    size_t f;
    for (f = 0; f < fakes_n; f++) {
      if (fake_ids != NULL && fake_ids[f] != NULL &&
          strcmp(fake_ids[f], c->cfg.pool[i].id) == 0) {
        c->models[i].iface = fakes[f];
        c->models[i].injected = true;
        c->models[i].loaded = true;
      }
    }
  }
  e = asngn_models_init(c);
  if (e != ASNGN_OK) goto fail;

  /* siblings (degrade on failure) */
  e = asngn_siblings_open(c);
  if (e != ASNGN_OK) goto fail;
  e = startup_readiness(c);
  if (e != ASNGN_OK) goto fail;

  /* caches */
  e = asngn_cache_init(c);
  if (e != ASNGN_OK) goto fail;
  e = asngn_toolcache_init(c);
  if (e != ASNGN_OK) goto fail;

  /* workers */
  e = asngn_workers_start(c);
  if (e != ASNGN_OK) goto fail;

  /* threaded builds preload the role models in the background so the
   * first turn starts hot; ASNGN_NO_THREADS keeps the lazy loads */
  if (!c->no_threads) {
    os_mutex_lock(&c->q_mu);
    c->bg_due_warm = true;
    os_mutex_unlock(&c->q_mu);
    asngn_background_kick(c);
  }

  asngn_log(c, ASNGN_LOG_INFO, "session", "asngn %s open at %s",
            asngn_version(), c->root);
  *out = c;
  return ASNGN_OK;

fail:
  asngn_close(c);
  return e;
}

asngn_err asngn_open(const asngn_open_params *p, asngn_ctx **out) {
  return asngn_open_with(p, NULL, 0, NULL, NULL, out);
}

asngn_err asngn_workspace_get(asngn_ctx *c, asngn_workspace_info *out) {
  asngn_err e;
  if (c == NULL || out == NULL) return ASNGN_ERR_INVALID;
  e = asngn_workspace_refresh(c);
  if (e == ASNGN_OK) *out = c->workspace;
  return e;
}

void asngn_close(asngn_ctx *c) {
  size_t i;
  if (c == NULL) return;
  asngn_workers_stop(c);
  /* close any sessions the host leaked */
  while (c->sessions_n > 0) {
    asngn_session *s = c->sessions[c->sessions_n - 1];
    c->sessions_n--;
    asngn_session_save_manifest(s);
    asngn_session_free(s);
  }
  free(c->sessions);
  c->sessions = NULL;
  asngn_cache_shutdown(c);
  asngn_toolcache_shutdown(c);
  asngn_siblings_close(c);
  asngn_models_shutdown(c);
  asngn_tele_shutdown(c);
  asngn_log_close(c);
  asngn_config_free(&c->cfg);
  free(c->root);
  free(c->sessions_dir);
  free(c->cache_dir);
  free(c->tele_dir);
  for (i = 0; i < c->notes_n; i++) { /* notes cache freed in siblings */ }
  ctx_locks_destroy(c);
  free(c);
}

/* ── sessions ─────────────────────────────────────────────────────────── */

const char *asngn_session_slug(const asngn_session *s) {
  return s != NULL ? s->slug : NULL;
}

asngn_err asngn_session_workspace(asngn_session *s,
                                  asngn_workspace_info *out) {
  asngn_err e;
  if (s == NULL || out == NULL) return ASNGN_ERR_INVALID;
  e = asngn_workspace_refresh(s->ctx);
  if (e != ASNGN_OK) return e;
  os_rwlock_wrlock(&s->lock);
  s->workspace = s->ctx->workspace;
  *out = s->workspace;
  os_rwlock_wrunlock(&s->lock);
  return ASNGN_OK;
}

static asngn_err startup_readiness(asngn_ctx *c) {
  bool seen[ASNGN_MAX_POOL] = {false};
  int r;
  if (c->cfg.profile != ASNGN_PROFILE_CODING) return ASNGN_OK;
  if (c->workspace.canonical_root[0] == '\0')
    return asngn_seterr(c, ASNGN_ERR_CONFIG,
                        "coding readiness failed: workspace missing");
  for (r = 0; r < ASNGN_ROLE_COUNT; r++) {
    int slot = c->role_slot[r];
    asngn_model_slot *m;
    if (slot < 0 || (size_t)slot >= c->models_n)
      return asngn_seterr(c, ASNGN_ERR_MODEL,
                          "coding readiness failed: role %s is unmapped",
                          asngn_role_name((asngn_role)r));
    if (seen[slot]) continue;
    seen[slot] = true;
    m = &c->models[slot];
    if (!m->injected && (m->cfg.path == NULL || !os_file_exists(m->cfg.path))) {
      if (c->allow_degraded) {
        asngn_log(c, ASNGN_LOG_WARN, "model",
                  "coding readiness degraded: model '%s' missing at %s",
                  m->cfg.id, m->cfg.path != NULL ? m->cfg.path : "(null)");
      } else {
        return asngn_seterr(c, ASNGN_ERR_MODEL,
                            "coding readiness failed: model '%s' missing "
                            "at %s; pass --allow-degraded to opt in",
                            m->cfg.id,
                            m->cfg.path != NULL ? m->cfg.path : "(null)");
      }
    }
  }
  return asngn_siblings_readiness(c);
}

static int session_slug_cmp(const void *a, const void *b) {
  const char *const *sa = a;
  const char *const *sb = b;
  return strcmp(*sa, *sb);
}

asngn_err asngn_session_open(asngn_ctx *c, const char *slug,
                             asngn_session **out) {
  asngn_session *s = NULL;
  asngn_err e;
  char gen[16];
  size_t i;

  if (c == NULL || out == NULL) return ASNGN_ERR_INVALID;
  *out = NULL;
  if (slug == NULL) {
    /* generated slug: s-<8 hex of a uuid> */
    char id[37];
    asngn_uuid_v4(id);
    snprintf(gen, sizeof gen, "s-%.8s", id);
    slug = gen;
  }
  os_rwlock_wrlock(&c->lock);
  for (i = 0; i < c->sessions_n; i++) {
    if (strcmp(c->sessions[i]->slug, slug) == 0) {
      os_rwlock_wrunlock(&c->lock);
      return asngn_seterr(c, ASNGN_ERR_BUSY,
                          "session %s is already open", slug);
    }
  }
  os_rwlock_wrunlock(&c->lock);

  e = asngn_session_load(c, slug, &s);
  if (e != ASNGN_OK) return e;

  os_rwlock_wrlock(&c->lock);
  if (c->sessions_n == c->sessions_cap) {
    size_t cap = c->sessions_cap != 0 ? c->sessions_cap * 2 : 4;
    asngn_session **ns = realloc(c->sessions, cap * sizeof *ns);
    if (ns == NULL) {
      os_rwlock_wrunlock(&c->lock);
      asngn_session_free(s);
      return ASNGN_ERR_NOMEM;
    }
    c->sessions = ns;
    c->sessions_cap = cap;
  }
  c->sessions[c->sessions_n++] = s;
  os_rwlock_wrunlock(&c->lock);
  /* the session's saved project follows it into Asper (NULL deselects);
   * a failure only degrades the memory zone, never the open */
  (void)asngn_siblings_project_sync(c, s->project);
  *out = s;
  return ASNGN_OK;
}

void asngn_session_close(asngn_session *s) {
  asngn_ctx *c;
  size_t i;
  bool busy;
  if (s == NULL) return;
  c = s->ctx;
  /* a running turn is cancelled and drained before the free */
  os_rwlock_wrlock(&s->lock);
  busy = s->busy;
  if (busy && s->running != NULL) asngn_task_cancel(s->running);
  os_rwlock_wrunlock(&s->lock);
  while (busy) {
    os_sleep_ms(10);
    os_rwlock_rdlock(&s->lock);
    busy = s->busy;
    os_rwlock_rdunlock(&s->lock);
  }
  /* drop any pending background fold that borrows this session */
  os_mutex_lock(&c->q_mu);
  if (c->bg_fold_session == s) {
    c->bg_fold_session = NULL;
    c->bg_due_fold = false;
  }
  os_mutex_unlock(&c->q_mu);
  os_rwlock_wrlock(&c->lock);
  for (i = 0; i < c->sessions_n; i++) {
    if (c->sessions[i] == s) {
      memmove(&c->sessions[i], &c->sessions[i + 1],
              (c->sessions_n - i - 1) * sizeof *c->sessions);
      c->sessions_n--;
      break;
    }
  }
  os_rwlock_wrunlock(&c->lock);
  asngn_session_save_manifest(s);
  asngn_session_free(s);
}

/* Remove every regular file in dir, then dir itself (empty-dir remove:
 * unexpected nested directories fail the rmdir and surface as IO). */
static asngn_err session_rm_dir(const char *dir) {
  char **files = NULL;
  size_t n = 0, i;
  asngn_err e = os_list_dir(dir, &files, &n);
  if (e != ASNGN_OK) return e;
  for (i = 0; i < n; i++) {
    if (e == ASNGN_OK) {
      char *full = os_path_join(dir, files[i]);
      if (full == NULL) e = ASNGN_ERR_NOMEM;
      else {
        asngn_err re = os_remove_file(full);
        if (re != ASNGN_OK && re != ASNGN_ERR_NOT_FOUND) e = re;
        free(full);
      }
    }
    free(files[i]);
  }
  free(files);
  if (e != ASNGN_OK) return e;
  e = os_remove_dir(dir);
  return e == ASNGN_ERR_NOT_FOUND ? ASNGN_OK : e;
}

asngn_err asngn_session_delete(asngn_ctx *c, const char *slug) {
  char *dir = NULL, *blobs = NULL;
  size_t i;
  asngn_err e;
  if (c == NULL || slug == NULL) return ASNGN_ERR_INVALID;
  if (!asngn_slug_valid(slug))
    return asngn_seterr(c, ASNGN_ERR_INVALID, "invalid slug %s", slug);
  os_rwlock_rdlock(&c->lock);
  for (i = 0; i < c->sessions_n; i++) {
    if (strcmp(c->sessions[i]->slug, slug) == 0) {
      os_rwlock_rdunlock(&c->lock);
      return asngn_seterr(c, ASNGN_ERR_BUSY,
                          "session %s is open; close it first", slug);
    }
  }
  os_rwlock_rdunlock(&c->lock);

  dir = os_path_join(c->sessions_dir, slug);
  blobs = dir != NULL ? os_path_join(dir, "blobs") : NULL;
  if (dir == NULL || blobs == NULL) {
    free(dir);
    free(blobs);
    return ASNGN_ERR_NOMEM;
  }
  {
    char *real = os_realpath(dir);
    if (real == NULL) {
      free(blobs);
      free(dir);
      return asngn_seterr(c, ASNGN_ERR_NOT_FOUND, "no session %s", slug);
    }
    free(real);
  }
  e = session_rm_dir(blobs);
  if (e == ASNGN_OK || e == ASNGN_ERR_NOT_FOUND) e = session_rm_dir(dir);
  free(blobs);
  free(dir);
  if (e != ASNGN_OK)
    return asngn_seterr(c, e, "cannot delete session %s", slug);
  asngn_log(c, ASNGN_LOG_INFO, "session", "deleted %s", slug);
  return ASNGN_OK;
}

asngn_err asngn_session_peek(asngn_ctx *c, const char *slug,
                             asngn_session_peek_info *out) {
  asngn_session *s = NULL;
  char *dir, *real;
  asngn_err e;
  if (c == NULL || slug == NULL || out == NULL) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof *out);
  if (!asngn_slug_valid(slug))
    return asngn_seterr(c, ASNGN_ERR_INVALID, "invalid slug %s", slug);
  /* loading creates a missing session; a peek must not */
  dir = os_path_join(c->sessions_dir, slug);
  if (dir == NULL) return ASNGN_ERR_NOMEM;
  real = os_realpath(dir);
  free(dir);
  if (real == NULL)
    return asngn_seterr(c, ASNGN_ERR_NOT_FOUND, "no session %s", slug);
  free(real);
  e = asngn_session_load(c, slug, &s);
  if (e != ASNGN_OK) return e;
  out->turns = s->led_n;
  out->spent_tokens = s->spent_tokens;
  out->created_at = (long long)s->created_at;
  if (s->led_n > 0)
    out->last_turn_at = (long long)s->led[s->led_n - 1].at;
  if (s->project != NULL)
    snprintf(out->project, sizeof out->project, "%s", s->project);
  asngn_session_free(s);
  return ASNGN_OK;
}

asngn_err asngn_session_list(asngn_ctx *c, char ***out_slugs,
                             size_t *out_n) {
  char **names = NULL;
  size_t n = 0, i, kept = 0;
  asngn_err e;
  if (c == NULL || out_slugs == NULL || out_n == NULL)
    return ASNGN_ERR_INVALID;
  *out_slugs = NULL;
  *out_n = 0;
  e = os_list_dirs(c->sessions_dir, &names, &n);
  if (e != ASNGN_OK)
    return asngn_seterr(c, e, "cannot list %s", c->sessions_dir);
  for (i = 0; i < n; i++) {
    if (asngn_slug_valid(names[i])) {
      names[kept++] = names[i];
    } else {
      free(names[i]);
    }
  }
  /* sort for determinism without quadratic growth on large stores */
  if (kept > 1) qsort(names, kept, sizeof *names, session_slug_cmp);
  *out_slugs = names;
  *out_n = kept;
  return ASNGN_OK;
}

asngn_err asngn_session_pin(asngn_session *s, size_t turn, int on) {
  asngn_ctx *c;
  size_t i, pinned = 0;
  bool found = false, changed = false;
  if (s == NULL) return ASNGN_ERR_INVALID;
  c = s->ctx;
  session_wait_fold_locked(c, s);
  if (s->busy) {
    os_rwlock_wrunlock(&s->lock);
    return asngn_seterr(c, ASNGN_ERR_BUSY,
                        "session %s: a turn is running", s->slug);
  }
  for (i = 0; i < s->log_n; i++)
    if (s->log[i].pinned) pinned++;
  for (i = 0; i < s->log_n; i++) {
    if (s->log[i].n == turn) {
      found = true;
      if (on && !s->log[i].pinned) {
        if (pinned >= (size_t)c->cfg.pinned_max) {
          os_rwlock_wrunlock(&s->lock);
          return asngn_seterr(c, ASNGN_ERR_INVALID,
                              "pin limit reached (%d); unpin first",
                              c->cfg.pinned_max);
        }
        s->log[i].pinned = true;
        s->log[i].folded = false; /* pinned turns rejoin the verbatim */
        changed = true;
      } else if (!on) {
        if (s->log[i].pinned) {
          s->log[i].pinned = false;
          changed = true;
        }
      }
      break;
    }
  }
  if (!found) {
    os_rwlock_wrunlock(&s->lock);
    return asngn_seterr(c, ASNGN_ERR_NOT_FOUND, "no turn %zu", turn);
  }
  if (!changed) {
    os_rwlock_wrunlock(&s->lock);
    return ASNGN_OK;
  }
  {
    /* the lock stays held across the rewrite so a submitted turn cannot
     * append into the closed-and-replaced transcript stream */
    asngn_err e = asngn_session_rewrite_transcript(s);
    if (e == ASNGN_OK) e = asngn_session_save_manifest(s);
    os_rwlock_wrunlock(&s->lock);
    return e;
  }
}

asngn_err asngn_session_compact(asngn_session *s) {
  asngn_ctx *c;
  size_t aux = 0;
  asngn_err e;
  if (s == NULL) return ASNGN_ERR_INVALID;
  c = s->ctx;
  /* claim the session like a turn: fold_run takes its own short holds
   * and must not run under a caller-held s->lock (the compressor call
   * would block every session API for minutes) */
  session_wait_fold_locked(c, s);
  if (s->busy) {
    os_rwlock_wrunlock(&s->lock);
    return asngn_seterr(c, ASNGN_ERR_BUSY,
                        "session %s: a turn is running", s->slug);
  }
  s->busy = true;
  os_rwlock_wrunlock(&s->lock);
  /* a cancel raised while waiting out a background fold must not abort
   * this deliberate one (shutdown keeps its flag) */
  os_mutex_lock(&c->q_mu);
  if (!c->stop) c->bg_cancel = 0;
  os_mutex_unlock(&c->q_mu);
  e = asngn_fold_run(c, s, true, &aux);
  os_rwlock_wrlock(&s->lock);
  s->busy = false;
  os_rwlock_wrunlock(&s->lock);
  return e;
}

/* ── stats ────────────────────────────────────────────────────────────── */

asngn_err asngn_get_stats(asngn_ctx *c, asngn_stats *out) {
  if (c == NULL || out == NULL) return ASNGN_ERR_INVALID;
  os_rwlock_rdlock(&c->lock);
  *out = c->stats;
  os_rwlock_rdunlock(&c->lock);
  return ASNGN_OK;
}

/* ── caches ───────────────────────────────────────────────────────────── */

asngn_err asngn_cache_clear(asngn_ctx *c, const char *scope) {
  if (c == NULL) return ASNGN_ERR_INVALID;
  if (scope != NULL && strcmp(scope, "session") != 0 &&
      strcmp(scope, "global") != 0)
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "cache scope must be \"session\", \"global\" or "
                        "null");
  return asngn_cache_clear_scope(c, scope);
}

/* ── feedback and continuation ────────────────────────────────────────── */

asngn_err asngn_feedback(asngn_session *s, size_t turn, int signal) {
  asngn_err e;
  if (s == NULL) return ASNGN_ERR_INVALID;
  os_rwlock_wrlock(&s->lock);
  e = asngn_ledger_set_feedback(s, turn, signal);
  os_rwlock_wrunlock(&s->lock);
  if (e == ASNGN_OK) {
    os_rwlock_wrlock(&s->ctx->lock);
    s->ctx->stats.qpt_rolling = asngn_session_qpt(s);
    os_rwlock_wrunlock(&s->ctx->lock);
  }
  return e;
}

/* ── confirmations ───────────────────────────────────────────────────── */

asngn_err asngn_confirm(asngn_ctx *c, const char *confirm_id, int allow,
                        int session_wide) {
  if (c == NULL || confirm_id == NULL) return ASNGN_ERR_INVALID;
  os_mutex_lock(&c->confirm.mu);
  if (c->confirm.id[0] == '\0' ||
      strcmp(c->confirm.id, confirm_id) != 0) {
    os_mutex_unlock(&c->confirm.mu);
    return asngn_seterr(c, ASNGN_ERR_NOT_FOUND,
                        "no pending confirmation %s", confirm_id);
  }
  c->confirm.decided = 1;
  c->confirm.allow = allow ? 1 : 0;
  c->confirm.session_wide = session_wide ? 1 : 0;
  os_cond_broadcast(&c->confirm.cv);
  os_mutex_unlock(&c->confirm.mu);
  return ASNGN_OK;
}

/* ── turns ────────────────────────────────────────────────────────────── */

void asngn_turn_state_free(asngn_turn_state *t) {
  size_t i;
  if (t == NULL) return;
  free(t->user_msg);
  for (i = 0; i < t->work_n; i++) free(t->work[i].text);
  free(t->work);
  free(t->memory_block);
  free(t->catalog);
  free(t->astools_gbnf);
  free(t->call_keys);
  for (i = 0; i < t->tools_list_n; i++) free(t->tools_list[i]);
  free(t->tools_list);
  free(t->answer);
  memset(t, 0, sizeof *t);
}

/* Wait out a background fold and return with s->lock write-held and no
 * fold running. A user action outranks housekeeping: a fast fold gets a
 * short grace, then it is asked to step aside (it aborts the compressor
 * call, keeps the pairs already folded, and re-queues itself). */
static void session_wait_fold_locked(asngn_ctx *c, asngn_session *s) {
  int grace = 2;
  os_rwlock_wrlock(&s->lock);
  while (s->busy && s->bg_folding) {
    os_rwlock_wrunlock(&s->lock);
    if (grace > 0) grace--;
    else c->bg_cancel = 1;
    os_mutex_lock(&c->q_mu);
    os_cond_timedwait(&c->q_cv, &c->q_mu, 20); /* fold end wakes us */
    os_mutex_unlock(&c->q_mu);
    os_rwlock_wrlock(&s->lock);
  }
}

static asngn_err submit_common(asngn_session *s, char *gated_msg,
                               const asngn_submit_opts *opts,
                               bool continuation, bool retry_up,
                               asngn_token_fn token_fn,
                               void *ud, asngn_task **out) {
  asngn_ctx *c = s->ctx;
  asngn_task *task;
  asngn_turn_state *t;

  task = calloc(1, sizeof *task);
  t = calloc(1, sizeof *t);
  if (task == NULL || t == NULL) {
    free(task);
    free(t);
    free(gated_msg);
    return ASNGN_ERR_NOMEM;
  }
  t->s = s;
  t->user_msg = gated_msg;
  if (opts != NULL) t->opts = *opts;
  t->continuation = continuation;
  t->retry_up = retry_up;
  t->token_cb = token_fn;
  t->token_ud = ud;
  t->gen_slot = -1;
  os_mutex_init(&task->mu);
  os_cond_init(&task->cv);
  task->ctx = c;
  task->session = s;
  task->turn = t;

  session_wait_fold_locked(c, s);
  if (s->busy) {
    os_rwlock_wrunlock(&s->lock);
    os_cond_destroy(&task->cv);
    os_mutex_destroy(&task->mu);
    asngn_turn_state_free(t);
    free(t);
    free(task);
    return asngn_seterr(c, ASNGN_ERR_BUSY,
                        "session %s: a turn is already running", s->slug);
  }
  s->busy = true;
  s->running = task;
  os_rwlock_wrunlock(&s->lock);

  {
    asngn_err e = asngn_worker_submit(c, task);
    if (e != ASNGN_OK) {
      os_rwlock_wrlock(&s->lock);
      s->busy = false;
      s->running = NULL;
      os_rwlock_wrunlock(&s->lock);
      os_cond_destroy(&task->cv);
      os_mutex_destroy(&task->mu);
      asngn_turn_state_free(t);
      free(t);
      free(task);
      return e;
    }
  }
  *out = task;
  return ASNGN_OK;
}

asngn_err asngn_submit(asngn_session *s, const char *text_utf8,
                       const asngn_submit_opts *opts,
                       asngn_token_fn token_fn, void *ud,
                       asngn_task **out) {
  asngn_ctx *c;
  char *msg;
  size_t len;
  asngn_err e;

  if (s == NULL || text_utf8 == NULL || out == NULL)
    return ASNGN_ERR_INVALID;
  *out = NULL;
  c = s->ctx;
  len = strlen(text_utf8);
  msg = malloc(len + 1);
  if (msg == NULL) return ASNGN_ERR_NOMEM;
  memcpy(msg, text_utf8, len + 1);
  e = asngn_gate_input(c, msg, &len);
  if (e != ASNGN_OK) {
    free(msg);
    return e;
  }
  msg[len] = '\0';
  return submit_common(s, msg, opts, false, false, token_fn, ud, out);
}

asngn_err asngn_more(asngn_session *s, asngn_token_fn fn, void *ud,
                     asngn_task **out) {
  char *msg = NULL;
  bool have = false, capped = false;
  if (s == NULL || out == NULL) return ASNGN_ERR_INVALID;
  *out = NULL;
  os_rwlock_rdlock(&s->lock);
  have = s->last_answer != NULL && s->last_user_msg != NULL;
  capped = s->last_capped;
  if (have && capped) msg = asngn_strdup(s->last_user_msg);
  os_rwlock_rdunlock(&s->lock);
  if (!have)
    return asngn_seterr(s->ctx, ASNGN_ERR_NOT_FOUND,
                        "nothing to continue");
  if (!capped)
    return asngn_seterr(s->ctx, ASNGN_ERR_INVALID,
                        "the last answer was not capped");
  if (msg == NULL) return ASNGN_ERR_NOMEM;
  return submit_common(s, msg, NULL, true, false, fn, ud, out);
}

asngn_err asngn_retry(asngn_session *s, asngn_token_fn fn, void *ud,
                      asngn_task **out) {
  char *msg;
  asngn_submit_opts opts;
  if (s == NULL || out == NULL) return ASNGN_ERR_INVALID;
  *out = NULL;
  os_rwlock_rdlock(&s->lock);
  msg = s->last_user_msg != NULL ? asngn_strdup(s->last_user_msg) : NULL;
  {
    bool have = s->last_user_msg != NULL;
    os_rwlock_rdunlock(&s->lock);
    if (!have)
      return asngn_seterr(s->ctx, ASNGN_ERR_NOT_FOUND,
                          "nothing to retry");
  }
  if (msg == NULL) return ASNGN_ERR_NOMEM;
  memset(&opts, 0, sizeof opts);
  opts.no_cache = 1;
  return submit_common(s, msg, &opts, false, true, fn, ud, out);
}

asngn_err asngn_task_wait(asngn_task *t, uint32_t timeout_ms,
                          asngn_turn_result *out) {
  asngn_err verdict;
  if (t == NULL || out == NULL) return ASNGN_ERR_INVALID;
  os_mutex_lock(&t->mu);
  while (!t->done) {
    int r = os_cond_timedwait(&t->cv, &t->mu,
                              timeout_ms == 0 ? -1 : (int64_t)timeout_ms);
    if (r == 0 && !t->done) {
      os_mutex_unlock(&t->mu);
      return ASNGN_ERR_BUSY;
    }
  }
  verdict = t->verdict;
  if (!t->result_taken) {
    *out = t->result;          /* strings move out exactly once */
    memset(&t->result, 0, sizeof t->result);
    t->result_taken = true;
  } else {
    *out = t->result;          /* zeroed copy, counters only */
  }
  os_mutex_unlock(&t->mu);
  return verdict;
}

asngn_err asngn_task_cancel(asngn_task *t) {
  if (t == NULL) return ASNGN_ERR_INVALID;
  if (t->turn != NULL) t->turn->cancel = 1;
  /* wake a possible pending confirmation so the loop can observe it */
  os_mutex_lock(&t->ctx->confirm.mu);
  os_cond_broadcast(&t->ctx->confirm.cv);
  os_mutex_unlock(&t->ctx->confirm.mu);
  return ASNGN_OK;
}

void asngn_task_free(asngn_task *t) {
  if (t == NULL) return;
  /* the worker owns the task until done; wait for it */
  os_mutex_lock(&t->mu);
  while (!t->done) {
    t->turn->cancel = 1;
    os_cond_timedwait(&t->cv, &t->mu, 100);
  }
  os_mutex_unlock(&t->mu);
  if (!t->result_taken) asngn_turn_result_free(&t->result);
  if (t->turn != NULL) {
    asngn_turn_state_free(t->turn);
    free(t->turn);
  }
  os_cond_destroy(&t->cv);
  os_mutex_destroy(&t->mu);
  free(t);
}

/* ── tick (ASNGN_NO_THREADS) ──────────────────────────────────────────── */

asngn_err asngn_tick(asngn_ctx *c) {
  if (c == NULL) return ASNGN_ERR_INVALID;
#ifdef ASNGN_NO_THREADS
  return asngn_run_due_work(c);
#else
  return ASNGN_OK;
#endif
}
