/* Generate opaque artifact payloads under bounded private inference. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

#define ASNGN_DRAFT_MARKER "@asngn:draft"

/* Models commonly wrap otherwise-correct source in one outer Markdown
 * fence despite an explicit raw-bytes instruction.  DRAFT is already an
 * opaque, non-dispatching phase, so remove exactly that transport wrapper.
 * Anything before the opening fence, a missing closing fence, or text after
 * it is not normalized.  Returns 1 on success, 0 for a malformed wrapper,
 * and -1 on allocation failure. */
static int draft_unwrap_outer_fence(char **text) {
  char *src, *replacement;
  const char *p, *open_nl, *body, *end, *line, *q;
  size_t n;

  if (text == NULL || *text == NULL) return 1;
  src = *text;
  p = src;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    p++;
  if (strncmp(p, "```", 3) != 0) return 1;
  open_nl = strchr(p + 3, '\n');
  if (open_nl == NULL) return 0;
  body = open_nl + 1;
  end = src + strlen(src);
  while (end > body && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
    end--;
  line = end;
  while (line > body && line[-1] != '\n')
    line--;
  q = line;
  while (q < end && (*q == ' ' || *q == '\t'))
    q++;
  if ((size_t)(end - q) != 3 || memcmp(q, "```", 3) != 0) return 0;

  n = (size_t)(line - body); /* retains the source newline before ` ``` ` */
  replacement = asngn_strndup(body, n);
  if (replacement == NULL) return -1;
  free(src);
  *text = replacement;
  return 1;
}

/* A complete one-line action is control output, not file content.  Keep the
 * test deliberately narrow: source files may legitimately discuss CALL or
 * action syntax somewhere in their body, and DRAFT has no dispatch authority
 * anyway. */
static bool draft_is_control_output(const char *text) {
  const char *p = text != NULL ? text : "";
  const char *end = p + strlen(p);
  const char *q;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    p++;
  while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
    end--;
  for (q = p; q < end; q++)
    if (*q == '\r' || *q == '\n') return false;
  return ((size_t)(end - p) >= 5 && memcmp(p, "CALL ", 5) == 0) ||
         ((size_t)(end - p) >= 8 && memcmp(p, "{action:", 8) == 0 && end[-1] == '}');
}

static bool draft_ident_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
         ch == '_';
}

/* Serialize one UTF-8 string as the quoted xCDN value accepted by astools.
 * The draft itself never passes through the decision grammar. */
static asngn_err append_xcdn_string(asngn_buf *b, const char *text) {
  const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
  asngn_err e = asngn_buf_appendc(b, '"');
  for (; e == ASNGN_OK && *p != '\0'; p++) {
    switch (*p) {
    case '"':
      e = asngn_buf_appends(b, "\\\"");
      break;
    case '\\':
      e = asngn_buf_appends(b, "\\\\");
      break;
    case '\n':
      e = asngn_buf_appends(b, "\\n");
      break;
    case '\r':
      e = asngn_buf_appends(b, "\\r");
      break;
    case '\t':
      e = asngn_buf_appends(b, "\\t");
      break;
    default:
      if (*p < 0x20)
        e = asngn_buf_printf(b, "\\u%04x", (unsigned)*p);
      else
        e = asngn_buf_appendc(b, (char)*p);
      break;
    }
  }
  if (e == ASNGN_OK) e = asngn_buf_appendc(b, '"');
  return e;
}

static asngn_err draft_file_content(asngn_ctx *c, asngn_turn_state *t, const char *call_args,
                                    char **out) {
  asngn_buf complete;
  asngn_err e = ASNGN_OK;
  *out = NULL;
  asngn_buf_init(&complete);

  for (;;) {
    asngn_buf ib;
    asngn_prompt prompt;
    char *chunk = NULL;
    int tin = 0, tout = 0;
    asngn_turn_phase saved_phase = t->phase;
    bool continuing = complete.len > 0;
    asngn_err generation_result;

    asngn_buf_init(&ib);
    if (!continuing) {
      e = asngn_buf_printf(&ib,
                           "Private artifact-draft phase. Create the exact complete file "
                           "content requested by the user for this pending fs.write intent:\n"
                           "%s\n\nOutput only the raw file bytes as UTF-8 text: no Markdown "
                           "fence, no explanation, no CALL/tool syntax, and no action object.",
                           call_args != NULL ? call_args : "{}");
    } else {
      size_t tail_start = complete.len > 4096 ? complete.len - 4096 : 0;
      uint8_t hash[32];
      char hex[65];
      while (tail_start < complete.len && ((unsigned char)complete.data[tail_start] & 0xC0) == 0x80)
        tail_start++;
      asngn_sha256(complete.data, complete.len, hash);
      asngn_sha256_hex(hash, 32, hex);
      e = asngn_buf_printf(&ib,
                           "Continue the SAME pending fs.write artifact. The exact accepted "
                           "prefix is %zu bytes with sha256 %s. Produce only bytes that follow "
                           "that prefix; do not restart, explain, fence, or repeat the tail.\n"
                           "Original intent:\n%s\n\nExact suffix of the accepted prefix:\n"
                           "---BEGIN EXACT SUFFIX---\n",
                           complete.len, hex, call_args != NULL ? call_args : "{}");
      if (e == ASNGN_OK)
        e = asngn_buf_append(&ib, complete.data + tail_start, complete.len - tail_start);
      if (e == ASNGN_OK) e = asngn_buf_appends(&ib, "\n---END EXACT SUFFIX---");
    }
    if (e != ASNGN_OK) {
      asngn_buf_free(&ib);
      goto fail;
    }

    memset(&prompt, 0, sizeof prompt);
    t->phase = ASNGN_PHASE_DRAFT;
    e = asngn_context_assemble(c, t->s, t, NULL, ib.data, t->gen_slot, &prompt);
    asngn_buf_free(&ib);
    if (e == ASNGN_OK) {
      int n_ctx = c->models[t->gen_slot].cfg.ctx > 0 ? c->models[t->gen_slot].cfg.ctx : 32768;
      int prompt_tokens =
          asngn_models_count_prompt(c, t->gen_slot, prompt.system_text, prompt.user_text);
      int draft_cap = n_ctx - (prompt_tokens > 0 ? prompt_tokens : 0) - c->cfg.safety_margin - 128;
      if (c->cfg.s_draft.max_tokens > 0 && draft_cap > c->cfg.s_draft.max_tokens)
        draft_cap = c->cfg.s_draft.max_tokens;
      if (draft_cap < 1) {
        asngn_prompt_free(&prompt);
        t->phase = saved_phase;
        e = asngn_seterr(c, ASNGN_ERR_CONTEXT,
                         "draft has no output capacity in model context "
                         "(n_ctx=%d prompt=%d safety=%d)",
                         n_ctx, prompt_tokens, c->cfg.safety_margin);
        goto fail;
      }
      /* A backend max_tokens value is invisible to the model.  Put the
       * effective ceiling at the high-attention end of the prompt.  The
       * reserve above pays for this directive, keeping the announced cap
       * within the physical context window. */
      {
        asngn_buf ub;
        char *budgeted;
        int before = asngn_models_count_tokens(c, t->gen_slot, prompt.user_text);
        asngn_buf_init(&ub);
        if (asngn_buf_printf(&ub,
                             "%s\n\nHard output budget: at most %d tokens. This is "
                             "a ceiling, not a target. Use the space required for a "
                             "complete, production-quality implementation; do not "
                             "omit requested behavior, replace code with TODOs, or "
                             "stop without closing the file cleanly.",
                             prompt.user_text, draft_cap) != ASNGN_OK) {
          asngn_buf_free(&ub);
          asngn_prompt_free(&prompt);
          t->phase = saved_phase;
          e = ASNGN_ERR_NOMEM;
          goto fail;
        }
        budgeted = asngn_buf_detach(&ub);
        asngn_buf_free(&ub);
        if (budgeted == NULL) {
          asngn_prompt_free(&prompt);
          t->phase = saved_phase;
          e = ASNGN_ERR_NOMEM;
          goto fail;
        }
        free(prompt.user_text);
        prompt.user_text = budgeted;
        {
          int after = asngn_models_count_tokens(c, t->gen_slot, prompt.user_text);
          if (after > before) prompt.tok_working += (size_t)(after - before);
        }
      }
      asngn_ledger_zones(&t->led, &prompt);
      e = asngn_context_validate(c, t->gen_slot, &prompt, draft_cap);
      if (e == ASNGN_OK)
        e = asngn_generate_watched(c, t, t->gen_slot, ASNGN_TASK_DRAFT, prompt.system_text,
                                   prompt.user_text, NULL, NULL, draft_cap, NULL, NULL, &chunk,
                                   &tin, &tout);
    }
    asngn_prompt_free(&prompt);
    t->phase = saved_phase;
    generation_result = e;
    if (e != ASNGN_OK && e != ASNGN_ERR_LIMIT) {
      free(chunk);
      goto fail;
    }
    if (tout > 0) t->led.gt_aux += (size_t)tout;
    if (chunk != NULL && chunk[0] != '\0') {
      size_t overlap = 0;
      size_t max_overlap = complete.len < strlen(chunk) ? complete.len : strlen(chunk);
      if (max_overlap > 4096) max_overlap = 4096;
      for (size_t n = max_overlap; n > 0; n--)
        if (memcmp(complete.data + complete.len - n, chunk, n) == 0) {
          overlap = n;
          break;
        }
      if (asngn_buf_appends(&complete, chunk + overlap) != ASNGN_OK) {
        free(chunk);
        e = ASNGN_ERR_NOMEM;
        goto fail;
      }
      if (c->asper_ok) {
        char object_ref[72];
        asngn_buf checkpoint;
        uint8_t hash[32];
        char hex[65];
        asngn_sha256(complete.data, complete.len, hash);
        asngn_sha256_hex(hash, 32, hex);
        e = asngn_siblings_object_put(c, complete.data, complete.len, object_ref);
        asngn_buf_init(&checkpoint);
        if (e == ASNGN_OK)
          e = asngn_buf_printf(&checkpoint,
                               "goal: %s\nphase: draft\nstatus: %s\nbytes: %zu\n"
                               "sha256: %s\nsource_object: %s\npending_intent: %s",
                               t->user_msg ? t->user_msg : "",
                               generation_result == ASNGN_ERR_LIMIT ? "continuing"
                                                                    : "chunk_committed",
                               complete.len, hex, object_ref, call_args != NULL ? call_args : "{}");
        if (e == ASNGN_OK) e = asngn_work_push(c, t, checkpoint.data);
        asngn_buf_free(&checkpoint);
        if (e != ASNGN_OK) {
          free(chunk);
          goto fail;
        }
      }
    }
    free(chunk);
    if (generation_result == ASNGN_ERR_LIMIT) {
      if (complete.len == 0) {
        e = asngn_seterr(c, ASNGN_ERR_LIMIT,
                         "draft reached its output limit without returning "
                         "a resumable partial payload");
        goto fail;
      }
      if ((c->cfg.session_tokens > 0 &&
           t->s->spent_tokens + (int64_t)t->led.gt_aux >= c->cfg.session_tokens) ||
          (c->cfg.daily_tokens > 0 &&
           asngn_daily_spend(c) >= c->cfg.daily_tokens)) {
        e = asngn_seterr(c, ASNGN_ERR_LIMIT,
                         "draft continuation paused at %zu bytes by the "
                         "configured token budget",
                         complete.len);
        goto fail;
      }
      continue;
    }
    if (complete.len == 0) {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL, "draft phase returned no file content");
      goto fail;
    }
    {
      char *draft = asngn_buf_detach(&complete);
      int normalized = draft_unwrap_outer_fence(&draft);
      if (normalized < 0) {
        free(draft);
        e = ASNGN_ERR_NOMEM;
        goto fail;
      }
      if (normalized == 0 || draft == NULL || draft[0] == '\0' || draft_is_control_output(draft)) {
        free(draft);
        e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                         "draft phase emitted an assistant wrapper or "
                         "action syntax");
        goto fail;
      }
      *out = draft;
      asngn_buf_free(&complete);
      return ASNGN_OK;
    }
  }

fail:
  asngn_buf_free(&complete);
  return e;
}

/* Some OpenAI-compatible chat templates teach models a virtual
 * `/workspace` mount. astools deliberately accepts only paths beneath its
 * configured workspace, so translate that well-known presentation alias to
 * the canonical workspace-relative form before validation. This does not
 * widen grants: the resulting path still goes through astools' normal path
 * resolver and policy checks. */
static char *normalize_virtual_workspace_aliases(const char *args) {
  static const char win_prefix[] = "C:/workspace/";
  static const char posix_prefix[] = "/workspace/";
  asngn_buf b;
  const char *p;
  asngn_buf_init(&b);
  for (p = args != NULL ? args : "{}"; *p != '\0';) {
    if (*p == '"' && strncmp(p + 1, win_prefix, sizeof win_prefix - 1) == 0) {
      if (asngn_buf_appendc(&b, '"') != ASNGN_OK) goto oom;
      p += sizeof win_prefix;
      continue;
    }
    if (*p == '"' && strncmp(p + 1, posix_prefix, sizeof posix_prefix - 1) == 0) {
      if (asngn_buf_appendc(&b, '"') != ASNGN_OK) goto oom;
      p += sizeof posix_prefix;
      continue;
    }
    if (asngn_buf_appendc(&b, *p++) != ASNGN_OK) goto oom;
  }
  {
    char *out = asngn_buf_detach(&b);
    asngn_buf_free(&b);
    return out;
  }
oom:
  asngn_buf_free(&b);
  return NULL;
}

/* Replace the deliberately short fs.write content marker only after the
 * separate draft phase completes.  Exactly one quoted marker is required,
 * preventing accidental substitutions in paths or source text. */
asngn_err asngn_expand_write_draft(asngn_ctx *c, asngn_turn_state *t, const char *ref,
                                   const char *cmd, const char *args, char **out) {
  static const char needle[] = "\"" ASNGN_DRAFT_MARKER "\"";
  const char *hit = NULL, *p;
  char *draft = NULL;
  char *normalized_args = NULL;
  asngn_buf b;
  asngn_err e;
  size_t marker_count = 0;
  int depth = 0;
  bool in_str = false, esc = false;

  *out = NULL;
  /* Native proposals already contain their complete payload. */
  if (c->cfg.native_actions) return ASNGN_OK;
  if (!asngn_tool_ref_is(ref, "fs") || strcmp(cmd, "write") != 0) return ASNGN_OK;
  if (args == NULL) return ASNGN_OK;

  normalized_args = normalize_virtual_workspace_aliases(args);
  if (normalized_args == NULL) return ASNGN_ERR_NOMEM;
  args = normalized_args;

  for (p = args; (p = strstr(p, ASNGN_DRAFT_MARKER)) != NULL; p += sizeof ASNGN_DRAFT_MARKER - 1)
    marker_count++;
  if (marker_count == 0) {
    free(normalized_args);
    return ASNGN_OK;
  }

  /* Locate an exact top-level `content: "@asngn:draft"` value.  A marker
   * in path (observed in a real failed turn), encoding, or any future field
   * must never be substituted or dispatched. */
  for (p = args; *p != '\0'; p++) {
    if (in_str) {
      if (esc)
        esc = false;
      else if (*p == '\\')
        esc = true;
      else if (*p == '"')
        in_str = false;
      continue;
    }
    if (*p == '"') {
      in_str = true;
      continue;
    }
    if (*p == '{') {
      depth++;
      continue;
    }
    if (*p == '}') {
      depth--;
      continue;
    }
    if (depth == 1 && strncmp(p, "content", 7) == 0 && (p == args || !draft_ident_char(p[-1])) &&
        !draft_ident_char(p[7])) {
      const char *q = p + 7;
      const char *after;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        q++;
      if (*q++ != ':') continue;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        q++;
      if (strncmp(q, needle, sizeof needle - 1) != 0) continue;
      after = q + sizeof needle - 1;
      while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n')
        after++;
      if (*after == ',' || *after == '}') hit = q;
      break;
    }
  }
  if (marker_count != 1 || hit == NULL) {
    free(normalized_args);
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "fs.write draft marker is reserved for the exact "
                        "content value");
  }

  e = draft_file_content(c, t, args, &draft);
  if (e != ASNGN_OK) {
    free(normalized_args);
    return e;
  }
  asngn_buf_init(&b);
  e = asngn_buf_append(&b, args, (size_t)(hit - args));
  if (e == ASNGN_OK) e = append_xcdn_string(&b, draft);
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, hit + sizeof needle - 1);
  free(draft);
  free(normalized_args);
  if (e != ASNGN_OK) {
    asngn_buf_free(&b);
    return e;
  }
  *out = asngn_buf_detach(&b);
  asngn_buf_free(&b);
  return *out != NULL ? ASNGN_OK : ASNGN_ERR_NOMEM;
}
