/*
 * session.c — the asngn session store.
 *
 * One directory per session under <root>/sessions/<slug>/:
 *   session.xcdn      #asngn_session manifest (atomic replace)
 *   transcript.xcdn   append stream of #turn values (torn-tail rule),
 *                     fsync'd per append so a committed turn is at least
 *                     as durable as the manifest that counts it;
 *                     rewritten atomically when fold flags change
 *   summary.xcdn      #asngn_summary (atomic replace)
 *   ledger.xcdn       append stream (ledger.c)
 *   blobs/<id>.txt    digested full texts; the blob index is
 *                     per-turn state, the files persist for the user
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

/* ── helpers ──────────────────────────────────────────────────────────── */

static char *sess_path(const asngn_session *s, const char *name) {
  return os_path_join(s->dir, name);
}

static xcdn_value_t *mk_str(const char *s) {
  return xcdn_value_string(s != NULL ? s : "");
}

static xcdn_value_t *mk_time(asngn_time t) {
  char buf[21];
  asngn_time_format_rfc3339(t, buf);
  return xcdn_value_datetime(buf);
}

/* Serialize one node into a line buffer (compact) and free the node. */
static asngn_err sess_node_line(xcdn_node_t *node, asngn_buf *out) {
  asngn_err e;
  if (node == NULL) return ASNGN_ERR_NOMEM;
  e = asngn_xnode_write(node, false, out);
  xcdn_node_free(node);
  return e;
}

/* ── turn (de)serialization ───────────────────────────────────────────── */

static xcdn_node_t *turn_build_node(const asngn_turn *t) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node;
  bool ok = obj != NULL;
  ok = ok && asngn_xobj_put(obj, "n", xcdn_value_int((int64_t)t->n));
  ok = ok && asngn_xobj_put(obj, "role", mk_str(t->role));
  ok = ok && asngn_xobj_put(obj, "at", mk_time(t->at));
  ok = ok && asngn_xobj_put(obj, "text", mk_str(t->text));
  ok = ok && asngn_xobj_put(obj, "pinned", xcdn_value_bool(t->pinned));
  ok = ok && asngn_xobj_put(obj, "folded", xcdn_value_bool(t->folded));
  if (ok && t->klass[0] != '\0') {
    xcdn_value_t *route = xcdn_value_object();
    ok = route != NULL;
    ok = ok && asngn_xobj_put(route, "class", mk_str(t->klass));
    ok = ok && asngn_xobj_put(route, "detail", mk_str(t->detail));
    ok = ok && asngn_xobj_put(route, "mode", mk_str(t->mode));
    ok = ok && asngn_xobj_put(route, "tier", mk_str(t->tier));
    ok = ok && asngn_xobj_put(route, "steps",
                              xcdn_value_int((int64_t)t->steps));
    ok = ok && asngn_xobj_put(route, "cache", mk_str(t->cache));
    if (ok) ok = asngn_xobj_put(obj, "route", route);
    else if (route != NULL) xcdn_value_free(route);
  }
  if (!ok) {
    xcdn_value_free(obj);
    return NULL;
  }
  node = xcdn_node_new(obj);
  if (node == NULL) {
    xcdn_value_free(obj);
    return NULL;
  }
  if (!asngn_xnode_tag(node, "turn")) {
    xcdn_node_free(node);
    return NULL;
  }
  return node;
}

static void sess_copy_field(char *dst, size_t dstsz, const char *src) {
  if (src == NULL) src = "";
  snprintf(dst, dstsz, "%s", src);
}

static bool turn_from_node(const xcdn_node_t *node, asngn_turn *out) {
  const xcdn_value_t *obj, *v, *route;
  int64_t n;
  const char *s;
  bool b;
  memset(out, 0, sizeof *out);
  if (node == NULL || node->value == NULL ||
      node->value->type != XCDN_VAL_OBJECT)
    return false;
  obj = node->value;
  if (!asngn_xint(asngn_xfield(obj, "n"), &n) || n < 1) return false;
  out->n = (size_t)n;
  s = asngn_xstr(asngn_xfield(obj, "role"));
  if (s == NULL ||
      (strcmp(s, "user") != 0 && strcmp(s, "assistant") != 0))
    return false;
  sess_copy_field(out->role, sizeof out->role, s);
  if (!asngn_xtime(asngn_xfield(obj, "at"), &out->at)) return false;
  s = asngn_xstr(asngn_xfield(obj, "text"));
  if (s == NULL) return false;
  out->text = asngn_strdup(s);
  if (out->text == NULL) return false;
  v = asngn_xfield(obj, "pinned");
  if (v != NULL && asngn_xbool(v, &b)) out->pinned = b;
  v = asngn_xfield(obj, "folded");
  if (v != NULL && asngn_xbool(v, &b)) out->folded = b;
  route = asngn_xfield(obj, "route");
  if (route != NULL && route->type == XCDN_VAL_OBJECT) {
    int64_t steps = 0;
    sess_copy_field(out->klass, sizeof out->klass,
                    asngn_xstr(asngn_xfield(route, "class")));
    sess_copy_field(out->detail, sizeof out->detail,
                    asngn_xstr(asngn_xfield(route, "detail")));
    sess_copy_field(out->mode, sizeof out->mode,
                    asngn_xstr(asngn_xfield(route, "mode")));
    sess_copy_field(out->tier, sizeof out->tier,
                    asngn_xstr(asngn_xfield(route, "tier")));
    sess_copy_field(out->cache, sizeof out->cache,
                    asngn_xstr(asngn_xfield(route, "cache")));
    if (asngn_xint(asngn_xfield(route, "steps"), &steps) && steps >= 0)
      out->steps = (int)steps;
  }
  return true;
}

/* ── manifest ─────────────────────────────────────────────────────────── */

asngn_err asngn_session_save_manifest(asngn_session *s) {
  asngn_ctx *c = s->ctx;
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_value_t *pins = xcdn_value_array();
  xcdn_node_t *node = NULL;
  asngn_buf buf;
  asngn_err e = ASNGN_ERR_NOMEM;
  char *path = NULL;
  size_t i;
  bool ok = obj != NULL && pins != NULL;

  /* Persist the current worktree identity, not merely the fingerprint from
   * session open; editor/build changes are first-class workspace state.
   * An isolated session may be saved while another session is active. */
  if (c->session_workspaces) {
    (void)asngn_workspace_info_refresh(c, &s->workspace);
  } else if (asngn_workspace_refresh(c) == ASNGN_OK) {
    s->workspace = c->workspace;
  }

  asngn_buf_init(&buf);
  for (i = 0; ok && i < s->log_n; i++)
    if (s->log[i].pinned)
      ok = asngn_xarr_push(pins, xcdn_value_int((int64_t)s->log[i].n));
  ok = ok && asngn_xobj_put(obj, "slug", mk_str(s->slug));
  ok = ok && asngn_xobj_put(obj, "created_at", mk_time(s->created_at));
  ok = ok && asngn_xobj_put(obj, "turns",
                            xcdn_value_int((int64_t)s->turns));
  ok = ok && asngn_xobj_put(obj, "world_epoch",
                            xcdn_value_int((int64_t)s->world_epoch));
  ok = ok && asngn_xobj_put(obj, "project",
                            s->project != NULL ? mk_str(s->project)
                                               : xcdn_value_null());
  if (ok) {
    xcdn_value_t *w = xcdn_value_object();
    ok = w != NULL;
    ok = ok && asngn_xobj_put(w, "root", mk_str(s->workspace.canonical_root));
    ok = ok && asngn_xobj_put(w, "repository_root",
                              mk_str(s->workspace.repository_root));
    ok = ok && asngn_xobj_put(w, "head", mk_str(s->workspace.head));
    ok = ok && asngn_xobj_put(w, "branch", mk_str(s->workspace.branch));
    ok = ok && asngn_xobj_put(w, "project_id",
                              mk_str(s->workspace.project_id));
    ok = ok && asngn_xobj_put(w, "ignore_rules",
                              mk_str(s->workspace.ignore_rules));
    ok = ok && asngn_xobj_put(w, "build_adapter",
                              mk_str(s->workspace.build_adapter));
    ok = ok && asngn_xobj_put(w, "fingerprint",
                              mk_str(s->workspace.fingerprint));
    if (ok) ok = asngn_xobj_put(obj, "workspace", w);
    else if (w != NULL) xcdn_value_free(w);
  }
  if (ok) {
    xcdn_node_t *pn = xcdn_node_new(pins);
    if (pn != NULL) {
      pins = NULL; /* owned by pn — put_node consumes it even on failure */
      ok = asngn_xobj_put_node(obj, "pinned", pn);
    } else {
      ok = false;
    }
  }
  ok = ok && asngn_xobj_put(obj, "redact_context",
                            xcdn_value_bool(s->redact_context));
  if (!ok) goto out;
  node = xcdn_node_new(obj);
  if (node == NULL) goto out;
  obj = NULL;
  if (!asngn_xnode_tag(node, "asngn_session")) goto out;
  e = asngn_xnode_write(node, true, &buf);
  if (e != ASNGN_OK) goto out;
  e = asngn_buf_appendc(&buf, '\n');
  if (e != ASNGN_OK) goto out;
  path = sess_path(s, "session.xcdn");
  if (path == NULL) { e = ASNGN_ERR_NOMEM; goto out; }
  e = asngn_write_atomic(c, path, buf.data, buf.len);

out:
  free(path);
  asngn_buf_free(&buf);
  if (node != NULL) xcdn_node_free(node);
  if (obj != NULL) xcdn_value_free(obj);
  if (pins != NULL) xcdn_value_free(pins);
  return e;
}

static void sess_apply_manifest(asngn_session *s, const xcdn_node_t *node) {
  const xcdn_value_t *obj, *v;
  int64_t n;
  bool b;
  if (node == NULL || node->value == NULL ||
      node->value->type != XCDN_VAL_OBJECT)
    return;
  obj = node->value;
  if (asngn_xtime(asngn_xfield(obj, "created_at"), &s->created_at)) { }
  if (asngn_xint(asngn_xfield(obj, "turns"), &n) && n >= 0)
    s->turns = (size_t)n;
  if (asngn_xint(asngn_xfield(obj, "world_epoch"), &n) && n >= 0)
    s->world_epoch = (uint64_t)n;
  v = asngn_xfield(obj, "project");
  if (v != NULL && v->type == XCDN_VAL_STRING) {
    free(s->project);
    s->project = asngn_strdup(v->data.string);
  }
  v = asngn_xfield(obj, "workspace");
  if (v != NULL && v->type == XCDN_VAL_OBJECT) {
#define WS_COPY(field, key) do { const char *ws_ = asngn_xstr(asngn_xfield(v, key)); \
  if (ws_ != NULL) snprintf(s->workspace.field, sizeof s->workspace.field, "%s", ws_); } while (0)
    WS_COPY(canonical_root, "root");
    WS_COPY(repository_root, "repository_root");
    WS_COPY(head, "head");
    WS_COPY(branch, "branch");
    WS_COPY(project_id, "project_id");
    WS_COPY(ignore_rules, "ignore_rules");
    WS_COPY(build_adapter, "build_adapter");
    WS_COPY(fingerprint, "fingerprint");
#undef WS_COPY
    s->workspace_loaded = s->workspace.canonical_root[0] != '\0';
  }
  v = asngn_xfield(obj, "redact_context");
  if (v != NULL && asngn_xbool(v, &b)) s->redact_context = b;
  /* pinned list is applied after the transcript loads (needs ordinals) */
}

static void sess_apply_pins(asngn_session *s, const xcdn_node_t *node) {
  const xcdn_value_t *obj, *pins;
  size_t i, j;
  if (node == NULL || node->value == NULL ||
      node->value->type != XCDN_VAL_OBJECT)
    return;
  obj = node->value;
  pins = asngn_xfield(obj, "pinned");
  if (pins == NULL || pins->type != XCDN_VAL_ARRAY) return;
  for (i = 0; i < xcdn_array_len(pins); i++) {
    const xcdn_node_t *pn = xcdn_array_get(pins, i);
    int64_t n;
    if (pn == NULL || !asngn_xint(pn->value, &n)) continue;
    for (j = 0; j < s->log_n; j++)
      if (s->log[j].n == (size_t)n) s->log[j].pinned = true;
  }
}

/* ── summary ──────────────────────────────────────────────────────────── */

asngn_err asngn_session_save_summary(asngn_session *s) {
  asngn_ctx *c = s->ctx;
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node = NULL;
  asngn_buf buf;
  asngn_err e = ASNGN_ERR_NOMEM;
  char *path = NULL;
  bool ok = obj != NULL;

  asngn_buf_init(&buf);
  ok = ok && asngn_xobj_put(obj, "text", mk_str(s->summary));
  ok = ok && asngn_xobj_put(obj, "debt",
                            xcdn_value_int((int64_t)s->summary_debt));
  if (!ok) goto out;
  node = xcdn_node_new(obj);
  if (node == NULL) goto out;
  obj = NULL;
  if (!asngn_xnode_tag(node, "asngn_summary")) goto out;
  e = asngn_xnode_write(node, true, &buf);
  if (e != ASNGN_OK) goto out;
  e = asngn_buf_appendc(&buf, '\n');
  if (e != ASNGN_OK) goto out;
  path = sess_path(s, "summary.xcdn");
  if (path == NULL) { e = ASNGN_ERR_NOMEM; goto out; }
  e = asngn_write_atomic(c, path, buf.data, buf.len);

out:
  free(path);
  asngn_buf_free(&buf);
  if (node != NULL) xcdn_node_free(node);
  if (obj != NULL) xcdn_value_free(obj);
  return e;
}

static void sess_load_summary(asngn_session *s) {
  asngn_ctx *c = s->ctx;
  char *path = sess_path(s, "summary.xcdn");
  struct xcdn_document *doc = NULL;
  if (path == NULL) return;
  if (asngn_stream_load(c, path, "summary", &doc) == ASNGN_OK &&
      doc != NULL) {
    xcdn_document_t *d = (xcdn_document_t *)doc;
    if (d->values_len > 0 && d->values[0] != NULL &&
        d->values[0]->value != NULL &&
        d->values[0]->value->type == XCDN_VAL_OBJECT) {
      const xcdn_value_t *obj = d->values[0]->value;
      const char *text = asngn_xstr(asngn_xfield(obj, "text"));
      int64_t debt = 0;
      if (text != NULL) {
        free(s->summary);
        s->summary = asngn_strdup(text);
      }
      if (asngn_xint(asngn_xfield(obj, "debt"), &debt) && debt >= 0)
        s->summary_debt = (size_t)debt;
    }
  }
  if (doc != NULL) xcdn_document_free((xcdn_document_t *)doc);
  free(path);
}

/* ── transcript ───────────────────────────────────────────────────────── */

static asngn_err sess_log_reserve(asngn_session *s) {
  if (s->log_n < s->log_cap) return ASNGN_OK;
  {
    size_t cap = s->log_cap != 0 ? s->log_cap * 2 : 32;
    asngn_turn *nl = realloc(s->log, cap * sizeof *nl);
    if (nl == NULL) return ASNGN_ERR_NOMEM;
    s->log = nl;
    s->log_cap = cap;
  }
  return ASNGN_OK;
}

asngn_err asngn_session_append_turn(asngn_session *s, const asngn_turn *t) {
  asngn_ctx *c = s->ctx;
  asngn_buf line;
  asngn_err e;
  xcdn_node_t *node;

  e = sess_log_reserve(s);
  if (e != ASNGN_OK) return e;
  asngn_buf_init(&line);
  node = turn_build_node(t);
  e = sess_node_line(node, &line);
  if (e != ASNGN_OK) {
    asngn_buf_free(&line);
    return asngn_seterr(c, e, "session %s: turn serialization failed",
                        s->slug);
  }
  e = asngn_stream_append(c, &s->transcript_st, line.data, line.len);
  asngn_buf_free(&line);
  if (e != ASNGN_OK) return e;

  s->log[s->log_n] = *t;
  s->log[s->log_n].text = asngn_strdup(t->text);
  if (s->log[s->log_n].text == NULL) return ASNGN_ERR_NOMEM;
  s->log_n++;
  return ASNGN_OK;
}

asngn_err asngn_session_rewrite_transcript(asngn_session *s) {
  asngn_ctx *c = s->ctx;
  asngn_buf buf;
  asngn_err e = ASNGN_OK;
  size_t i;
  char *path;

  asngn_buf_init(&buf);
  for (i = 0; i < s->log_n && e == ASNGN_OK; i++) {
    xcdn_node_t *node = turn_build_node(&s->log[i]);
    e = sess_node_line(node, &buf);
    if (e == ASNGN_OK) e = asngn_buf_appendc(&buf, '\n');
  }
  if (e != ASNGN_OK) {
    asngn_buf_free(&buf);
    return asngn_seterr(c, e, "session %s: transcript rewrite failed",
                        s->slug);
  }
  path = sess_path(s, "transcript.xcdn");
  if (path == NULL) {
    asngn_buf_free(&buf);
    return ASNGN_ERR_NOMEM;
  }
  /* close the appender across the atomic replace, then reopen */
  asngn_stream_close(&s->transcript_st);
  e = asngn_write_atomic(c, path, buf.data, buf.len);
  if (e == ASNGN_OK)
    e = asngn_stream_open(c, &s->transcript_st, path, true);
  free(path);
  asngn_buf_free(&buf);
  return e;
}

static asngn_err sess_load_transcript(asngn_session *s) {
  asngn_ctx *c = s->ctx;
  char *path = sess_path(s, "transcript.xcdn");
  struct xcdn_document *docp = NULL;
  xcdn_document_t *doc;
  asngn_err e;
  size_t i;

  if (path == NULL) return ASNGN_ERR_NOMEM;
  e = asngn_stream_load(c, path, "transcript", &docp);
  free(path);
  if (e != ASNGN_OK) return e;
  doc = (xcdn_document_t *)docp;
  if (doc == NULL) return ASNGN_OK;
  for (i = 0; i < doc->values_len; i++) {
    xcdn_node_t *node = doc->values[i];
    asngn_turn t;
    if (!xcdn_node_has_tag(node, "turn")) {
      asngn_log(c, ASNGN_LOG_WARN, "session",
                "%s: transcript value %zu is not #turn; skipped", s->slug,
                i);
      continue;
    }
    if (!turn_from_node(node, &t)) {
      free(t.text);
      asngn_log(c, ASNGN_LOG_WARN, "session",
                "%s: transcript value %zu invalid; skipped", s->slug, i);
      continue;
    }
    e = sess_log_reserve(s);
    if (e != ASNGN_OK) {
      free(t.text);
      xcdn_document_free(doc);
      return e;
    }
    s->log[s->log_n++] = t;
  }
  xcdn_document_free(doc);
  if (s->log_n > 0 && s->log[s->log_n - 1].n > s->turns)
    s->turns = s->log[s->log_n - 1].n;
  return ASNGN_OK;
}

/* ── blobs ───────────────────────────────────────────────────────────── */

asngn_err asngn_session_add_blob(asngn_session *s, const char *invocation_id,
                                 const char *label, const char *text,
                                 size_t len, asngn_blob **out) {
  asngn_ctx *c = s->ctx;
  char name[64];
  char *bdir = NULL, *bpath = NULL;
  asngn_blob *b;
  asngn_err e;

  if (out != NULL) *out = NULL;
  if (s->blobs_n == s->blobs_cap) {
    size_t cap = s->blobs_cap != 0 ? s->blobs_cap * 2 : 8;
    asngn_blob *nb = realloc(s->blobs, cap * sizeof *nb);
    if (nb == NULL) return ASNGN_ERR_NOMEM;
    s->blobs = nb;
    s->blobs_cap = cap;
  }
  bdir = sess_path(s, "blobs");
  if (bdir == NULL) return ASNGN_ERR_NOMEM;
  snprintf(name, sizeof name, "%s.txt", invocation_id);
  bpath = os_path_join(bdir, name);
  free(bdir);
  if (bpath == NULL) return ASNGN_ERR_NOMEM;
  e = os_write_file(bpath, text, len);
  if (e != ASNGN_OK) {
    free(bpath);
    return asngn_seterr(c, e, "session %s: blob write failed", s->slug);
  }
  b = &s->blobs[s->blobs_n];
  memset(b, 0, sizeof *b);
  b->id = asngn_strdup(invocation_id);
  b->label = asngn_strdup(label);
  b->path = bpath;
  b->size = len;
  b->slice_off = 0;
  if (b->id == NULL || b->label == NULL) {
    free(b->id);
    free(b->label);
    free(b->path);
    return ASNGN_ERR_NOMEM;
  }
  s->blobs_n++;
  if (out != NULL) *out = b;
  return ASNGN_OK;
}

void asngn_session_clear_blobs(asngn_session *s) {
  size_t i;
  for (i = 0; i < s->blobs_n; i++) {
    free(s->blobs[i].id);
    free(s->blobs[i].label);
    free(s->blobs[i].path);
  }
  s->blobs_n = 0;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

void asngn_session_free(asngn_session *s) {
  size_t i;
  if (s == NULL) return;
  asngn_stream_close(&s->transcript_st);
  asngn_stream_close(&s->ledger_st);
  for (i = 0; i < s->log_n; i++) free(s->log[i].text);
  free(s->log);
  asngn_session_clear_blobs(s);
  free(s->blobs);
  for (i = 0; i < s->allow_n; i++) free(s->allow[i]);
  free(s->allow);
  free(s->led);
  free(s->summary);
  free(s->project);
  free(s->last_user_msg);
  free(s->last_answer);
  free(s->dir);
  os_rwlock_destroy(&s->lock);
  free(s);
}

asngn_err asngn_session_load(asngn_ctx *c, const char *slug,
                             asngn_session **out) {
  asngn_session *s;
  char *mpath = NULL, *tpath = NULL, *lpath = NULL, *bdir = NULL;
  char *wdir = NULL;
  struct xcdn_document *mdoc = NULL;
  asngn_err e;
  bool session_workspace;

  *out = NULL;
  if (!asngn_slug_valid(slug))
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "session: invalid slug \"%s\"", slug);
  s = calloc(1, sizeof *s);
  if (s == NULL) return ASNGN_ERR_NOMEM;
  s->ctx = c;
  snprintf(s->slug, sizeof s->slug, "%s", slug);
  os_rwlock_init(&s->lock);
  s->created_at = asngn_clock_now(&c->clock);
  s->redact_context = c->cfg.redact_context;
  s->workspace = c->workspace;
  s->summary = asngn_strdup("");
  s->dir = os_path_join(c->sessions_dir, slug);
  if (s->dir == NULL || s->summary == NULL) {
    e = ASNGN_ERR_NOMEM;
    goto fail;
  }
  e = os_mkdir_p(s->dir);
  if (e != ASNGN_OK) {
    e = asngn_seterr(c, e, "session %s: cannot create directory", slug);
    goto fail;
  }
  session_workspace = c->session_workspaces;
  if (session_workspace) {
    wdir = sess_path(s, "workspace");
    if (wdir == NULL) { e = ASNGN_ERR_NOMEM; goto fail; }
    e = os_mkdir_p(wdir);
    if (e == ASNGN_OK)
      e = asngn_workspace_info_init(c, wdir, &s->workspace);
    if (e != ASNGN_OK) {
      e = asngn_seterr(c, e, "session %s: cannot create workspace/", slug);
      goto fail;
    }
    s->workspace_loaded = true;
  }
  bdir = sess_path(s, "blobs");
  if (bdir == NULL) { e = ASNGN_ERR_NOMEM; goto fail; }
  e = os_mkdir_p(bdir);
  free(bdir);
  bdir = NULL;
  if (e != ASNGN_OK) {
    e = asngn_seterr(c, e, "session %s: cannot create blobs/", slug);
    goto fail;
  }

  /* manifest (phase 1: scalars) */
  mpath = sess_path(s, "session.xcdn");
  if (mpath == NULL) { e = ASNGN_ERR_NOMEM; goto fail; }
  e = asngn_stream_load(c, mpath, "session manifest", &mdoc);
  if (e != ASNGN_OK) goto fail;
  if (mdoc != NULL) {
    xcdn_document_t *d = (xcdn_document_t *)mdoc;
    if (d->values_len > 0) sess_apply_manifest(s, d->values[0]);
  }
  if (session_workspace) {
    /* Ignore legacy manifests that pointed at the engine root or an
     * unrelated checkout.  Session mode has one deterministic writable
     * root and migrates old sessions to it on open. */
    e = asngn_workspace_info_init(c, wdir, &s->workspace);
    if (e != ASNGN_OK) goto fail;
    s->workspace_loaded = true;
  } else if (s->workspace_loaded &&
      strcmp(s->workspace.canonical_root, c->workspace.canonical_root) != 0) {
    e = asngn_seterr(c, ASNGN_ERR_CONFIG,
                     "session %s belongs to workspace %s, not %s", slug,
                     s->workspace.canonical_root,
                     c->workspace.canonical_root);
    goto fail;
  }
  if (!session_workspace) s->workspace = c->workspace;

  /* transcript */
  e = sess_load_transcript(s);
  if (e != ASNGN_OK) goto fail;
  if (mdoc != NULL) {
    xcdn_document_t *d = (xcdn_document_t *)mdoc;
    if (d->values_len > 0) sess_apply_pins(s, d->values[0]);
    xcdn_document_free(d);
    mdoc = NULL;
  }

  /* summary */
  sess_load_summary(s);

  /* appenders */
  tpath = sess_path(s, "transcript.xcdn");
  lpath = sess_path(s, "ledger.xcdn");
  if (tpath == NULL || lpath == NULL) { e = ASNGN_ERR_NOMEM; goto fail; }
  e = asngn_stream_open(c, &s->transcript_st, tpath, true);
  if (e != ASNGN_OK) goto fail;
  e = asngn_stream_open(c, &s->ledger_st, lpath, true);
  if (e != ASNGN_OK) goto fail;

  /* ledger totals (spent tokens, QpT window, feedback) */
  e = asngn_ledger_replay(s);
  if (e != ASNGN_OK) goto fail;

  /* fresh manifest for brand-new sessions */
  if (!os_file_exists(mpath)) {
    e = asngn_session_save_manifest(s);
    if (e != ASNGN_OK) goto fail;
  }

  free(mpath);
  free(tpath);
  free(lpath);
  free(wdir);
  *out = s;
  return ASNGN_OK;

fail:
  if (mdoc != NULL) xcdn_document_free((xcdn_document_t *)mdoc);
  free(mpath);
  free(tpath);
  free(lpath);
  free(wdir);
  asngn_session_free(s);
  return e;
}

asngn_err asngn_session_workspace_activate(asngn_session *s) {
  asngn_ctx *c;
  asngn_err e;
  if (s == NULL || s->ctx == NULL) return ASNGN_ERR_INVALID;
  c = s->ctx;
  if (!c->session_workspaces)
    return ASNGN_OK;
  os_rwlock_wrlock(&s->lock);
  e = asngn_workspace_info_refresh(c, &s->workspace);
  if (e == ASNGN_OK) c->workspace = s->workspace;
  os_rwlock_wrunlock(&s->lock);
  if (e != ASNGN_OK) return e;
  return asngn_siblings_workspace_sync(c, c->workspace.canonical_root);
}
