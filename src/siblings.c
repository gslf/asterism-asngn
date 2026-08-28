/*
 * siblings.c — in-process integration with Asper and astools.
 *
 * asngn owns the sibling lifecycle: both siblings are opened during
 * asngn_open against directories under the engine root and closed at
 * asngn_close; their log callbacks are funneled into asngn's logging
 * under the subsystem tags "asper" and "astools". Both integrations are
 * optional and degrade: a sibling that fails to open leaves the
 * engine running with the corresponding zones and steps absent.
 *
 * Policy: asngn narrows, never widens, sibling policy. Sandbox
 * level and grants come from the astools configuration; asngn only ever
 * layers tighter per-invocation deadlines and grant narrowing on top of
 * what the sibling already permits.
 *
 * Ownership: every string that crosses the sibling boundary is copied at
 * the boundary. Sibling allocations are released here with asper_free /
 * asper_records_free / astools_free; callers of this module only ever
 * free() what they receive — the allocators never mix.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

#include "asper.h"
#include "astools.h"
#include "xcdn.h"

/* ═══════════════════════ helpers ═══════════════════════ */

/* Join a configured sibling path onto the engine root: absolute paths
 * (leading '/', or a Windows drive prefix) pass through, "." is the
 * engine root itself, everything else is root-relative. Returns a
 * malloc'd string; NULL on OOM (or when path is NULL). */
static char *sib_join(const asngn_ctx *c, const char *path) {
  if (!path) return NULL;
  if (os_path_is_abs(path))
    return asngn_strdup(path);
  if (strcmp(path, ".") == 0) return asngn_strdup(c->root);
  return os_path_join(c->root, path);
}

/* Sibling log levels share asngn's numeric convention (0..3); clamp
 * defensively anyway. */
static int sib_level(int level) {
  if (level < ASNGN_LOG_ERROR) return ASNGN_LOG_ERROR;
  if (level > ASNGN_LOG_DEBUG) return ASNGN_LOG_DEBUG;
  return level;
}

/* The siblings hand their loggers the already-formatted line, timestamp
 * and level included; re-logging that verbatim double-prefixes the file
 * log and leaves the TUI trace showing nothing but dates. Skip their
 * "<ISO timestamp> LEVEL " lead-in and keep the message proper. */
static const char *sib_strip_prefix(const char *msg) {
  const char *p = msg;
  size_t i;
  static const char *const LEVELS[] = {"ERROR", "WARN", "INFO", "DEBUG",
                                       NULL};
  /* timestamp token: starts "dddd-" and runs to the first space */
  for (i = 0; i < 4; i++)
    if (p[i] < '0' || p[i] > '9') return msg;
  if (p[4] != '-') return msg;
  while (*p != '\0' && *p != ' ') p++;
  while (*p == ' ') p++;
  for (i = 0; LEVELS[i] != NULL; i++) {
    size_t n = strlen(LEVELS[i]);
    if (strncmp(p, LEVELS[i], n) == 0 &&
        (p[n] == ' ' || p[n] == '\0')) {
      p += n;
      while (*p == ' ') p++;
      return p;
    }
  }
  return msg;
}

/* A record whose body is empty after stripping carries no signal; keep
 * it out of the WARN/ERROR band so it never reaches the trace pane. */
static void sib_log(asngn_ctx *c, int level, const char *subsys,
                    const char *msg) {
  const char *m = msg != NULL ? sib_strip_prefix(msg) : "";
  const char *q;
  int lv = sib_level(level);
  for (q = m; *q == ' '; q++) {}
  if (*q == '\0' && lv <= ASNGN_LOG_WARN) lv = ASNGN_LOG_INFO;
  asngn_log(c, lv, subsys, "%s", m);
}

static void sib_asper_log(int level, const char *msg, void *ud) {
  sib_log((asngn_ctx *)ud, level, "asper", msg);
}

static void sib_astools_log(int level, const char *msg, void *ud) {
  sib_log((asngn_ctx *)ud, level, "astools", msg);
}

static astools_catalog_level sib_catalog_level(asngn_catalog_level l) {
  switch (l) {
    case ASNGN_CATALOG_INDEX: return ASTOOLS_CATALOG_INDEX;
    case ASNGN_CATALOG_FULL: return ASTOOLS_CATALOG_FULL;
    case ASNGN_CATALOG_SUMMARY:
    default: return ASTOOLS_CATALOG_SUMMARY;
  }
}

/* Longest prefix of s[0..len) not exceeding max bytes that does not split
 * a UTF-8 sequence. */
static size_t sib_utf8_clip(const char *s, size_t len, size_t max) {
  if (len <= max) return len;
  while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
  return max;
}

/* ═══════════════════════ open / close ═══════════════════════ */

static asngn_err sib_open_asper(asngn_ctx *c) {
  char *root, *conf = NULL;
  asngn_err e;

  root = sib_join(c, c->cfg.asper_root);
  if (!root)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
  if (c->cfg.asper_config) {
    conf = sib_join(c, c->cfg.asper_config);
    if (!conf) {
      free(root);
      return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
    }
  }

  e = os_mkdir_p(root);
  if (e != ASNGN_OK) {
    asngn_log(c, ASNGN_LOG_ERROR, "asper",
              "cannot create memory root %s; memory disabled (degraded)",
              root);
  } else {
    asper_open_params p;
    asper_model_binding models;
    asper_err ae;
    memset(&p, 0, sizeof(p));
    p.memory_root = root;
    p.config_path = conf;
    memset(&models, 0, sizeof models);
    models.manager = c->shared_models;
    {
      int curator_slot = asngn_models_slot_for_role(c, ASNGN_ROLE_COMPRESSOR);
      int embed_slot = asngn_models_slot_for_role(c, ASNGN_ROLE_EMBEDDER);
      if (curator_slot >= 0)
        models.curator_model_id = c->models[curator_slot].cfg.id;
      if (embed_slot >= 0) {
        models.embedding_model_id = c->models[embed_slot].cfg.id;
        models.embedding_dim = c->models[embed_slot].cfg.dim;
      }
      asngn_models_embed_hash(c, models.embedding_model_hash);
    }
    /* Asper's relative model paths (the shared weights files) resolve
     * under the engine root, exactly like asngn's own pool paths. */
    ae = asper_open_at_with_models(&p, c->root, &models, &c->asper);
    if (ae != ASPER_OK) {
      /* No context was created, so there is no asper_last_error to read:
       * the stable code name is all we have. */
      c->asper = NULL;
      asngn_log(c, ASNGN_LOG_ERROR, "asper",
                "asper_open failed: %s; memory disabled (degraded)",
                asper_err_name(ae));
    } else {
      c->asper_ok = true;
      asper_set_logger(c->asper, sib_asper_log, c);
    }
  }

  free(root);
  free(conf);
  return ASNGN_OK;
}

static asngn_err sib_open_astools_at(asngn_ctx *c, const char *workspace) {
  char *root, *work, *conf = NULL;
  asngn_err e;

  root = sib_join(c, c->cfg.astools_root);
  work = asngn_strdup(workspace);
  if (c->cfg.astools_config) conf = sib_join(c, c->cfg.astools_config);
  if (!root || !work || (c->cfg.astools_config && !conf)) {
    free(root);
    free(work);
    free(conf);
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
  }

  e = os_mkdir_p(root);
  if (e == ASNGN_OK) e = os_mkdir_p(work);
  if (e != ASNGN_OK) {
    asngn_log(c, ASNGN_LOG_ERROR, "astools",
              "cannot create registry/workspace under %s; tools disabled "
              "(degraded)",
              c->root);
  } else {
    const char *regs[2];
    astools_open_params p;
    astools_err ae;
    regs[0] = root;
    regs[1] = NULL;
    memset(&p, 0, sizeof(p));
    /* an explicit astools config owns the registry roots (its entries
     * may carry full trust); the default root applies only without one,
     * so the same directory is never scanned at two trust levels */
    p.registry_paths = conf != NULL ? NULL : regs;
    p.config_path = conf;
    p.workspace_root = work;
    ae = astools_open(&p, &c->astools);
    if (ae != ASTOOLS_OK) {
      c->astools = NULL;
      asngn_log(c, ASNGN_LOG_ERROR, "astools",
                "astools_open failed: %s; tools disabled (degraded)",
                astools_err_name(ae));
    } else {
      char *active = asngn_strdup(work);
      if (active == NULL) {
        astools_close(c->astools);
        c->astools = NULL;
        free(root);
        free(work);
        free(conf);
        return asngn_seterr(c, ASNGN_ERR_NOMEM,
                            "siblings: out of memory");
      }
      free(c->astools_workspace_active);
      c->astools_workspace_active = active;
      c->astools_ok = true;
      astools_set_logger(c->astools, sib_astools_log, c);
    }
  }

  free(root);
  free(work);
  free(conf);
  return ASNGN_OK;
}

asngn_err asngn_siblings_open(asngn_ctx *c) {
  asngn_err e;

  if (!c) return ASNGN_ERR_INVALID;
  c->asper_ok = false;
  c->astools_ok = false;

  if (c->cfg.asper_enable) {
    e = sib_open_asper(c);
    if (e != ASNGN_OK) return e;
  }
  if (c->cfg.astools_enable) {
    e = sib_open_astools_at(c, c->workspace.canonical_root);
    if (e != ASNGN_OK) return e;
  }
  return ASNGN_OK;
}

asngn_err asngn_siblings_readiness(asngn_ctx *c) {
  asper_readiness mr;
  astools_readiness tr;
  const char *why = NULL;
  if (c == NULL) return ASNGN_ERR_INVALID;
  memset(&mr, 0, sizeof mr);
  memset(&tr, 0, sizeof tr);
  if (c->cfg.asper_enable && (!c->asper_ok || c->asper == NULL))
    why = "Asper is unavailable";
  else if (c->cfg.asper_enable &&
           (asper_get_readiness(c->asper, &mr) != ASPER_OK ||
            !mr.store_ok || !mr.embedder_ok || !mr.curator_ok))
    why = "Asper store, embedder, or curator is not ready";
  else if (c->cfg.astools_enable && (!c->astools_ok || c->astools == NULL))
    why = "astools is unavailable";
  else if (c->cfg.astools_enable &&
           (astools_get_readiness(c->astools, &tr) != ASTOOLS_OK ||
            !tr.workspace_bound))
    why = "astools has no canonical workspace";
  else if (c->cfg.astools_enable && (tr.workspace_access & 2) == 0)
    why = "astools lacks a read-write workspace grant";
  else if (c->cfg.astools_enable && tr.sandbox_level < 1)
    why = "astools sandbox is disabled";
  else if (c->cfg.astools_enable && tr.tools_enabled == 0)
    why = "astools has no enabled tools";
  if (why == NULL) return ASNGN_OK;
  if (c->allow_degraded) {
    asngn_log(c, ASNGN_LOG_WARN, "session",
              "coding readiness degraded: %s (--allow-degraded)", why);
    return ASNGN_OK;
  }
  return asngn_seterr(c, ASNGN_ERR_SIBLING,
                      "coding readiness failed: %s; pass "
                      "--allow-degraded to opt in", why);
}

void asngn_siblings_close(asngn_ctx *c) {
  if (!c) return;
  if (c->astools) {
    astools_close(c->astools);
    c->astools = NULL;
  }
  if (c->asper) {
    asper_close(c->asper);
    c->asper = NULL;
  }
  c->asper_ok = false;
  c->astools_ok = false;
  free(c->astools_workspace_active);
  c->astools_workspace_active = NULL;
  free(c->astools_catalog);
  c->astools_catalog = NULL;
  free(c->astools_grammar);
  c->astools_grammar = NULL;
  free(c->notes);
  c->notes = NULL;
  c->notes_n = 0;
  c->notes_cap = 0;
}

asngn_err asngn_siblings_workspace_sync(asngn_ctx *c, const char *root) {
  asngn_err e;
  if (c == NULL || root == NULL || root[0] == '\0')
    return ASNGN_ERR_INVALID;
  if (!c->cfg.astools_enable) return ASNGN_OK;

  /* The agent worker serializes turns across sessions.  Rebinding here
   * gives every turn an astools context whose auto-grant and relative-path
   * base are exactly that session's private workspace. */
  if (c->astools_workspace_active != NULL &&
      strcmp(c->astools_workspace_active, root) == 0)
    return ASNGN_OK;
  if (c->astools != NULL) {
    astools_close(c->astools);
    c->astools = NULL;
  }
  c->astools_ok = false;
  free(c->astools_workspace_active);
  c->astools_workspace_active = NULL;
  e = sib_open_astools_at(c, root);
  if (e != ASNGN_OK) return e;
  if (c->cfg.profile == ASNGN_PROFILE_CODING)
    return asngn_siblings_readiness(c);
  return ASNGN_OK;
}

/* ═══════════════════════ catalog / grammar ═══════════════════════ */

/* Fetch, refresh the context cache under sib_mu, and hand the caller an
 * independent malloc'd copy (never the astools allocation). */
static asngn_err sib_refresh(asngn_ctx *c, char *text /* astools-owned */,
                             char **cache_slot, char **out) {
  char *mine, *theirs;

  mine = asngn_strdup(text ? text : "");
  theirs = asngn_strdup(text ? text : "");
  astools_free(text);
  if (!mine || !theirs) {
    free(mine);
    free(theirs);
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
  }

  os_mutex_lock(&c->sib_mu);
  free(*cache_slot);
  *cache_slot = mine;
  os_mutex_unlock(&c->sib_mu);

  *out = theirs;
  return ASNGN_OK;
}

asngn_err asngn_siblings_catalog(asngn_ctx *c, char **out_text) {
  char *text = NULL;
  astools_err ae;
  size_t budget;

  if (!c || !out_text) return ASNGN_ERR_INVALID;
  *out_text = NULL;
  if (!c->astools_ok) return ASNGN_OK;

  budget = c->cfg.catalog_chars > 0 ? (size_t)c->cfg.catalog_chars : 0;
  ae = astools_catalog(c->astools, sib_catalog_level(c->cfg.catalog_level),
                       budget, &text);
  if (ae != ASTOOLS_OK)
    return asngn_seterr(c, ASNGN_ERR_SIBLING, "astools: %s: %s",
                        astools_err_name(ae),
                        astools_last_error(c->astools));
  return sib_refresh(c, text, &c->astools_catalog, out_text);
}

asngn_err asngn_siblings_grammar(asngn_ctx *c, char **out_gbnf) {
  char *text = NULL;
  astools_err ae;

  if (!c || !out_gbnf) return ASNGN_ERR_INVALID;
  *out_gbnf = NULL;
  if (!c->astools_ok) return ASNGN_OK;

  ae = astools_grammar_export(c->astools, &text);
  if (ae != ASTOOLS_OK)
    return asngn_seterr(c, ASNGN_ERR_SIBLING, "astools: %s: %s",
                        astools_err_name(ae),
                        astools_last_error(c->astools));
  return sib_refresh(c, text, &c->astools_grammar, out_gbnf);
}

/* ═══════════════════════ annotations ═══════════════════════ */

/* Remember a note in the context cache; a full cache or OOM only costs a
 * re-parse on the next lookup, so failures are silent. */
static void sib_note_remember(asngn_ctx *c, const asngn_tool_note *note) {
  size_t i;

  os_mutex_lock(&c->sib_mu);
  for (i = 0; i < c->notes_n; i++) {
    if (strcmp(c->notes[i].ref, note->ref) == 0 &&
        strcmp(c->notes[i].cmd, note->cmd) == 0) {
      os_mutex_unlock(&c->sib_mu);
      return; /* raced with another lookup; first entry wins */
    }
  }
  if (c->notes_n == c->notes_cap) {
    size_t cap = c->notes_cap ? c->notes_cap * 2 : 8;
    asngn_tool_note *grown =
        (asngn_tool_note *)realloc(c->notes, cap * sizeof(*grown));
    if (!grown) {
      os_mutex_unlock(&c->sib_mu);
      return;
    }
    c->notes = grown;
    c->notes_cap = cap;
  }
  c->notes[c->notes_n++] = *note;
  os_mutex_unlock(&c->sib_mu);
}

asngn_err asngn_siblings_annotations(asngn_ctx *c, const char *ref,
                                     const char *cmd, asngn_tool_note *out) {
  char *text = NULL;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  const xcdn_node_t *root_node = NULL;
  const xcdn_value_t *obj, *cmds;
  const char *ver;
  asngn_tool_note note;
  astools_err ae;
  asngn_err e;
  size_t i, n;
  bool found = false;

  if (!c || !ref || !cmd || !out) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  if (!c->astools_ok)
    return asngn_seterr(c, ASNGN_ERR_UNSUPPORTED, "astools disabled");

  os_mutex_lock(&c->sib_mu);
  for (i = 0; i < c->notes_n; i++) {
    if (strcmp(c->notes[i].ref, ref) == 0 &&
        strcmp(c->notes[i].cmd, cmd) == 0) {
      *out = c->notes[i];
      os_mutex_unlock(&c->sib_mu);
      return ASNGN_OK;
    }
  }
  os_mutex_unlock(&c->sib_mu);

  ae = astools_tool_manifest(c->astools, ref, &text);
  if (ae != ASTOOLS_OK)
    return asngn_seterr(c, ASNGN_ERR_SIBLING, "astools: %s: %s",
                        astools_err_name(ae),
                        astools_last_error(c->astools));

  doc = xcdn_parse_str(text, strlen(text), &xe);
  astools_free(text);
  if (!doc)
    return asngn_seterr(c, ASNGN_ERR_SIBLING,
                        "astools: manifest for %s does not parse: %s "
                        "(line %zu)",
                        ref, xe.message, xe.span.line);

  /* The manifest is the first #astools_tool-tagged value, falling back to
   * the first object value. */
  for (i = 0; i < doc->values_len; i++) {
    if (xcdn_node_has_tag(doc->values[i], "astools_tool")) {
      root_node = doc->values[i];
      break;
    }
  }
  for (i = 0; !root_node && i < doc->values_len; i++) {
    if (doc->values[i]->value &&
        doc->values[i]->value->type == XCDN_VAL_OBJECT)
      root_node = doc->values[i];
  }
  obj = root_node ? root_node->value : NULL;
  if (!obj || obj->type != XCDN_VAL_OBJECT) {
    xcdn_document_free(doc);
    return asngn_seterr(c, ASNGN_ERR_SIBLING,
                        "astools: manifest for %s has no tool object", ref);
  }

  memset(&note, 0, sizeof(note));
  snprintf(note.ref, sizeof(note.ref), "%s", ref);
  snprintf(note.cmd, sizeof(note.cmd), "%s", cmd);
  ver = asngn_xstr(asngn_xfield(obj, "version"));
  if (ver) snprintf(note.version, sizeof(note.version), "%s", ver);

  cmds = asngn_xfield(obj, "commands");
  if (!cmds || cmds->type != XCDN_VAL_ARRAY) {
    xcdn_document_free(doc);
    return asngn_seterr(c, ASNGN_ERR_SIBLING,
                        "astools: manifest for %s has no commands array",
                        ref);
  }

  n = xcdn_array_len(cmds);
  for (i = 0; i < n && !found; i++) {
    const xcdn_node_t *cn = xcdn_array_get(cmds, i);
    const xcdn_value_t *cv = cn ? cn->value : NULL;
    const xcdn_value_t *ann;
    const char *name;
    bool b;

    if (!cv || cv->type != XCDN_VAL_OBJECT) continue;
    name = asngn_xstr(asngn_xfield(cv, "name"));
    if (!name || strcmp(name, cmd) != 0) continue;
    found = true;

    /* An absent annotations object means all annotations false. */
    ann = asngn_xfield(cv, "annotations");
    if (ann && ann->type == XCDN_VAL_OBJECT) {
      if (asngn_xbool(asngn_xfield(ann, "read_only"), &b))
        note.read_only = b;
      if (asngn_xbool(asngn_xfield(ann, "destructive"), &b))
        note.destructive = b;
      if (asngn_xbool(asngn_xfield(ann, "idempotent"), &b))
        note.idempotent = b;
      if (asngn_xbool(asngn_xfield(ann, "long_running"), &b))
        note.long_running = b;
    }
  }
  xcdn_document_free(doc);

  if (!found) {
    e = asngn_seterr(c, ASNGN_ERR_NOT_FOUND,
                     "astools: tool %s has no command %s", ref, cmd);
    return e;
  }

  sib_note_remember(c, &note);
  *out = note;
  return ASNGN_OK;
}

/* ═══════════════════════ memory zone (Asper) ═══════════════════════ */

asngn_err asngn_siblings_memory(asngn_ctx *c, const char *base_prompt,
                                const char *user_message, char **out) {
  char *prompt = NULL, *copy;
  asper_err ae;

  if (!c || !base_prompt || !out) return ASNGN_ERR_INVALID;
  *out = NULL;

  if (!c->asper_ok) {
    /* Degraded: system zone only, no memory block. */
    *out = asngn_strdup(base_prompt);
    if (!*out)
      return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
    return ASNGN_OK;
  }

  ae = asper_build_prompt(c->asper, base_prompt, user_message, &prompt);
  if (ae != ASPER_OK || !prompt) {
    /* Memory must never kill a turn: WARN and fall back to the base. */
    asngn_log(c, ASNGN_LOG_WARN, "asper",
              "build_prompt failed: %s: %s; memory zone skipped",
              asper_err_name(ae), asper_last_error(c->asper));
    asper_free(prompt);
    *out = asngn_strdup(base_prompt);
    if (!*out)
      return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
    return ASNGN_OK;
  }

  copy = asngn_strdup(prompt);
  asper_free(prompt);
  if (!copy)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
  *out = copy;
  return ASNGN_OK;
}

asngn_err asngn_siblings_observe(asngn_ctx *c, int is_assistant,
                                 const char *text) {
  asper_err ae;

  if (!c || !text) return ASNGN_ERR_INVALID;
  if (!c->asper_ok) return ASNGN_OK;

  ae = asper_observe_turn(c->asper,
                          is_assistant ? ASPER_ROLE_ASSISTANT
                                       : ASPER_ROLE_USER,
                          text);
  if (ae != ASPER_OK)
    /* Observation is best-effort; the curator misses one turn, that's all. */
    asngn_log(c, ASNGN_LOG_WARN, "asper", "observe_turn failed: %s: %s",
              asper_err_name(ae), asper_last_error(c->asper));
  return ASNGN_OK;
}

/* ═══════════════════════ recall ═══════════════════════ */

#define SIB_RECALL_CITES     3
#define SIB_CITE_MAX_BYTES 2048

asngn_err asngn_siblings_recall(asngn_ctx *c, const char *question,
                                char **out_block) {
  char *answer = NULL;
  asper_record **cited = NULL;
  size_t cited_n = 0;
  asper_err ae;
  asngn_buf b;
  bool ok;
  size_t i;

  if (!c || !question || !out_block) return ASNGN_ERR_INVALID;
  *out_block = NULL;

  if (!c->asper_ok) {
    *out_block = asngn_strdup("memory: unavailable");
    goto out_fixed;
  }

  ae = asper_recall(c->asper, question, &answer, &cited, &cited_n);
  if (ae == ASPER_ERR_NOT_FOUND) {
    *out_block = asngn_strdup("memory: nothing relevant");
    goto out_fixed;
  }
  if (ae != ASPER_OK) {
    char line[96];
    asngn_log(c, ASNGN_LOG_WARN, "asper", "recall failed: %s: %s",
              asper_err_name(ae), asper_last_error(c->asper));
    snprintf(line, sizeof(line), "memory: unavailable (%s)",
             asper_err_name(ae));
    *out_block = asngn_strdup(line);
    goto out_fixed;
  }

  asngn_buf_init(&b);
  ok = asngn_buf_appends(&b, "memory: ") == ASNGN_OK &&
       asngn_buf_appends(&b, answer ? answer : "") == ASNGN_OK;
  for (i = 0; ok && i < cited_n && i < SIB_RECALL_CITES; i++) {
    const char *content = asper_record_content(cited[i]);
    size_t clen;
    if (!content) continue;
    clen = sib_utf8_clip(content, strlen(content), SIB_CITE_MAX_BYTES);
    ok = asngn_buf_appends(&b, "\n- ") == ASNGN_OK &&
         asngn_buf_append(&b, content, clen) == ASNGN_OK;
  }
  asper_free(answer);
  asper_records_free(cited, cited_n);

  if (!ok) {
    asngn_buf_free(&b);
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
  }
  *out_block = asngn_buf_detach(&b);
  if (!*out_block)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
  return ASNGN_OK;

out_fixed:
  if (!*out_block)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "siblings: out of memory");
  return ASNGN_OK;
}

/* ═══════════════════════ projects ═══════════════════════ */

asngn_err asngn_siblings_project(asngn_ctx *c, const char *slug) {
  asper_err ae;

  if (!c) return ASNGN_ERR_INVALID;
  if (!c->asper_ok)
    return asngn_seterr(c, ASNGN_ERR_UNSUPPORTED, "asper disabled");

  ae = asper_project_select(c->asper, slug); /* NULL slug deselects */
  if (ae != ASPER_OK)
    return asngn_seterr(c, ASNGN_ERR_SIBLING, "asper: %s: %s",
                        asper_err_name(ae), asper_last_error(c->asper));
  return ASNGN_OK;
}

/* Make Asper's active project match `want` (NULL = none). Asper holds a
 * single active project per context while every session carries its
 * own: sessions sync it on open and at each turn, so switching
 * sessions switches the project memory with them. No-op when they
 * already agree. */
asngn_err asngn_siblings_project_sync(asngn_ctx *c, const char *want) {
  char *cur = NULL;
  bool same;
  if (c == NULL) return ASNGN_ERR_INVALID;
  if (!c->asper_ok) return ASNGN_ERR_UNSUPPORTED;
  if (asper_project_active(c->asper, &cur) != ASPER_OK) cur = NULL;
  same = (want == NULL && cur == NULL) ||
         (want != NULL && cur != NULL && strcmp(want, cur) == 0);
  if (cur != NULL) asper_free(cur);
  if (same) return ASNGN_OK;
  return asngn_siblings_project(c, want);
}
