/*
 * telemetry.c — event ring + batched, rotating file sink.
 *
 * One event shape everywhere: a hand-emitted compact #asngn_event value
 * (emitting xCDN is trivial). The in-memory ring is always on; the
 * file sink exists only when telemetry.path is configured, appends in one
 * batch per turn (asngn_tele_flush), and rotates by size exactly like the
 * log. A telemetry failure never fails the turn that emitted it:
 * every failure in this module is silent or a WARN log at worst.
 *
 * Locking: c->tele_mu is a leaf — never held while taking another
 * lock, so the host event callback and asngn_log run outside it.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

/* ── path resolution ──────────────────────────────────────────────────── */

static bool tele_path_is_abs(const char *p) {
  if (p[0] == '/') return true;
#ifdef _WIN32
  if (p[0] == '\\') return true;
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':')
    return true;
#endif
  return false;
}

/* cfg.tele_path resolved to the file the sink writes: absolute paths are
 * taken as-is, relative ones join under the engine root (the default
 * "telemetry/telemetry.xcdn" thus lands in <root>/telemetry/). Malloc'd;
 * NULL when unset or on OOM. Shared by init and rotation so both always
 * agree on the target file. */
static char *tele_resolved_path(asngn_ctx *c) {
  const char *p = c->cfg.tele_path;
  if (!p || p[0] == '\0') return NULL;
  if (tele_path_is_abs(p)) return asngn_strdup(p);
  return os_path_join(c->root, p);
}

/* Last path separator, or NULL. */
static char *tele_last_sep(char *path) {
  char *a = strrchr(path, '/');
#ifdef _WIN32
  char *b = strrchr(path, '\\');
  if (!a || (b && b > a)) a = b;
#endif
  return a;
}

/* ── init / shutdown ──────────────────────────────────────────────────── */

asngn_err asngn_tele_init(asngn_ctx *c) {
  size_t cap;
  char *path;
  if (!c) return ASNGN_ERR_INVALID;

  cap = c->cfg.tele_ring > 0 ? (size_t)c->cfg.tele_ring : 1;
  c->ring.items = calloc(cap, sizeof *c->ring.items);
  if (!c->ring.items)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "telemetry: ring allocation");
  c->ring.cap = cap;
  c->ring.n = 0;
  c->ring.head = 0;

  asngn_buf_init(&c->tele_batch);
  c->tele_fp = NULL;
  c->tele_size = 0;

  if (!c->cfg.tele_path || c->cfg.tele_path[0] == '\0')
    return ASNGN_OK; /* ring-only mode */

  path = tele_resolved_path(c);
  if (!path) {
    asngn_log(c, ASNGN_LOG_WARN, "telemetry",
              "cannot resolve telemetry path; file sink disabled");
    return ASNGN_OK;
  }

  {
    char *sep = tele_last_sep(path);
    if (sep && sep != path) {
      *sep = '\0';
      (void)os_mkdir_p(path);
      *sep = '/';
    }
  }

  c->tele_fp = os_fopen(path, "ab");
  if (c->tele_fp) {
    if (fseek(c->tele_fp, 0, SEEK_END) == 0) {
      long pos = ftell(c->tele_fp);
      if (pos > 0) c->tele_size = (size_t)pos;
    }
  } else {
    asngn_log(c, ASNGN_LOG_WARN, "telemetry",
              "cannot open '%s'; file sink disabled", path);
  }
  free(path);
  return ASNGN_OK; /* the ring works either way */
}

void asngn_tele_shutdown(asngn_ctx *c) {
  size_t i;
  if (!c) return;
  asngn_tele_flush(c);
  os_mutex_lock(&c->tele_mu);
  if (c->tele_fp) {
    fclose(c->tele_fp);
    c->tele_fp = NULL;
  }
  c->tele_size = 0;
  if (c->ring.items) {
    for (i = 0; i < c->ring.cap; i++) free(c->ring.items[i]);
    free(c->ring.items);
  }
  c->ring.items = NULL;
  c->ring.cap = 0;
  c->ring.n = 0;
  c->ring.head = 0;
  asngn_buf_free(&c->tele_batch);
  os_mutex_unlock(&c->tele_mu);
}

/* ── emit ─────────────────────────────────────────────────────────────── */

/* kind, span, parent, and session are engine-controlled tokens (no
 * escaping needed); defensively drop '"' and line breaks anyway so a bad
 * token can never tear the single-line value. */
static asngn_err tele_append_token(asngn_buf *b, const char *s) {
  for (; *s; s++) {
    if (*s == '"' || *s == '\n' || *s == '\r') continue;
    if (asngn_buf_appendc(b, *s) != ASNGN_OK) return ASNGN_ERR_NOMEM;
  }
  return ASNGN_OK;
}

void asngn_tele_emit(asngn_ctx *c, const char *kind, const char *span,
                     const char *parent, const char *session, size_t turn,
                     const char *data_xcdn) {
  if (c && c->owner) c=c->owner;
  asngn_buf b;
  char stamp[21];
  char *line;
  size_t len;
  bool ok;
  asngn_event_fn cb;
  void *cb_ud;

  if (!c || !kind) return;

  /* Build the line outside the mutex. Shape:
   *   #asngn_event {at: t"...", kind: "...", span: u"...", parent: u"...",
   *                 session: "...", turn: n, data: {...}}
   * span/parent/session/turn are omitted entirely when NULL/0. */
  asngn_buf_init(&b);
  asngn_time_format_rfc3339(asngn_clock_now(&c->clock), stamp);

  ok = asngn_buf_printf(&b, "#asngn_event {at: t\"%s\", kind: \"", stamp) ==
           ASNGN_OK &&
       tele_append_token(&b, kind) == ASNGN_OK &&
       asngn_buf_appendc(&b, '"') == ASNGN_OK;
  if (ok && span && span[0]) {
    ok = asngn_buf_appends(&b, ", span: u\"") == ASNGN_OK &&
         tele_append_token(&b, span) == ASNGN_OK &&
         asngn_buf_appendc(&b, '"') == ASNGN_OK;
  }
  if (ok && parent && parent[0]) {
    ok = asngn_buf_appends(&b, ", parent: u\"") == ASNGN_OK &&
         tele_append_token(&b, parent) == ASNGN_OK &&
         asngn_buf_appendc(&b, '"') == ASNGN_OK;
  }
  if (ok && session && session[0]) {
    ok = asngn_buf_appends(&b, ", session: \"") == ASNGN_OK &&
         tele_append_token(&b, session) == ASNGN_OK &&
         asngn_buf_appendc(&b, '"') == ASNGN_OK;
  }
  if (ok && turn > 0) ok = asngn_buf_printf(&b, ", turn: %zu", turn) == ASNGN_OK;
  if (ok) {
    ok = asngn_buf_appends(&b, ", data: ") == ASNGN_OK &&
         asngn_buf_appends(&b, data_xcdn && data_xcdn[0] ? data_xcdn : "{}") ==
             ASNGN_OK &&
         asngn_buf_appendc(&b, '}') == ASNGN_OK;
  }
  if (!ok) { /* OOM: drop silently — telemetry never fails the caller */
    asngn_buf_free(&b);
    return;
  }
  len = b.len;
  line = asngn_buf_detach(&b);
  if (!line) return;

  os_mutex_lock(&c->tele_mu);
  if (c->ring.items && c->ring.cap > 0) {
    char *copy = asngn_strdup(line);
    if (copy) { /* overwrite-oldest */
      size_t slot;
      if (c->ring.n == c->ring.cap) {
        free(c->ring.items[c->ring.head]);
        c->ring.items[c->ring.head] = NULL;
        c->ring.head = (c->ring.head + 1) % c->ring.cap;
        c->ring.n--;
      }
      slot = (c->ring.head + c->ring.n) % c->ring.cap;
      c->ring.items[slot] = copy;
      c->ring.n++;
    }
  }
  if (c->tele_fp) {
    size_t old = c->tele_batch.len;
    if (asngn_buf_append(&c->tele_batch, line, len) != ASNGN_OK ||
        asngn_buf_appendc(&c->tele_batch, '\n') != ASNGN_OK) {
      /* OOM: roll the batch back to whole lines and drop this event */
      c->tele_batch.len = old;
      if (c->tele_batch.data) c->tele_batch.data[old] = '\0';
    }
  }
  cb = c->event_cb;
  cb_ud = c->event_ud;
  os_mutex_unlock(&c->tele_mu);

  /* Callback outside tele_mu (tele_mu is a leaf). */
  if (cb) cb(line, cb_ud);
  free(line);
}

/* ── tail ─────────────────────────────────────────────────────────────── */

asngn_err asngn_tele_tail(asngn_ctx *c, size_t n, char ***out, size_t *out_n) {
  char **v = NULL;
  size_t take, start, i = 0;
  bool oom = false;

  if (!c || !out || !out_n) return ASNGN_ERR_INVALID;
  *out = NULL;
  *out_n = 0;

  os_mutex_lock(&c->tele_mu);
  take = n < c->ring.n ? n : c->ring.n;
  if (take > 0) {
    v = calloc(take, sizeof *v);
    if (!v) {
      oom = true;
    } else {
      start = c->ring.head + (c->ring.n - take); /* oldest of the tail */
      for (i = 0; i < take; i++) {
        v[i] = asngn_strdup(c->ring.items[(start + i) % c->ring.cap]);
        if (!v[i]) {
          oom = true;
          break;
        }
      }
      if (oom) {
        while (i > 0) free(v[--i]);
        free(v);
        v = NULL;
      }
    }
  }
  os_mutex_unlock(&c->tele_mu);

  if (oom) return asngn_seterr(c, ASNGN_ERR_NOMEM, "telemetry: tail copy");
  *out = v;
  *out_n = take;
  return ASNGN_OK;
}

/* ── flush + rotation ─────────────────────────────────────────────────── */

/* Rotate <path> -> <path>.1 -> ... -> <path>.(max_files-1); the overflow
 * file is deleted. Reopens a fresh current file (log.c's scheme). tele_mu
 * held. On failure the sink may end up disabled (tele_fp NULL) — the
 * caller WARNs once outside the mutex. */
static void tele_rotate_locked(asngn_ctx *c) {
  char *path, *pa, *pb;
  size_t plen;
  int i, maxf = c->cfg.tele_max_files;

  if (c->tele_fp) {
    fflush(c->tele_fp);
    fclose(c->tele_fp);
    c->tele_fp = NULL;
  }
  c->tele_size = 0;

  path = tele_resolved_path(c);
  if (!path) return; /* OOM: sink stays disabled */
  plen = strlen(path);
  if (maxf < 1) maxf = 1;

  pa = malloc(plen + 32);
  pb = malloc(plen + 32);
  if (pa && pb) {
    if (maxf == 1) {
      (void)os_remove_file(path); /* no rotated copies kept */
    } else {
      snprintf(pa, plen + 32, "%s.%d", path, maxf - 1);
      (void)os_remove_file(pa); /* delete overflow beyond max_files */
      for (i = maxf - 1; i >= 2; i--) {
        snprintf(pa, plen + 32, "%s.%d", path, i - 1);
        snprintf(pb, plen + 32, "%s.%d", path, i);
        (void)os_rename(pa, pb); /* best effort; source may be absent */
      }
      snprintf(pa, plen + 32, "%s.1", path);
      (void)os_rename(path, pa);
    }
  }
  free(pa);
  free(pb);

  c->tele_fp = os_fopen(path, "ab");
  if (c->tele_fp) {
    uint64_t sz = 0;
    /* if the rename failed we appended to the old file: track real size */
    if (os_file_size(path, &sz) == ASNGN_OK) c->tele_size = (size_t)sz;
  }
  free(path);
}

void asngn_tele_flush(asngn_ctx *c) {
  if (c && c->owner) c=c->owner;
  bool warn_rotate = false, warn_write = false;
  if (!c) return;

  os_mutex_lock(&c->tele_mu);
  if (c->tele_fp && c->tele_batch.len > 0) {
    size_t len = c->tele_batch.len;
    if (c->cfg.tele_rotate_kb > 0 &&
        c->tele_size + len > (size_t)c->cfg.tele_rotate_kb * 1024u)
      tele_rotate_locked(c);
    if (c->tele_fp) {
      if (fwrite(c->tele_batch.data, 1, len, c->tele_fp) == len) {
        fflush(c->tele_fp);
        c->tele_size += len;
      } else {
        warn_write = true;
      }
    } else {
      warn_rotate = true; /* rotation lost the sink; batch is dropped */
    }
    c->tele_batch.len = 0;
    if (c->tele_batch.data) c->tele_batch.data[0] = '\0';
  }
  os_mutex_unlock(&c->tele_mu);

  /* WARNs outside tele_mu — a telemetry failure never fails the turn. */
  if (warn_rotate)
    asngn_log(c, ASNGN_LOG_WARN, "telemetry",
              "rotation failed; file sink disabled");
  if (warn_write)
    asngn_log(c, ASNGN_LOG_WARN, "telemetry", "batch write failed");
}
