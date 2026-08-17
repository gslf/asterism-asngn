/*
 * toolcache.c — the exact-key tool-result cache.
 *
 * A flat in-memory array on the context short-circuits repeated identical
 * tool calls. The key is the caller-computed SHA-256 over tool id +
 * version + command + canonical args; eligibility (read_only ∧ idempotent)
 * is likewise the caller's job. TTL is cache.tool_ttl, capacity
 * cache.tool_max_entries with LRU eviction by last_used_ms; results larger
 * than 64 KiB are silently not cached. A world-epoch bump clears the
 * cache entirely (asngn_toolcache_clear).
 *
 * Persistence mirrors the stream discipline of the siblings, kept v1
 * simple and honest: every put appends one
 *   #tool_cache_entry { key: "<hex64>", result: "...", at: t"..." }
 * to cache/tools.xcdn (transient open/append/close, torn-tail rule on
 * load); a clear rewrites the file empty atomically. No compaction pass:
 * expired and superseded records are dropped on reload.
 *
 * Locking: c->cache_mu guards the array. get/put/clear take the
 * WRITE lock — even a get mutates (LRU stamp, expiry drop). Tool hits are
 * counted by the caller; no telemetry is emitted here.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

#define ASNGN_TOOLCACHE_RESULT_MAX (64u * 1024u)

/* ── helpers ──────────────────────────────────────────────────────────── */

static char *tc_path(asngn_ctx *c) {
  return os_path_join(c->cache_dir, "tools.xcdn");
}

static int tc_hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static bool tc_key_parse(const char *s, uint8_t out[32]) {
  size_t i;
  if (s == NULL || strlen(s) != 64) return false;
  for (i = 0; i < 32; i++) {
    int hi = tc_hexval(s[2 * i]);
    int lo = tc_hexval(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static void tc_remove_at(asngn_ctx *c, size_t i) {
  free(c->toolcache[i].result_xcdn);
  if (i + 1 < c->toolcache_n)
    memmove(&c->toolcache[i], &c->toolcache[i + 1],
            (c->toolcache_n - i - 1) * sizeof *c->toolcache);
  c->toolcache_n--;
}

static void tc_evict_lru(asngn_ctx *c) {
  size_t victim = 0, i;
  if (c->toolcache_n == 0) return;
  for (i = 1; i < c->toolcache_n; i++)
    if (c->toolcache[i].last_used_ms < c->toolcache[victim].last_used_ms)
      victim = i;
  tc_remove_at(c, victim);
}

static asngn_err tc_reserve(asngn_ctx *c) {
  if (c->toolcache_n < c->toolcache_cap) return ASNGN_OK;
  {
    size_t cap = c->toolcache_cap != 0 ? c->toolcache_cap * 2 : 16;
    asngn_toolcache_entry *ne = realloc(c->toolcache, cap * sizeof *ne);
    if (ne == NULL) return ASNGN_ERR_NOMEM;
    c->toolcache = ne;
    c->toolcache_cap = cap;
  }
  return ASNGN_OK;
}

/* Serialize one #tool_cache_entry record into a compact line. */
static asngn_err tc_entry_line(const uint8_t key[32], const char *result,
                               asngn_time at, asngn_buf *out) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node = NULL;
  char hex[65], ts[21];
  asngn_err e = ASNGN_ERR_NOMEM;
  bool ok = obj != NULL;

  asngn_sha256_hex(key, 32, hex);
  asngn_time_format_rfc3339(at, ts);
  ok = ok && asngn_xobj_put(obj, "key", xcdn_value_string(hex));
  ok = ok && asngn_xobj_put(obj, "result", xcdn_value_string(result));
  ok = ok && asngn_xobj_put(obj, "at", xcdn_value_datetime(ts));
  if (ok) {
    node = xcdn_node_new(obj);
    if (node != NULL) {
      obj = NULL;
      ok = asngn_xnode_tag(node, "tool_cache_entry");
    } else ok = false;
  }
  if (ok) e = asngn_xnode_write(node, false, out);
  if (node != NULL) xcdn_node_free(node);
  if (obj != NULL) xcdn_value_free(obj);
  return e;
}

static asngn_err tc_append_line(asngn_ctx *c, const asngn_buf *line) {
  asngn_stream st;
  char *path;
  asngn_err e;

  if (line->data == NULL || line->len == 0) return ASNGN_ERR_INVALID;
  path = tc_path(c);
  if (path == NULL) return ASNGN_ERR_NOMEM;
  e = asngn_stream_open(c, &st, path, false);
  free(path);
  if (e != ASNGN_OK) return e;
  e = asngn_stream_append(c, &st, line->data, line->len);
  asngn_stream_close(&st);
  return e;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

asngn_err asngn_toolcache_init(asngn_ctx *c) {
  char *path;
  struct xcdn_document *docp = NULL;
  xcdn_document_t *doc;
  asngn_err e;
  size_t i;
  asngn_time now;
  int64_t mono;

  if (c == NULL || c->cache_dir == NULL) return ASNGN_ERR_INVALID;
  if (!c->cfg.tool_cache) return ASNGN_OK;
  (void)os_mkdir_p(c->cache_dir);
  path = tc_path(c);
  if (path == NULL)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "tool cache: out of memory");
  e = asngn_stream_load(c, path, "tool cache", &docp);
  free(path);
  if (e != ASNGN_OK) return e;
  doc = (xcdn_document_t *)docp;
  if (doc == NULL) return ASNGN_OK;

  now = asngn_clock_now(&c->clock);
  mono = asngn_clock_mono_ms(&c->clock);
  for (i = 0; i < doc->values_len; i++) {
    const xcdn_node_t *node = doc->values[i];
    const xcdn_value_t *obj;
    uint8_t key[32];
    const char *result;
    char *copy;
    asngn_time at;
    asngn_toolcache_entry *slot = NULL;
    size_t j;

    if (!xcdn_node_has_tag(node, "tool_cache_entry")) {
      asngn_log(c, ASNGN_LOG_WARN, "cache",
                "tools.xcdn value %zu has an unknown tag; skipped", i);
      continue;
    }
    obj = node->value;
    if (obj == NULL || obj->type != XCDN_VAL_OBJECT) {
      asngn_log(c, ASNGN_LOG_WARN, "cache",
                "tools.xcdn value %zu invalid; skipped", i);
      continue;
    }
    result = asngn_xstr(asngn_xfield(obj, "result"));
    if (!tc_key_parse(asngn_xstr(asngn_xfield(obj, "key")), key) ||
        result == NULL || !asngn_xtime(asngn_xfield(obj, "at"), &at)) {
      asngn_log(c, ASNGN_LOG_WARN, "cache",
                "tools.xcdn value %zu invalid; skipped", i);
      continue;
    }
    if (c->cfg.tool_ttl_s > 0 && at + c->cfg.tool_ttl_s <= now)
      continue; /* expired on disk: silently dropped by the reload */

    copy = asngn_strdup(result);
    if (copy == NULL) {
      xcdn_document_free(doc);
      return asngn_seterr(c, ASNGN_ERR_NOMEM, "tool cache: out of memory");
    }
    for (j = 0; j < c->toolcache_n; j++) {
      if (memcmp(c->toolcache[j].key, key, 32) == 0) {
        slot = &c->toolcache[j];
        break;
      }
    }
    if (slot != NULL) { /* later record supersedes an earlier put */
      free(slot->result_xcdn);
    } else {
      if (c->cfg.tool_max_entries > 0) {
        while (c->toolcache_n >= (size_t)c->cfg.tool_max_entries)
          tc_evict_lru(c);
      }
      if (tc_reserve(c) != ASNGN_OK) {
        free(copy);
        xcdn_document_free(doc);
        return asngn_seterr(c, ASNGN_ERR_NOMEM, "tool cache: out of memory");
      }
      slot = &c->toolcache[c->toolcache_n++];
      memcpy(slot->key, key, 32);
    }
    slot->result_xcdn = copy;
    slot->at = at;
    /* Replayed stamps sit just below the current monotonic clock, in
     * file order, so any live touch always outranks them for LRU. */
    slot->last_used_ms = mono - (int64_t)(doc->values_len - i);
  }
  xcdn_document_free(doc);
  asngn_log(c, ASNGN_LOG_INFO, "cache", "tool cache: %zu entr%s loaded",
            c->toolcache_n, c->toolcache_n == 1 ? "y" : "ies");
  return ASNGN_OK;
}

void asngn_toolcache_shutdown(asngn_ctx *c) {
  size_t i;
  if (c == NULL) return;
  for (i = 0; i < c->toolcache_n; i++) free(c->toolcache[i].result_xcdn);
  free(c->toolcache);
  c->toolcache = NULL;
  c->toolcache_n = c->toolcache_cap = 0;
}

/* ── get / put / clear ────────────────────────────────────────────────── */

/* On a hit *out_result is a malloc'd copy (caller frees with free());
 * takes the write lock because a hit stamps LRU and expiry drops. */
bool asngn_toolcache_get(asngn_ctx *c, const uint8_t key[32],
                         char **out_result) {
  size_t i;
  bool found = false;

  if (c == NULL || key == NULL || out_result == NULL) return false;
  *out_result = NULL;
  if (!c->cfg.tool_cache) return false;

  os_rwlock_wrlock(&c->cache_mu);
  for (i = 0; i < c->toolcache_n; i++) {
    asngn_toolcache_entry *ent = &c->toolcache[i];
    if (memcmp(ent->key, key, 32) != 0) continue;
    if (c->cfg.tool_ttl_s > 0 &&
        ent->at + c->cfg.tool_ttl_s <= asngn_clock_now(&c->clock)) {
      tc_remove_at(c, i); /* expired: miss and drop */
      break;
    }
    ent->last_used_ms = asngn_clock_mono_ms(&c->clock);
    *out_result = asngn_strdup(ent->result_xcdn);
    found = *out_result != NULL;
    break;
  }
  os_rwlock_wrunlock(&c->cache_mu);
  return found;
}

void asngn_toolcache_put(asngn_ctx *c, const uint8_t key[32],
                         const char *result_xcdn) {
  size_t i;
  asngn_time now;
  int64_t mono;
  asngn_buf line;
  char *copy;
  asngn_toolcache_entry *slot = NULL;

  if (c == NULL || key == NULL || result_xcdn == NULL) return;
  if (!c->cfg.tool_cache) return;
  if (strlen(result_xcdn) > ASNGN_TOOLCACHE_RESULT_MAX)
    return; /* silently: oversized results are not cached */
  copy = asngn_strdup(result_xcdn);
  if (copy == NULL) return;

  now = asngn_clock_now(&c->clock);
  mono = asngn_clock_mono_ms(&c->clock);

  os_rwlock_wrlock(&c->cache_mu);
  for (i = 0; i < c->toolcache_n; i++) {
    if (memcmp(c->toolcache[i].key, key, 32) == 0) {
      slot = &c->toolcache[i];
      break;
    }
  }
  if (slot != NULL) {
    free(slot->result_xcdn);
  } else {
    if (c->cfg.tool_max_entries > 0) {
      while (c->toolcache_n >= (size_t)c->cfg.tool_max_entries)
        tc_evict_lru(c);
    }
    if (tc_reserve(c) != ASNGN_OK) {
      os_rwlock_wrunlock(&c->cache_mu);
      free(copy);
      return;
    }
    slot = &c->toolcache[c->toolcache_n++];
    memcpy(slot->key, key, 32);
  }
  slot->result_xcdn = copy;
  slot->at = now;
  slot->last_used_ms = mono;
  os_rwlock_wrunlock(&c->cache_mu);

  /* Best-effort persistence; the reload drops what never landed. */
  asngn_buf_init(&line);
  if (tc_entry_line(key, result_xcdn, now, &line) != ASNGN_OK ||
      tc_append_line(c, &line) != ASNGN_OK)
    asngn_log(c, ASNGN_LOG_WARN, "cache",
              "tool cache: persist to tools.xcdn failed; entry kept in "
              "memory only");
  asngn_buf_free(&line);
}

/* World-epoch bump invalidation: drop everything, cheap and safe. */
void asngn_toolcache_clear(asngn_ctx *c) {
  size_t i;
  char *path;

  if (c == NULL) return;
  os_rwlock_wrlock(&c->cache_mu);
  for (i = 0; i < c->toolcache_n; i++) free(c->toolcache[i].result_xcdn);
  c->toolcache_n = 0;
  os_rwlock_wrunlock(&c->cache_mu);

  path = c->cache_dir != NULL ? tc_path(c) : NULL;
  if (path == NULL ||
      asngn_write_atomic(c, path, "", 0) != ASNGN_OK)
    asngn_log(c, ASNGN_LOG_WARN, "cache",
              "tool cache: cannot rewrite tools.xcdn empty");
  free(path);
}
