/*
 * session.c — the asngn session store.
 *
 * Operational data lives under <root>/sessions/<slug>/. Conversation memory
 * is authoritative in Asper.
 *
 * One directory per session contains:
 *   session.xcdn      #asngn_session manifest (atomic replace)
 *   ledger.xcdn       append stream (ledger.c)
 *   workspace/        optional isolated working tree
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

xcdn_node_t *asngn_turn_node(const asngn_turn *t) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node;
  bool ok = obj != NULL;
  ok = ok && asngn_xobj_put(obj,"workspace",mk_str(t->workspace));
  ok = ok && asngn_xobj_put(obj,"commit",mk_str(t->commit));
  ok = ok && asngn_xobj_put(obj,"project",mk_str(t->project));
  ok = ok && asngn_xobj_put(obj, "turn_id", mk_str(t->turn_id));
  ok = ok && asngn_xobj_put(obj, "n", xcdn_value_int((int64_t)t->n));
  ok = ok && asngn_xobj_put(obj, "role", mk_str(t->role));
  ok = ok && asngn_xobj_put(obj, "at", mk_time(t->at));
  ok = ok && asngn_xobj_put(obj, "text", mk_str(t->text));
  ok = ok && asngn_xobj_put(obj, "pinned", xcdn_value_bool(t->pinned));
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

bool asngn_turn_parse(const xcdn_node_t *node, asngn_turn *out) {
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
  sess_copy_field(out->workspace,sizeof out->workspace,asngn_xstr(asngn_xfield(obj,"workspace")));
  sess_copy_field(out->commit,sizeof out->commit,asngn_xstr(asngn_xfield(obj,"commit")));
  sess_copy_field(out->project,sizeof out->project,asngn_xstr(asngn_xfield(obj,"project")));
  sess_copy_field(out->turn_id, sizeof out->turn_id,
                  asngn_xstr(asngn_xfield(obj,"turn_id")));
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

static asngn_err turn_from_object(asngn_ctx *c, const char *object_ref,
                                  asngn_turn *out) {
  void *data = NULL;
  size_t len = 0;
  xcdn_document_t *doc = NULL;
  xcdn_error_t xe;
  asngn_err e;
  memset(out, 0, sizeof *out);
  e = asngn_siblings_object_read(c, object_ref, 0, 0, &data, &len);
  if (e != ASNGN_OK) return e;
  memset(&xe, 0, sizeof xe);
  doc = xcdn_parse_str((const char *)data, len, &xe);
  free(data);
  if (!doc || doc->values_len != 1 ||
      !xcdn_node_has_tag(doc->values[0], "turn") ||
      !asngn_turn_parse(doc->values[0], out)) {
    if (doc) xcdn_document_free(doc);
    return asngn_seterr(c, ASNGN_ERR_PARSE,
                        "session: invalid Asper turn object");
  }
  xcdn_document_free(doc);
  return ASNGN_OK;
}

/* ── manifest ─────────────────────────────────────────────────────────── */

asngn_err asngn_session_save_manifest(asngn_session *s) {
  asngn_ctx *c = s->ctx;
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node = NULL;
  asngn_buf buf;
  asngn_err e = ASNGN_ERR_NOMEM;
  char *path = NULL;
  bool ok = obj != NULL;

  /* Persist the current worktree identity, not merely the fingerprint from
   * session open; editor/build changes are first-class workspace state.
   * An isolated session may be saved while another session is active. */
  if (c->session_workspaces) {
    (void)asngn_workspace_info_refresh(c, &s->workspace);
  } else if (asngn_workspace_refresh(c) == ASNGN_OK) {
    s->workspace = c->workspace;
  }

  asngn_buf_init(&buf);
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
  ok = ok && asngn_xobj_put(obj, "redact_context",
                            xcdn_value_bool(s->redact_context));
  ok = ok && asngn_xobj_put(obj, "mode",
                            mk_str(asngn_usage_mode_name(s->usage_mode)));
  ok = ok && asngn_xobj_put(
                 obj, "security_profile",
                 mk_str(asngn_security_profile_name(s->security_profile)));
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
  v = asngn_xfield(obj, "mode");
  if (v != NULL && v->type == XCDN_VAL_STRING) {
    if (strcmp(v->data.string, "chat") == 0)
      s->usage_mode = ASNGN_USAGE_CHAT;
    else if (strcmp(v->data.string, "coding") == 0)
      s->usage_mode = ASNGN_USAGE_CODING;
    else if (strcmp(v->data.string, "automate") == 0)
      s->usage_mode = ASNGN_USAGE_AUTOMATE;
  }
  v = asngn_xfield(obj, "security_profile");
  if (v != NULL && v->type == XCDN_VAL_STRING) {
    if (strcmp(v->data.string, "chat") == 0)
      s->security_profile = ASNGN_SECURITY_CHAT;
    else if (strcmp(v->data.string, "coding-readonly") == 0)
      s->security_profile = ASNGN_SECURITY_CODING_READONLY;
    else if (strcmp(v->data.string, "coding-sandboxed") == 0)
      s->security_profile = ASNGN_SECURITY_CODING_SANDBOXED;
    else if (strcmp(v->data.string, "automation-ci") == 0)
      s->security_profile = ASNGN_SECURITY_AUTOMATION_CI;
  }
  /* pinned list is applied after the transcript loads (needs ordinals) */
}

/* ── transcript view ──────────────────────────────────────────────────── */

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

asngn_err asngn_session_stage_turn(asngn_session *s, const asngn_turn *t) {
  asngn_err e = sess_log_reserve(s);
  if (e != ASNGN_OK) return e;
  s->log[s->log_n] = *t;
  s->log[s->log_n].text = asngn_strdup(t->text);
  if (!s->log[s->log_n].text) return ASNGN_ERR_NOMEM;
  s->log_n++;
  return ASNGN_OK;
}

asngn_err asngn_session_append_turn(asngn_session *s, const asngn_turn *t) {
  asngn_ctx *c = s->ctx;
  asngn_buf line;
  asngn_err e;
  xcdn_node_t *node;
  char object_ref[72] = {0};
  char event_id[37] = {0};

  size_t staged = s->log_n;
  for (size_t i=0; t->turn_id[0] && i<s->log_n; i++) {
    if (!strcmp(t->turn_id,s->log[i].turn_id) && !strcmp(t->role,s->log[i].role)) {
      if (s->log[i].n!=t->n || strcmp(s->log[i].text,t->text))
        return asngn_seterr(c,ASNGN_ERR_PARSE,"conflicting transaction projection %s",t->turn_id);
      if (s->log[i].event_id[0]) return ASNGN_OK;
      staged = i; break;
    }
  }
  e = sess_log_reserve(s);
  if (e != ASNGN_OK) return e;
  asngn_buf_init(&line);
  if (c->asper_ok) {
    node = asngn_turn_node(t);
    e = sess_node_line(node, &line);
    if (e != ASNGN_OK) {
      asngn_buf_free(&line);
      return asngn_seterr(c, e, "session %s: turn serialization failed",
                          s->slug);
    }
    e = asngn_siblings_object_put(c, line.data, line.len, object_ref);
    if (e == ASNGN_OK)
      e = asngn_siblings_event_append(
          c, s->slug,
          strcmp(t->role, "assistant") == 0 ? ASNGN_MEM_ASSISTANT
                                             : ASNGN_MEM_USER,
          t->text ? t->text : "", object_ref, t->pinned, event_id);
  } else {
    /* Asper-disabled operation is intentionally ephemeral.  The in-memory
     * turn remains usable for this process, but ASNGN never becomes a second
     * persistent memory owner. */
    e = ASNGN_OK;
  }
  asngn_buf_free(&line);
  if (e != ASNGN_OK) return e;

  if (staged < s->log_n) {
    if (event_id[0]) memcpy(s->log[staged].event_id,event_id,37);
    return ASNGN_OK;
  }
  s->log[s->log_n] = *t;
  s->log[s->log_n].text = asngn_strdup(t->text);
  if (s->log[s->log_n].text == NULL) return ASNGN_ERR_NOMEM;
  if (event_id[0]) memcpy(s->log[s->log_n].event_id, event_id, 37);
  s->log_n++;
  return ASNGN_OK;
}

/* Load the authoritative transcript view from Asper. Every conversation event
 * must refer to the complete serialized turn object. */
static asngn_err sess_load_asper(asngn_session *s) {
  asngn_memory_event *events = NULL;
  size_t n = 0;
  asngn_err e;
  if (!s->ctx->asper_ok) return ASNGN_ERR_UNSUPPORTED;
  e = asngn_siblings_event_list(s->ctx, s->slug, &events, &n);
  if (e != ASNGN_OK) return e;
  for (size_t i = 0; i < n; i++) {
    asngn_turn t;
    if (events[i].kind != ASNGN_MEM_USER &&
        events[i].kind != ASNGN_MEM_ASSISTANT)
      continue;
    memset(&t, 0, sizeof t);
    if (!events[i].object_ref[0]) {
      e = asngn_seterr(s->ctx, ASNGN_ERR_PARSE,
                       "session %s: event %s has no turn object",
                       s->slug, events[i].id);
      goto out;
    }
    e = turn_from_object(s->ctx, events[i].object_ref, &t);
    if (e != ASNGN_OK) goto out;
    memcpy(t.event_id, events[i].id, 37);
    t.pinned = events[i].pinned;
    e = sess_log_reserve(s);
    if (e != ASNGN_OK) {
      free(t.text);
      goto out;
    }
    s->log[s->log_n++] = t;
    if (t.n > s->turns) s->turns = t.n;
  }
out:
  asngn_siblings_events_free(events, n);
  return e;
}

/* ── blobs ───────────────────────────────────────────────────────────── */

asngn_err asngn_session_add_blob(asngn_session *s, const char *invocation_id,
                                 const char *label, const char *text,
                                 size_t len, asngn_blob **out) {
  asngn_ctx *c = s->ctx;
  char object_ref[72] = {0};
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
  if (c->asper_ok) {
    e = asngn_siblings_object_put(c, text, len, object_ref);
    if (e != ASNGN_OK) return e;
  } else {
    return ASNGN_ERR_UNSUPPORTED;
  }
  b = &s->blobs[s->blobs_n];
  memset(b, 0, sizeof *b);
  b->id = asngn_strdup(invocation_id);
  b->label = asngn_strdup(label);
  if (object_ref[0]) memcpy(b->object_ref, object_ref, sizeof b->object_ref);
  b->size = len;
  b->slice_off = 0;
  if (b->id == NULL || b->label == NULL) {
    free(b->id);
    free(b->label);
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
  }
  s->blobs_n = 0;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

void asngn_session_free(asngn_session *s) {
  size_t i;
  if (s == NULL) return;
  asngn_stream_close(&s->ledger_st);
  asngn_stream_close(&s->journal_st);
  for (i = 0; i < s->log_n; i++) free(s->log[i].text);
  free(s->log);
  asngn_session_clear_blobs(s);
  free(s->blobs);
  for (i = 0; i < s->allow_n; i++) free(s->allow[i]);
  free(s->allow);
  free(s->led);
  free(s->project);
  free(s->active_file);free(s->objective);asngn_code_index_free(s->code_index);
  free(s->last_user_msg);
  free(s->last_answer);
  free(s->dir);
  os_rwlock_destroy(&s->lock);
  free(s);
}

asngn_err asngn_session_load(asngn_ctx *c, const char *slug,
                             asngn_session **out) {
  asngn_session *s;
  char *mpath = NULL, *lpath = NULL;
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
  s->usage_mode = ASNGN_USAGE_CODING;
  s->security_profile = ASNGN_SECURITY_CODING_SANDBOXED;
  s->workspace = c->workspace;
  s->dir = os_path_join(c->sessions_dir, slug);
  if (s->dir == NULL) {
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

  /* The manifest is a projection and may be ahead after an interrupted write. */
  s->turns=0;
  /* Authoritative source is Asper. */
  if (c->asper_ok) {
    e = sess_load_asper(s);
    if (e != ASNGN_OK) goto fail;
  }
  if (mdoc != NULL) {
    xcdn_document_free((xcdn_document_t *)mdoc);
    mdoc = NULL;
  }

  /* appenders */
  lpath = sess_path(s, "ledger.xcdn");
  if (lpath == NULL) { e = ASNGN_ERR_NOMEM; goto fail; }
  e = asngn_stream_open(c, &s->ledger_st, lpath, true);
  if (e != ASNGN_OK) goto fail;

  /* ledger totals (spent tokens, QpT window, feedback) */
  e = asngn_ledger_replay(s);
  if (e != ASNGN_OK) goto fail;

  { char *jp = sess_path(s, "turns.xcdn");
    if (!jp) { e=ASNGN_ERR_NOMEM; goto fail; }
    e=asngn_stream_open(c,&s->journal_st,jp,true); free(jp);
    if (e==ASNGN_OK) e=asngn_turn_recover(s);
    if (e!=ASNGN_OK) goto fail;
  }

  /* fresh manifest for brand-new sessions */
  if (!os_file_exists(mpath)) {
    e = asngn_session_save_manifest(s);
    if (e != ASNGN_OK) goto fail;
  }

  free(mpath);
  free(lpath);
  free(wdir);
  *out = s;
  return ASNGN_OK;

fail:
  if (mdoc != NULL) xcdn_document_free((xcdn_document_t *)mdoc);
  free(mpath);
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
