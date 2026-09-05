/*
 * cache.c — the semantic answer cache.
 *
 * Storage: cache/semantic.xcdn is an append-only stream of
 * #cache_entry values plus batched #cache_touch { ids, at } hit updates,
 * compacted (rewrite + atomic replace) at close or when touch ops exceed
 * 2048 — the journal discipline of the siblings. Vectors are derived data
 * in cache/embeddings.bin (embedcache.c), attached at init and re-embedded
 * lazily at probe time when missing.
 *
 * Locking: every access to c->cache goes through c->cache_mu.
 *   - Probes take the WRITE lock: a probe may lazily re-embed entries
 *     (mutating entry->vec) and mutates hit_count/last_hit on a hit.
 *   - Plan hints and counts take the READ lock (no mutation).
 *   - The lock order permits cache_mu -> models_mu, so embedding calls
 *     are legal under the cache lock.
 *   - init and shutdown run single-threaded (the asngn_open/close
 *     contract) and touch the array without contention.
 * Appends use a transient open/append/close stream per record — inserts
 * and hits are rare (at most one per turn), so no persistent FILE* is
 * kept on the context. A failed append never loses the entry: it stays
 * resident and is persisted by the next compact.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

/* Compact when pending touch records exceed this many. */
#define ASNGN_CACHE_TOUCH_MAX 2048

/* ── small helpers ────────────────────────────────────────────────────── */

static char *cache_sem_path(asngn_ctx *c) {
  return os_path_join(c->cache_dir, "semantic.xcdn");
}

/* One-shot append of a serialized line to semantic.xcdn. */
static asngn_err cache_append_line(asngn_ctx *c, const asngn_buf *line) {
  asngn_stream st;
  char *path;
  asngn_err e;

  if (line->data == NULL || line->len == 0) return ASNGN_ERR_INVALID;
  path = cache_sem_path(c);
  if (path == NULL)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "cache: out of memory");
  e = asngn_stream_open(c, &st, path, false);
  free(path);
  if (e != ASNGN_OK) return e;
  e = asngn_stream_append(c, &st, line->data, line->len);
  asngn_stream_close(&st);
  return e;
}

static void cache_entry_free(asngn_cache_entry *e) {
  size_t i;
  if (e == NULL) return;
  free(e->session);
  free(e->project);
  free(e->query);
  free(e->answer);
  for (i = 0; i < e->tools_n; i++) free(e->tools[i]);
  free(e->tools);
  free(e->vec);
  memset(e, 0, sizeof *e);
}

static void cache_remove_at(asngn_ctx *c, size_t i) {
  cache_entry_free(&c->cache[i]);
  if (i + 1 < c->cache_n)
    memmove(&c->cache[i], &c->cache[i + 1],
            (c->cache_n - i - 1) * sizeof *c->cache);
  c->cache_n--;
}

static asngn_err cache_reserve(asngn_ctx *c) {
  if (c->cache_n < c->cache_cap) return ASNGN_OK;
  {
    size_t cap = c->cache_cap != 0 ? c->cache_cap * 2 : 32;
    asngn_cache_entry *ne = realloc(c->cache, cap * sizeof *ne);
    if (ne == NULL) return ASNGN_ERR_NOMEM;
    c->cache = ne;
    c->cache_cap = cap;
  }
  return ASNGN_OK;
}

/* Cosine over L2-normalized vectors is the plain dot product. */
static double cache_dot(const float *a, const float *b, int dim) {
  double acc = 0.0;
  int i;
  for (i = 0; i < dim; i++) acc += (double)a[i] * (double)b[i];
  return acc;
}

/* Scope, project, and TTL eligibility. Epoch and tools_used
 * rules are applied by the outcome logic, not here. */
static bool cache_eligible(const asngn_cache_entry *e, const asngn_session *s,
                           asngn_time now) {
  if (e->scope != ASNGN_SCOPE_GLOBAL) {
    if (e->session == NULL || s == NULL || strcmp(e->session, s->slug) != 0)
      return false;
  }
  if (e->project != NULL || (s != NULL && s->project != NULL)) {
    if (e->project == NULL || s == NULL || s->project == NULL ||
        strcmp(e->project, s->project) != 0)
      return false;
  }
  if (e->ttl_s > 0 && e->created_at + e->ttl_s <= now) return false;
  return true;
}

static const char *cache_outcome_name(asngn_cache_outcome o) {
  switch (o) {
    case ASNGN_CACHE_HIT: return "hit";
    case ASNGN_CACHE_ADAPT: return "adapt";
    default: return "miss";
  }
}

static void cache_emit_probe(asngn_ctx *c, const asngn_session *s,
                             asngn_cache_outcome outcome, double cos) {
  char data[96];
  snprintf(data, sizeof data, "{ outcome: \"%s\", cos: %.4f }",
           cache_outcome_name(outcome), cos);
  asngn_tele_emit(c, "cache_probe", NULL, NULL, s->slug, s->turns + 1, data);
}

/* ── #cache_entry (de)serialization ──────────────────────────────────── */

static xcdn_node_t *cache_entry_node(const asngn_cache_entry *e) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node;
  char ts[21], ttl[32];
  bool ok = obj != NULL;

  ok = ok && asngn_xobj_put(obj, "id", xcdn_value_uuid(e->id));
  ok = ok && asngn_xobj_put(
                 obj, "scope",
                 xcdn_value_string(e->scope == ASNGN_SCOPE_GLOBAL ? "global"
                                                                  : "session"));
  ok = ok && asngn_xobj_put(obj, "session",
                            e->session != NULL ? xcdn_value_string(e->session)
                                               : xcdn_value_null());
  ok = ok && asngn_xobj_put(obj, "project",
                            e->project != NULL ? xcdn_value_string(e->project)
                                               : xcdn_value_null());
  ok = ok && asngn_xobj_put(obj, "epoch", xcdn_value_int((int64_t)e->epoch));
  ok = ok && asngn_xobj_put(
                 obj, "query",
                 xcdn_value_string(e->query != NULL ? e->query : ""));
  ok = ok && asngn_xobj_put(
                 obj, "answer",
                 xcdn_value_string(e->answer != NULL ? e->answer : ""));
  ok = ok && asngn_xobj_put(obj, "detail", xcdn_value_string(e->detail));
  ok = ok && asngn_xobj_put(obj, "tier", xcdn_value_string(e->tier));
  ok = ok && asngn_xobj_put(obj, "gen_tokens",
                            xcdn_value_int((int64_t)e->gen_tokens));
  ok = ok && asngn_xobj_put(obj, "tools_used", xcdn_value_bool(e->tools_used));
  if (ok && e->tools_used) { /* tools present only when tools_used */
    xcdn_value_t *arr = xcdn_value_array();
    size_t i;
    ok = arr != NULL;
    for (i = 0; ok && i < e->tools_n; i++)
      ok = asngn_xarr_push(
          arr, xcdn_value_string(e->tools[i] != NULL ? e->tools[i] : ""));
    if (ok) {
      ok = asngn_xobj_put(obj, "tools", arr);
      arr = NULL;
    }
    if (arr != NULL) xcdn_value_free(arr);
  }
  asngn_time_format_rfc3339(e->created_at, ts);
  ok = ok && asngn_xobj_put(obj, "created_at", xcdn_value_datetime(ts));
  asngn_time_format_rfc3339(e->last_hit, ts);
  ok = ok && asngn_xobj_put(obj, "last_hit", xcdn_value_datetime(ts));
  ok = ok && asngn_xobj_put(obj, "hit_count",
                            xcdn_value_int((int64_t)e->hit_count));
  snprintf(ttl, sizeof ttl, "PT%lldS", (long long)e->ttl_s);
  ok = ok && asngn_xobj_put(obj, "ttl", xcdn_value_duration(ttl));
  if (!ok) {
    if (obj != NULL) xcdn_value_free(obj);
    return NULL;
  }
  node = xcdn_node_new(obj);
  if (node == NULL) {
    xcdn_value_free(obj);
    return NULL;
  }
  if (!asngn_xnode_tag(node, "cache_entry")) {
    xcdn_node_free(node);
    return NULL;
  }
  return node;
}

/* Tolerant parse: id, scope, query, answer and created_at are required;
 * everything else falls back to a sane default. false => caller skips
 * the value with a WARN. */
static bool cache_entry_from_node(asngn_ctx *c, const xcdn_node_t *node,
                                  asngn_cache_entry *out) {
  const xcdn_value_t *obj, *v;
  const char *sv;
  int64_t n;
  bool b;

  memset(out, 0, sizeof *out);
  if (node == NULL || node->value == NULL ||
      node->value->type != XCDN_VAL_OBJECT)
    return false;
  obj = node->value;

  if (!asngn_xuuid(asngn_xfield(obj, "id"), out->id)) return false;
  sv = asngn_xstr(asngn_xfield(obj, "scope"));
  if (sv == NULL) return false;
  if (strcmp(sv, "global") == 0) out->scope = ASNGN_SCOPE_GLOBAL;
  else if (strcmp(sv, "session") == 0) out->scope = ASNGN_SCOPE_SESSION;
  else return false;

  sv = asngn_xstr(asngn_xfield(obj, "session"));
  if (sv != NULL) {
    out->session = asngn_strdup(sv);
    if (out->session == NULL) goto fail;
  }
  sv = asngn_xstr(asngn_xfield(obj, "project"));
  if (sv != NULL) {
    out->project = asngn_strdup(sv);
    if (out->project == NULL) goto fail;
  }
  if (asngn_xint(asngn_xfield(obj, "epoch"), &n) && n >= 0)
    out->epoch = (uint64_t)n;

  sv = asngn_xstr(asngn_xfield(obj, "query"));
  if (sv == NULL) goto fail;
  out->query = asngn_strdup(sv);
  if (out->query == NULL) goto fail;
  sv = asngn_xstr(asngn_xfield(obj, "answer"));
  if (sv == NULL) goto fail;
  out->answer = asngn_strdup(sv);
  if (out->answer == NULL) goto fail;

  sv = asngn_xstr(asngn_xfield(obj, "detail"));
  snprintf(out->detail, sizeof out->detail, "%s", sv != NULL ? sv : "");
  sv = asngn_xstr(asngn_xfield(obj, "tier"));
  snprintf(out->tier, sizeof out->tier, "%s", sv != NULL ? sv : "");
  if (asngn_xint(asngn_xfield(obj, "gen_tokens"), &n) && n >= 0)
    out->gen_tokens = (size_t)n;
  if (asngn_xbool(asngn_xfield(obj, "tools_used"), &b)) out->tools_used = b;

  v = asngn_xfield(obj, "tools");
  if (out->tools_used && v != NULL && v->type == XCDN_VAL_ARRAY &&
      xcdn_array_len(v) > 0) {
    size_t len = xcdn_array_len(v), i;
    out->tools = calloc(len, sizeof *out->tools);
    if (out->tools == NULL) goto fail;
    for (i = 0; i < len; i++) {
      const xcdn_node_t *tn = xcdn_array_get(v, i);
      const char *ts = tn != NULL ? asngn_xstr(tn->value) : NULL;
      if (ts == NULL) continue; /* tolerant: non-string element skipped */
      out->tools[out->tools_n] = asngn_strdup(ts);
      if (out->tools[out->tools_n] == NULL) goto fail;
      out->tools_n++;
    }
  }

  if (!asngn_xtime(asngn_xfield(obj, "created_at"), &out->created_at))
    goto fail;
  if (!asngn_xtime(asngn_xfield(obj, "last_hit"), &out->last_hit))
    out->last_hit = out->created_at;
  if (asngn_xint(asngn_xfield(obj, "hit_count"), &n) && n >= 0)
    out->hit_count = (size_t)n;
  if (!asngn_xdur(asngn_xfield(obj, "ttl"), &out->ttl_s))
    out->ttl_s = c->cfg.cache_ttl_s;
  return true;

fail:
  cache_entry_free(out);
  return false;
}

/* ── #cache_touch records ─────────────────────────────────────────────── */

/* One touch line for a single hit: #cache_touch { ids: [u"..."], at }. */
static asngn_err cache_touch_line(const char *id, asngn_time at,
                                  asngn_buf *out) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_value_t *ids = xcdn_value_array();
  xcdn_node_t *node = NULL;
  char ts[21];
  asngn_err e = ASNGN_ERR_NOMEM;
  bool ok = obj != NULL && ids != NULL;

  ok = ok && asngn_xarr_push(ids, xcdn_value_uuid(id));
  if (ok) {
    ok = asngn_xobj_put(obj, "ids", ids);
    ids = NULL;
  }
  asngn_time_format_rfc3339(at, ts);
  ok = ok && asngn_xobj_put(obj, "at", xcdn_value_datetime(ts));
  if (ok) {
    node = xcdn_node_new(obj);
    if (node != NULL) {
      obj = NULL;
      ok = asngn_xnode_tag(node, "cache_touch");
    } else ok = false;
  }
  if (ok) e = asngn_xnode_write(node, false, out);
  if (node != NULL) xcdn_node_free(node);
  if (obj != NULL) xcdn_value_free(obj);
  if (ids != NULL) xcdn_value_free(ids);
  return e;
}

/* Replay one touch record onto the resident array (init only). */
static void cache_touch_apply(asngn_ctx *c, const xcdn_node_t *node) {
  const xcdn_value_t *obj, *ids;
  asngn_time at;
  size_t i, j, len;

  if (node == NULL || node->value == NULL ||
      node->value->type != XCDN_VAL_OBJECT)
    return;
  obj = node->value;
  if (!asngn_xtime(asngn_xfield(obj, "at"), &at)) return;
  ids = asngn_xfield(obj, "ids");
  if (ids == NULL || ids->type != XCDN_VAL_ARRAY) return;
  len = xcdn_array_len(ids);
  for (i = 0; i < len; i++) {
    const xcdn_node_t *idn = xcdn_array_get(ids, i);
    char id[37];
    if (idn == NULL || !asngn_xuuid(idn->value, id)) continue;
    for (j = 0; j < c->cache_n; j++) {
      if (strcmp(c->cache[j].id, id) == 0) {
        c->cache[j].last_hit = at;
        c->cache[j].hit_count++;
        break;
      }
    }
  }
}

/* ── compaction ──────────────────────────────────────────────────────── */

/* Rewrite semantic.xcdn atomically from the resident array (one compact
 * value per line), reset the touch counters, then persist the vectors.
 * Caller holds cache_mu (write) or is single-threaded. */
static asngn_err cache_compact_locked(asngn_ctx *c) {
  asngn_buf out;
  char *path;
  size_t i;
  asngn_err e = ASNGN_OK;

  asngn_buf_init(&out);
  for (i = 0; e == ASNGN_OK && i < c->cache_n; i++) {
    xcdn_node_t *node = cache_entry_node(&c->cache[i]);
    if (node == NULL) {
      e = ASNGN_ERR_NOMEM;
      break;
    }
    e = asngn_xnode_write(node, false, &out);
    xcdn_node_free(node);
    if (e == ASNGN_OK) e = asngn_buf_appendc(&out, '\n');
  }
  if (e == ASNGN_OK) {
    path = cache_sem_path(c);
    if (path == NULL) e = ASNGN_ERR_NOMEM;
    else {
      e = asngn_write_atomic(c, path, out.data != NULL ? out.data : "",
                             out.len);
      free(path);
    }
  }
  asngn_buf_free(&out);
  if (e != ASNGN_OK) {
    asngn_log(c, ASNGN_LOG_WARN, "cache", "semantic cache compact failed");
    if (e == ASNGN_ERR_NOMEM)
      return asngn_seterr(c, e, "cache: out of memory compacting");
    return e;
  }
  c->cache_touch_pending = 0;
  for (i = 0; i < c->cache_n; i++) c->cache[i].touched = false;
  return asngn_embedcache_save(c);
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

asngn_err asngn_cache_init(asngn_ctx *c) {
  char *path;
  struct xcdn_document *docp = NULL;
  xcdn_document_t *doc;
  asngn_err e;
  size_t i, dropped = 0;
  asngn_time now;

  if (c == NULL || c->cache_dir == NULL) return ASNGN_ERR_INVALID;
  if (!c->cfg.cache_enable) return ASNGN_OK;
  (void)os_mkdir_p(c->cache_dir);
  path = cache_sem_path(c);
  if (path == NULL)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "cache: out of memory");
  e = asngn_stream_load(c, path, "semantic cache", &docp);
  free(path);
  if (e != ASNGN_OK) return e;
  doc = (xcdn_document_t *)docp;

  if (doc != NULL) {
    for (i = 0; i < doc->values_len; i++) {
      const xcdn_node_t *node = doc->values[i];
      if (xcdn_node_has_tag(node, "cache_entry")) {
        asngn_cache_entry ent;
        if (!cache_entry_from_node(c, node, &ent)) {
          asngn_log(c, ASNGN_LOG_WARN, "cache",
                    "semantic cache value %zu invalid; skipped", i);
          continue;
        }
        e = cache_reserve(c);
        if (e != ASNGN_OK) {
          cache_entry_free(&ent);
          xcdn_document_free(doc);
          return asngn_seterr(c, e, "cache: out of memory loading");
        }
        c->cache[c->cache_n++] = ent;
      } else if (xcdn_node_has_tag(node, "cache_touch")) {
        cache_touch_apply(c, node);
      } else {
        asngn_log(c, ASNGN_LOG_WARN, "cache",
                  "semantic cache value %zu has an unknown tag; skipped", i);
      }
    }
    xcdn_document_free(doc);
  }

  /* TTL sweep at open. */
  now = asngn_clock_now(&c->clock);
  for (i = c->cache_n; i-- > 0;) {
    const asngn_cache_entry *ent = &c->cache[i];
    if (ent->ttl_s > 0 && ent->created_at + ent->ttl_s <= now) {
      cache_remove_at(c, i);
      dropped++;
    }
  }
  if (dropped > 0)
    asngn_log(c, ASNGN_LOG_INFO, "cache",
              "semantic cache: %zu expired entr%s dropped at open", dropped,
              dropped == 1 ? "y" : "ies");
  asngn_log(c, ASNGN_LOG_INFO, "cache", "semantic cache: %zu entr%s loaded",
            c->cache_n, c->cache_n == 1 ? "y" : "ies");

  /* Attach vectors; entries left without one re-embed lazily at probe. */
  return asngn_embedcache_load(c);
}

void asngn_cache_shutdown(asngn_ctx *c) {
  size_t i;
  if (c == NULL) return;
  if (c->cfg.cache_enable) {
    (void)asngn_cache_compact(c);   /* also saves the vectors */
    (void)asngn_embedcache_save(c); /* fallback if compact failed */
  }
  for (i = 0; i < c->cache_n; i++) cache_entry_free(&c->cache[i]);
  free(c->cache);
  c->cache = NULL;
  c->cache_n = c->cache_cap = 0;
  c->cache_touch_pending = 0;
}

/* ── probe ───────────────────────────────────────────────────────────── */

asngn_err asngn_cache_probe(asngn_ctx *c, asngn_session *s, const char *query,
                            double adapt_bias, asngn_cache_probe_result *out) {
  if (c->owner) c=c->owner;
  int dim;
  float *vec;
  asngn_time now;
  asngn_err e;
  asngn_cache_entry *best = NULL;
  double best_cos = 0.0;
  asngn_buf touch;
  bool have_touch = false;
  size_t i;

  if (c == NULL || s == NULL || query == NULL || out == NULL)
    return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof *out);
  out->outcome = ASNGN_CACHE_MISS;
  if (!c->cfg.cache_enable) return ASNGN_OK;

  dim = asngn_models_embed_dim(c);
  if (dim <= 0) {
    asngn_log(c, ASNGN_LOG_WARN, "cache",
              "probe: no embedder available; treating as miss");
    cache_emit_probe(c, s, ASNGN_CACHE_MISS, 0.0);
    return ASNGN_OK;
  }
  c->embed_dim = dim;
  vec = malloc((size_t)dim * sizeof *vec);
  if (vec == NULL)
    return asngn_seterr(c, ASNGN_ERR_NOMEM,
                        "cache: out of memory embedding query");
  e = asngn_models_embed(c, query, vec);
  if (e != ASNGN_OK) {
    free(vec);
    asngn_log(c, ASNGN_LOG_WARN, "cache",
              "probe: query embedding failed; treating as miss");
    cache_emit_probe(c, s, ASNGN_CACHE_MISS, 0.0);
    return ASNGN_OK;
  }

  now = asngn_clock_now(&c->clock);
  asngn_buf_init(&touch);

  os_rwlock_wrlock(&c->cache_mu);
  for (i = 0; i < c->cache_n; i++) {
    asngn_cache_entry *ent = &c->cache[i];
    double cos;
    if (!cache_eligible(ent, s, now)) continue;
    if (ent->vec == NULL) { /* lazy re-embed (vectors are derived data) */
      float *ev = malloc((size_t)dim * sizeof *ev);
      if (ev == NULL) continue;
      if (asngn_models_embed(c, ent->query, ev) != ASNGN_OK) {
        free(ev);
        continue;
      }
      ent->vec = ev;
    }
    if (ent->tools_used) continue; /* plan hints only */
    cos = cache_dot(vec, ent->vec, dim);
    if (best == NULL || cos > best_cos) {
      best = ent;
      best_cos = cos;
    }
  }

  if (best != NULL && best_cos >= c->cfg.hit_threshold &&
      best->epoch == s->world_epoch) {
    out->outcome = ASNGN_CACHE_HIT;
    best->hit_count++;
    best->last_hit = now;
    best->touched = true;
    c->cache_touch_pending++;
    if (c->cache_touch_pending > ASNGN_CACHE_TOUCH_MAX) {
      /* Folds the pending touches into the rewrite; no record needed. */
      (void)cache_compact_locked(c);
    } else if (cache_touch_line(best->id, now, &touch) == ASNGN_OK) {
      have_touch = true;
    }
  } else if (best != NULL &&
             best_cos >= c->cfg.adapt_threshold - adapt_bias) {
    out->outcome = ASNGN_CACHE_ADAPT;
  }
  /* copy the matched entry out while still under cache_mu: the borrowed
   * pointer would dangle across sweeps/clears once the lock drops */
  if (out->outcome != ASNGN_CACHE_MISS && best != NULL) {
    out->query = asngn_strdup(best->query);
    out->answer = asngn_strdup(best->answer);
    snprintf(out->detail, sizeof out->detail, "%s", best->detail);
    snprintf(out->tier, sizeof out->tier, "%s", best->tier);
    out->gen_tokens = best->gen_tokens;
    if (out->query == NULL || out->answer == NULL) {
      free(out->query);
      free(out->answer);
      out->query = NULL;
      out->answer = NULL;
      out->outcome = ASNGN_CACHE_MISS;
    }
  }
  out->cos = best != NULL ? best_cos : 0.0;
  {
    bool touch_ok = true;
    if (have_touch) touch_ok = cache_append_line(c, &touch) == ASNGN_OK;
    os_rwlock_wrunlock(&c->cache_mu);
    if (!touch_ok)
      asngn_log(c, ASNGN_LOG_WARN, "cache",
                "touch append failed; hit kept in memory until compact");
  }

  out->query_vec = vec;
  asngn_buf_free(&touch);
  cache_emit_probe(c, s, out->outcome, out->cos);
  return ASNGN_OK;
}

void asngn_cache_probe_free(asngn_cache_probe_result *p) {
  if (p == NULL) return;
  free(p->query_vec);
  free(p->query);
  free(p->answer);
  memset(p, 0, sizeof *p);
}

/* ── plan hints ──────────────────────────────────────────────────────── */

asngn_err asngn_cache_plan_hint(asngn_ctx *c, asngn_session *s,
                                const float *query_vec, char **out_line) {
  if (c->owner) c=c->owner;
  int dim;
  size_t i;
  asngn_time now;
  const asngn_cache_entry *best = NULL;
  double best_cos = 0.0;
  asngn_err e = ASNGN_OK;

  if (c == NULL || s == NULL || out_line == NULL) return ASNGN_ERR_INVALID;
  *out_line = NULL;
  if (!c->cfg.cache_enable || query_vec == NULL) return ASNGN_OK;
  dim = asngn_models_embed_dim(c);
  if (dim <= 0) dim = c->embed_dim;
  if (dim <= 0) return ASNGN_OK;

  now = asngn_clock_now(&c->clock);
  os_rwlock_rdlock(&c->cache_mu);
  for (i = 0; i < c->cache_n; i++) {
    const asngn_cache_entry *ent = &c->cache[i];
    double cos;
    if (!ent->tools_used || ent->tools_n == 0 || ent->vec == NULL) continue;
    if (!cache_eligible(ent, s, now)) continue;
    cos = cache_dot(query_vec, ent->vec, dim);
    if (cos < c->cfg.adapt_threshold) continue;
    if (best == NULL || cos > best_cos) {
      best = ent;
      best_cos = cos;
    }
  }
  if (best != NULL) {
    asngn_buf line;
    size_t k, n = best->tools_n < 5 ? best->tools_n : 5;
    asngn_buf_init(&line);
    e = asngn_buf_appends(&line, "a similar request previously used: ");
    for (k = 0; e == ASNGN_OK && k < n; k++) {
      if (k > 0) e = asngn_buf_appends(&line, ", ");
      if (e == ASNGN_OK)
        e = asngn_buf_appends(&line,
                              best->tools[k] != NULL ? best->tools[k] : "");
    }
    if (e == ASNGN_OK) {
      *out_line = asngn_buf_detach(&line);
      if (*out_line == NULL) e = ASNGN_ERR_NOMEM;
    }
    asngn_buf_free(&line);
  }
  os_rwlock_rdunlock(&c->cache_mu);
  if (e == ASNGN_ERR_NOMEM)
    return asngn_seterr(c, e, "cache: out of memory building plan hint");
  return e;
}

/* ── insert ───────────────────────────────────────────────────────────── */

asngn_err asngn_cache_insert(asngn_ctx *c, asngn_session *s, const char *query,
                             const float *vec, const char *answer,
                             const char *detail, const char *tier,
                             size_t gen_tokens, bool tools_used, char **tools,
                             size_t tools_n) {
  if (c->owner) c=c->owner;
  asngn_cache_entry ent;
  asngn_buf line;
  xcdn_node_t *node;
  int dim;
  size_t i;
  asngn_err e;
  bool ok;

  if (c == NULL || s == NULL || query == NULL || answer == NULL)
    return ASNGN_ERR_INVALID;
  if (!c->cfg.cache_enable || vec == NULL) return ASNGN_OK;
  dim = asngn_models_embed_dim(c);
  if (dim <= 0) dim = c->embed_dim;
  if (dim <= 0) return ASNGN_OK; /* no embedder: nothing probeable */

  memset(&ent, 0, sizeof ent);
  asngn_uuid_v4(ent.id);
  ent.scope = c->cfg.cache_scope;
  ent.epoch = s->world_epoch;
  ent.created_at = ent.last_hit = asngn_clock_now(&c->clock);
  ent.hit_count = 0;
  ent.ttl_s = c->cfg.cache_ttl_s;
  ent.gen_tokens = gen_tokens;
  ent.tools_used = tools_used;
  snprintf(ent.detail, sizeof ent.detail, "%s", detail != NULL ? detail : "");
  snprintf(ent.tier, sizeof ent.tier, "%s", tier != NULL ? tier : "");

  ok = true;
  ent.query = asngn_strdup(query);
  ok = ok && ent.query != NULL;
  if (ok) {
    ent.answer = asngn_strdup(answer);
    ok = ent.answer != NULL;
  }
  if (ok && ent.scope == ASNGN_SCOPE_SESSION) {
    ent.session = asngn_strdup(s->slug);
    ok = ent.session != NULL;
  }
  if (ok && s->project != NULL) {
    ent.project = asngn_strdup(s->project);
    ok = ent.project != NULL;
  }
  if (ok) {
    ent.vec = malloc((size_t)dim * sizeof *ent.vec);
    ok = ent.vec != NULL;
    if (ok) memcpy(ent.vec, vec, (size_t)dim * sizeof *ent.vec);
  }
  if (ok && tools_used && tools != NULL && tools_n > 0) {
    ent.tools = calloc(tools_n, sizeof *ent.tools);
    ok = ent.tools != NULL;
    for (i = 0; ok && i < tools_n; i++) {
      if (tools[i] == NULL) continue;
      ent.tools[ent.tools_n] = asngn_strdup(tools[i]);
      ok = ent.tools[ent.tools_n] != NULL;
      if (ok) ent.tools_n++;
    }
  }
  if (!ok) {
    cache_entry_free(&ent);
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "cache: out of memory inserting");
  }

  /* Serialize before taking the lock (the node is pure entry state). */
  asngn_buf_init(&line);
  node = cache_entry_node(&ent);
  if (node == NULL) {
    cache_entry_free(&ent);
    asngn_buf_free(&line);
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "cache: out of memory inserting");
  }
  e = asngn_xnode_write(node, false, &line);
  xcdn_node_free(node);
  if (e != ASNGN_OK) {
    cache_entry_free(&ent);
    asngn_buf_free(&line);
    return asngn_seterr(c, e, "cache: cannot serialize entry");
  }

  os_rwlock_wrlock(&c->cache_mu);
  if (c->cfg.cache_max_entries > 0) { /* evict the oldest last_hit */
    while (c->cache_n >= (size_t)c->cfg.cache_max_entries && c->cache_n > 0) {
      size_t victim = 0;
      for (i = 1; i < c->cache_n; i++)
        if (c->cache[i].last_hit < c->cache[victim].last_hit) victim = i;
      cache_remove_at(c, victim);
    }
  }
  e = cache_reserve(c);
  if (e != ASNGN_OK) {
    os_rwlock_wrunlock(&c->cache_mu);
    cache_entry_free(&ent);
    asngn_buf_free(&line);
    return asngn_seterr(c, e, "cache: out of memory inserting");
  }
  c->cache[c->cache_n++] = ent;
  /* Durable append inside the critical section: a concurrent compact /
   * clear cannot slip between the in-memory insert and the file write
   * (which would resurrect cleared entries or duplicate compacted ones).
   * A failure is survivable: the entry stays resident and the next
   * compact rewrites the file from memory. */
  e = cache_append_line(c, &line);
  os_rwlock_wrunlock(&c->cache_mu);
  if (e != ASNGN_OK)
    asngn_log(c, ASNGN_LOG_WARN, "cache",
              "insert: append to semantic.xcdn failed; entry kept in "
              "memory until compact");
  asngn_buf_free(&line);
  return ASNGN_OK;
}

/* ── maintenance ──────────────────────────────────────────────────────── */

asngn_err asngn_cache_clear_scope(asngn_ctx *c, const char *scope) {
  if (c->owner) c=c->owner;
  bool drop_session, drop_global;
  size_t i, dropped = 0;
  asngn_err e;

  if (c == NULL) return ASNGN_ERR_INVALID;
  if (scope == NULL) {
    drop_session = drop_global = true;
  } else if (strcmp(scope, "session") == 0) {
    drop_session = true;
    drop_global = false;
  } else if (strcmp(scope, "global") == 0) {
    drop_session = false;
    drop_global = true;
  } else {
    return asngn_seterr(c, ASNGN_ERR_INVALID, "cache: unknown scope \"%s\"",
                        scope);
  }

  os_rwlock_wrlock(&c->cache_mu);
  for (i = c->cache_n; i-- > 0;) {
    bool global = c->cache[i].scope == ASNGN_SCOPE_GLOBAL;
    if ((global && drop_global) || (!global && drop_session)) {
      cache_remove_at(c, i);
      dropped++;
    }
  }
  e = cache_compact_locked(c);
  os_rwlock_wrunlock(&c->cache_mu);
  asngn_log(c, ASNGN_LOG_INFO, "cache",
            "semantic cache cleared (%s): %zu entr%s dropped",
            scope != NULL ? scope : "all", dropped,
            dropped == 1 ? "y" : "ies");
  return e;
}

asngn_err asngn_cache_sweep(asngn_ctx *c) {
  size_t i, dropped = 0;
  asngn_time now;
  asngn_err e = ASNGN_OK;

  if (c == NULL) return ASNGN_ERR_INVALID;
  now = asngn_clock_now(&c->clock);
  os_rwlock_wrlock(&c->cache_mu);
  for (i = c->cache_n; i-- > 0;) {
    const asngn_cache_entry *ent = &c->cache[i];
    if (ent->ttl_s > 0 && ent->created_at + ent->ttl_s <= now) {
      cache_remove_at(c, i);
      dropped++;
    }
  }
  if (dropped > 0) e = cache_compact_locked(c);
  os_rwlock_wrunlock(&c->cache_mu);
  if (dropped > 0)
    asngn_log(c, ASNGN_LOG_INFO, "cache",
              "sweep: %zu expired entr%s dropped", dropped,
              dropped == 1 ? "y" : "ies");
  return e;
}

asngn_err asngn_cache_compact(asngn_ctx *c) {
  asngn_err e;
  if (c == NULL) return ASNGN_ERR_INVALID;
  os_rwlock_wrlock(&c->cache_mu);
  e = cache_compact_locked(c);
  os_rwlock_wrunlock(&c->cache_mu);
  return e;
}

size_t asngn_cache_count(asngn_ctx *c) {
  size_t n;
  if (c == NULL) return 0;
  os_rwlock_rdlock(&c->cache_mu);
  n = c->cache_n;
  os_rwlock_rdunlock(&c->cache_mu);
  return n;
}
