/* Validate, authorize, journal and execute a selected tool invocation. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

asngn_err asngn_call_execute(asngn_ctx *c, asngn_turn_state *t, const char *line,
                             const char *fallback) {
  asngn_session *s = t->s;
  char *ref = NULL, *cmd = NULL, *args = NULL, *expanded_args = NULL;
  char *canon_args = NULL;
  const char *exec_args;
  asngn_tool_note note;
  uint8_t key[32], cache_key[32], call_key[32], draft_intent[32];
  size_t call_key_pos = (size_t)-1, draft_key_pos = (size_t)-1;
  bool draft_call = false;
  asngn_err e = ASNGN_OK;
  astools_err ae;
  bool cached_hit = false;
  const astools_selected_command *selected = NULL;
  int64_t t0 = asngn_clock_mono_ms(&c->clock);

  if (t->phase != ASNGN_PHASE_ACTION)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "tool dispatch attempted outside action phase");
  if (c->astools == NULL || !c->astools_ok || t->opts.no_tools)
    return asngn_call_error(c, t, "tool", "call", NULL, "asngn/no-tools",
                            "tools are disabled for this turn");

  ae = astools_call_parse(c->astools, line, &ref, &cmd, &args);
  if (ae != ASTOOLS_OK)
    return asngn_call_error(c, t, "tool", "call", NULL, "astools/invalid-args",
                            "malformed call line");

  int selected_index = asngn_tools_find(t, ref, cmd);
  if (selected_index < 0) {
    asngn_tool_note denied_note = {0};
    if (t->security_profile == ASNGN_SECURITY_CODING_READONLY &&
        asngn_siblings_annotations(c, ref, cmd, &denied_note) == ASNGN_OK &&
        (!denied_note.read_only || denied_note.destructive)) {
      t->authorization_blocked = true;
      asngn_turn_stream_emit(
          t, ASNGN_STREAM_NOTICE,
          "The coding-readonly profile excludes this mutation; a writable profile is required.");
    }
    e = asngn_call_error(
        c, t, ref, cmd, args, "asngn/not-selected",
        "command unavailable in this selection; use discover with its name or purpose");
    goto out;
  }
  selected = astools_selection_get(t->tool_selection, (size_t)selected_index);
  ae = astools_selection_validate(t->tool_selection, selected->tool, args);
  if (ae != ASTOOLS_OK) {
    if (ae == ASTOOLS_ERR_DENIED) {
      t->authorization_blocked = true;
      asngn_turn_stream_emit(
          t, ASNGN_STREAM_NOTICE,
          "The tool's identity or permissions changed; rediscover it before continuing.");
    }
    e = asngn_call_error(c, t, ref, cmd, args, "astools/preflight", astools_last_error(c->astools));
    goto out;
  }

  /* A draft marker expands into freshly sampled content, so the ordinary
   * post-expansion call hash would consider every repeat different. Track the
   * stable intent before expansion, but scope it to the workspace state: a
   * failed write is blocked only until some other action changes its inputs. */
  if (strcmp(ref, "fs") == 0 && strcmp(cmd, "write") == 0 && strstr(args, "@asngn:draft") != NULL) {
    asngn_buf ib;
    uint8_t workspace_hash[32];
    size_t i;
    bool repeat = false;
    draft_call = true;
    asngn_buf_init(&ib);
    if (asngn_buf_printf(&ib, "draft|%s|%s|%s", ref, cmd, args) != ASNGN_OK) {
      asngn_buf_free(&ib);
      e = ASNGN_ERR_NOMEM;
      goto out;
    }
    asngn_sha256(ib.data, ib.len, draft_intent);
    asngn_buf_free(&ib);
    asngn_workspace_hash(c, workspace_hash);
    asngn_call_state_key(draft_intent, workspace_hash, s->world_epoch, call_key);
    for (i = 0; i < t->call_keys_n; i++)
      if (memcmp(t->call_keys[i], call_key, 32) == 0) repeat = true;
    if (repeat) {
      t->repeat_calls++;
      t->futile_row++;
      t->call_mute = true;
      asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn,
                      "{guard: \"identical_call\"}");
      os_rwlock_wrlock(&c->lock);
      c->stats.guard_trips++;
      os_rwlock_wrunlock(&c->lock);
      e = asngn_call_error(c, t, ref, cmd, args, "asngn/repeat",
                           "the same artifact write was already attempted in "
                           "the current workspace state");
      goto out;
    }
    if (t->call_keys_n == t->call_keys_cap) {
      size_t cap = t->call_keys_cap != 0 ? t->call_keys_cap * 2 : 8;
      uint8_t (*nk)[32] = realloc(t->call_keys, cap * 32);
      if (nk != NULL) {
        t->call_keys = nk;
        t->call_keys_cap = cap;
      }
    }
    if (t->call_keys_n < t->call_keys_cap) {
      draft_key_pos = t->call_keys_n;
      memcpy(t->call_keys[t->call_keys_n++], call_key, 32);
    }
  }

  e = asngn_expand_write_draft(c, t, ref, cmd, args, &expanded_args);
  if (e != ASNGN_OK) goto out;
  exec_args = expanded_args != NULL ? expanded_args : args;

  canon_args = asngn_call_args(exec_args);
  if (canon_args == NULL) {
    e = ASNGN_ERR_NOMEM;
    goto out;
  }
  {
    uint8_t args_hash[32];
    char args_hex[65];
    /* Log callbacks receive DEBUG records even when the file sink does not.
     * Tool arguments routinely contain credentials, so neither sink may see
     * the payload itself. */
    asngn_sha256(canon_args, strlen(canon_args), args_hash);
    asngn_sha256_hex(args_hash, 32, args_hex);
    asngn_log(c, ASNGN_LOG_DEBUG, "loop", "call %s.%s args_sha256=%s", ref, cmd, args_hex);
  }

  /* plan gate: args validated before any confirmation UI */
  ae = expanded_args ? astools_selection_validate(t->tool_selection, selected->tool, exec_args)
                     : ASTOOLS_OK;
  if (ae != ASTOOLS_OK) {
    e = asngn_call_error(c, t, ref, cmd, args, "astools/invalid-args",
                         astools_last_error(c->astools));
    goto out;
  }
  memset(&note, 0, sizeof note);
  note.read_only = selected->read_only;
  note.destructive = selected->destructive;
  note.idempotent = selected->idempotent;
  note.long_running = selected->long_running;
  snprintf(note.version, sizeof note.version, "%s", strchr(selected->ref, '@') + 1);

  if (t->security_profile == ASNGN_SECURITY_CODING_READONLY &&
      (!note.read_only || note.destructive)) {
    char notice[192];
    t->authorization_blocked = true;
    snprintf(notice, sizeof notice,
             "Authorization required: %s.%s is blocked by the "
             "coding-readonly profile. Change the profile to continue.",
             ref, cmd);
    asngn_turn_stream_emit(t, ASNGN_STREAM_NOTICE, notice);
    asngn_tele_emit(c, "authorization", t->span_root, NULL, s->slug, t->led.turn,
                    "{granted: false, profile: \"coding-readonly\"}");
    e = asngn_call_error(c, t, ref, cmd, args, "asngn/profile-readonly", notice);
    if (e == ASNGN_OK) asngn_call_fallback(c, t, fallback);
    goto out;
  }

  /* Persistent cache reuse and the per-turn repeat guard both observe the
   * live workspace.  The repeat key additionally carries world_epoch and is
   * advanced after successful mutations (see asngn_call_state_key). */
  {
    asngn_buf kb;
    uint8_t workspace_hash[32];
    char workspace_hex[65];
    asngn_buf_init(&kb);
    if (asngn_buf_printf(&kb, "%s|%s|%s|%s", selected->ref, selected->content_sha256, cmd,
                         canon_args) != ASNGN_OK) {
      asngn_buf_free(&kb);
      e = ASNGN_ERR_NOMEM;
      goto out;
    }
    asngn_sha256(kb.data, kb.len, key);
    asngn_buf_free(&kb);
    asngn_workspace_hash(c, workspace_hash);
    asngn_sha256_hex(workspace_hash, sizeof workspace_hash, workspace_hex);
    asngn_call_state_key(key, workspace_hash, s->world_epoch, call_key);
    asngn_buf_init(&kb);
    if (asngn_buf_appends(&kb, workspace_hex) != ASNGN_OK) {
      asngn_buf_free(&kb);
      e = ASNGN_ERR_NOMEM;
      goto out;
    }
    {
      asngn_sha256_ctx hc;
      asngn_sha256_init(&hc);
      asngn_sha256_update(&hc, key, sizeof key);
      asngn_sha256_update(&hc, kb.data, kb.len);
      asngn_sha256_final(&hc, cache_key);
    }
    asngn_buf_free(&kb);
  }
  {
    size_t i;
    bool repeat = false;
    for (i = 0; i < t->call_keys_n; i++)
      if (memcmp(t->call_keys[i], call_key, 32) == 0) repeat = true;
    /* oscillation guard: A-B-A-B alternation of blocked/failing calls */
    if (repeat) {
      if (memcmp(call_key, t->osc_b, 32) == 0 && memcmp(t->osc_a, t->osc_b, 32) != 0)
        t->osc_cycles++;
      memcpy(t->osc_b, t->osc_a, 32);
      memcpy(t->osc_a, call_key, 32);
      t->repeat_calls++;
      t->futile_row++;
      /* a model that just repeated a call tends to repeat it again:
       * withhold the CALL alternative for one pass so the next decision
       * must answer or think instead */
      t->call_mute = true;
      asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn,
                      "{guard: \"identical_call\"}");
      os_rwlock_wrlock(&c->lock);
      c->stats.guard_trips++;
      os_rwlock_wrunlock(&c->lock);
      e = asngn_call_error(c, t, ref, cmd, args, "asngn/repeat", "already ran; result above");
      goto out;
    }
    memcpy(t->osc_b, t->osc_a, 32);
    memcpy(t->osc_a, call_key, 32);
    t->repeat_calls = 0; /* a fresh call shows the model adapted */
  }
  /* remember the key now, so identical retries — including of denied,
   * capped, or cached calls — are blocked and cannot spam the
   * confirmation prompt */
  if (t->call_keys_n == t->call_keys_cap) {
    size_t cap = t->call_keys_cap != 0 ? t->call_keys_cap * 2 : 8;
    uint8_t (*nk)[32] = realloc(t->call_keys, cap * 32);
    if (nk != NULL) {
      t->call_keys = nk;
      t->call_keys_cap = cap;
    }
  }
  if (t->call_keys_n < t->call_keys_cap) {
    call_key_pos = t->call_keys_n;
    memcpy(t->call_keys[t->call_keys_n++], call_key, 32);
  }

  /* tool-call cap */
  if (t->tool_calls >= c->cfg.max_tool_calls) {
    t->futile_row++;
    asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug, t->led.turn, "{guard: \"tool_cap\"}");
    os_rwlock_wrlock(&c->lock);
    c->stats.guard_trips++;
    os_rwlock_wrunlock(&c->lock);
    e = asngn_call_error(c, t, ref, cmd, args, "asngn/tool-cap",
                         "tool budget for this turn is spent; answer with "
                         "what you have");
    goto out;
  }
  /* past the walls: whatever happens next (cache replay, deny, error,
   * dispatch) gives the model fresh information — not a futile step */
  t->futile_row = 0;

  /* tool-result cache: read_only AND idempotent commands only */
  if (c->cfg.tool_cache && note.read_only && note.idempotent) {
    char *hit = NULL;
    if (asngn_toolcache_get(c, cache_key, &hit) && hit != NULL) {
      struct astools_result_s r;
      char *out_line = NULL;
      memset(&r, 0, sizeof r);
      r.ok = 1;
      r.result_xcdn = hit;
      if (astools_call_format(c->astools, ref, cmd, &r, &out_line) == ASTOOLS_OK &&
          out_line != NULL) {
        e = asngn_call_outcome(c, t, ref, cmd, args, out_line);
        astools_free(out_line);
      }
      free(hit);
      cached_hit = true;
      /* a replayed result still makes this a tool-touched turn:
       * its answer must never become verbatim-reusable */
      t->tools_used = true;
      t->tool_ok_seen = true; /* the cache only stores ok results */
      {
        char lbl[132];
        char **nl;
        snprintf(lbl, sizeof lbl, "%s.%s", ref, cmd);
        nl = realloc(t->tools_list, (t->tools_list_n + 1) * sizeof *nl);
        if (nl != NULL) {
          t->tools_list = nl;
          t->tools_list[t->tools_list_n] = asngn_strdup(lbl);
          if (t->tools_list[t->tools_list_n] != NULL) t->tools_list_n++;
        }
      }
      os_rwlock_wrlock(&c->lock);
      c->stats.tool_cache_hits++;
      os_rwlock_wrunlock(&c->lock);
      asngn_tele_emit(c, "tool_call", t->span_root, NULL, s->slug, t->led.turn, "{cached: true}");
      goto out;
    }
  }

  /* action gate: annotation-driven confirmation */
  {
    const char *deny_code = NULL;
    if (!asngn_call_confirm(c, t, ref, cmd, args, &note, &deny_code)) {
      char notice[192];
      char auth_data[128];
      t->authorization_blocked = true;
      snprintf(notice, sizeof notice,
               "Authorization required: %s.%s was not approved. Change the "
               "security profile or confirmation policy to continue.",
               ref, cmd);
      asngn_turn_stream_emit(t, ASNGN_STREAM_NOTICE, notice);
      snprintf(auth_data, sizeof auth_data, "{granted: false, profile: \"%s\"}",
               asngn_security_profile_name(t->security_profile));
      asngn_tele_emit(c, "authorization", t->span_root, NULL, s->slug, t->led.turn, auth_data);
      e = asngn_call_error(c, t, ref, cmd, args, deny_code, notice);
      if (e == ASNGN_OK) asngn_call_fallback(c, t, fallback);
      goto out;
    }
  }

  /* dispatch through astools */
  {
    astools_result r;
    char *out_line = NULL;
    uint32_t deadline_ms = 0;
    if (c->asper_ok) {
      asngn_buf call_event;
      asngn_buf_init(&call_event);
      e = asngn_buf_printf(&call_event, "CALL %s.%s %s", ref, cmd,
                           exec_args != NULL ? exec_args : "{}");
      if (e == ASNGN_OK)
        e = asngn_siblings_event_append(c, s->slug, ASNGN_MEM_TOOL_CALL, call_event.data, NULL,
                                        false, NULL);
      asngn_buf_free(&call_event);
      if (e != ASNGN_OK) goto out;
    }
    if (t->deadline_mono > 0) {
      int64_t remaining = t->deadline_mono - asngn_clock_mono_ms(&c->clock);
      if (remaining <= 0) {
        e = ASNGN_ERR_TIMEOUT;
        goto out;
      }
      deadline_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    }
    memset(&r, 0, sizeof r);
    {
      char data[160];
      snprintf(data, sizeof data, "{what: \"tool\", tool: \"%.32s\", command: \"%.32s\"}", ref,
               cmd);
      asngn_tele_emit(c, "phase", t->span_root, NULL, s->slug, t->led.turn, data);
    }
    {
      asngn_buf intent;
      asngn_buf_init(&intent);
      e = asngn_buf_printf(&intent, "%s.%s %s", ref, cmd, exec_args ? exec_args : "{}");
      t->action_mutates = !note.read_only;
      asngn_uuid_v4(t->action_id);
      if (e == ASNGN_OK) e = asngn_turn_journal(t, "action", intent.data);
      asngn_buf_free(&intent);
      if (e != ASNGN_OK) goto out;
    }
    /* A failed mutation may still have changed files. Revoke before dispatch. */
    if (!note.read_only && !asngn_verification_command(ref, cmd, expanded_args)) {
      t->verification_attempted = false;
      t->verification_ok = false;
      os_rwlock_wrlock(&s->lock);
      e = asngn_work_revoke(s);
      os_rwlock_wrunlock(&s->lock);
      if (e != ASNGN_OK) goto out;
    }
    char proof_base[65] = "";
    if (asngn_verification_command(ref, cmd, expanded_args) &&
        asngn_workspace_refresh(c) == ASNGN_OK)
      memcpy(proof_base, c->workspace.fingerprint, sizeof proof_base);
    ae = (astools_err)asngn_tools_invoke(t, selected->tool, exec_args, deadline_ms, &r);
    t->tool_calls++;
    t->tools_used = true;
    if (ae == ASTOOLS_OK && r.ok) t->tool_ok_seen = true;
    char proof_after[65] = "";
    if (asngn_verification_command(ref, cmd, expanded_args)) {
      if (asngn_workspace_refresh(c) == ASNGN_OK)
        memcpy(proof_after, c->workspace.fingerprint, sizeof proof_after);
      t->verification_attempted = true;
      t->verification_ok = ae == ASTOOLS_OK && r.ok &&
                           asngn_verification_result_ok(r.result_xcdn) && proof_base[0] &&
                           !strcmp(proof_base, proof_after);
      memcpy(t->verification_snapshot, proof_base, sizeof proof_base);
      memcpy(t->verification_action_id, t->action_id, sizeof t->action_id);
    }
    e = asngn_turn_journal(t, "observed",
                           r.result_xcdn ? r.result_xcdn
                                         : (r.error_code ? r.error_code : "unknown outcome"));
    if (e == ASNGN_OK)
      e = asngn_work_observe(t, ref, cmd, exec_args, ae == ASTOOLS_OK && r.ok, r.result_xcdn,
                             proof_base, proof_after);
    if (e != ASNGN_OK) {
      astools_result_free(&r);
      goto out;
    }
    {
      char lbl[132];
      char **nl;
      snprintf(lbl, sizeof lbl, "%s.%s", ref, cmd);
      nl = realloc(t->tools_list, (t->tools_list_n + 1) * sizeof *nl);
      if (nl != NULL) {
        t->tools_list = nl;
        t->tools_list[t->tools_list_n] = asngn_strdup(lbl);
        if (t->tools_list[t->tools_list_n] != NULL) t->tools_list_n++;
      }
    }
    os_rwlock_wrlock(&c->lock);
    c->stats.tool_calls++;
    os_rwlock_wrunlock(&c->lock);

    if (ae == ASTOOLS_ERR_DENIED ||
        (r.error_code != NULL && strstr(r.error_code, "denied") != NULL)) {
      t->authorization_blocked = true;
      asngn_turn_stream_emit(t, ASNGN_STREAM_NOTICE,
                             "Authorization is missing for a requested tool action. Update the "
                             "tool permissions or security profile to continue.");
      asngn_tele_emit(c, "authorization", t->span_root, NULL, s->slug, t->led.turn,
                      "{granted: false, profile: \"tool\"}");
    }

    if (ae != ASTOOLS_OK && r.error_code == NULL) {
      /* engine-level failure without a code: synthesize one */
      const char *code = ae == ASTOOLS_ERR_DENIED      ? "astools/denied"
                         : ae == ASTOOLS_ERR_TIMEOUT   ? "astools/timeout"
                         : ae == ASTOOLS_ERR_CANCELLED ? "astools/cancelled"
                                                       : "astools/failed";
      e = asngn_call_error(c, t, ref, cmd, args, code, astools_last_error(c->astools));
      if (e == ASNGN_OK) asngn_call_fallback(c, t, fallback);
      astools_result_free(&r);
      goto telem;
    }

    /* world epoch: successful non-read_only invocations */
    if (ae == ASTOOLS_OK && r.ok && !note.read_only) {
      uint8_t workspace_hash[32];
      t->wrote_workspace = true;
      if (asngn_artifact_command(ref, cmd) &&
          !asngn_verification_command(ref, cmd, expanded_args)) {
        t->artifact_written = true;
        t->verification_attempted = false;
        t->verification_ok = false;
      }
      s->world_epoch++;
      asngn_toolcache_clear(c);
      /* The action WAL reserved this epoch before dispatch. Recovery
       * invalidates stale cache entries even if the tool outcome is unknown. */
      /* Store the command against the state it produced.  Its own outputs do
       * not make an immediate duplicate look fresh, while a later edit does. */
      asngn_workspace_hash(c, workspace_hash);
      if (call_key_pos != (size_t)-1)
        asngn_call_state_key(key, workspace_hash, s->world_epoch, t->call_keys[call_key_pos]);
      if (draft_call && draft_key_pos != (size_t)-1)
        asngn_call_state_key(draft_intent, workspace_hash, s->world_epoch,
                             t->call_keys[draft_key_pos]);
    }
    /* tool cache insert */
    if (ae == ASTOOLS_OK && r.ok && c->cfg.tool_cache && note.read_only && note.idempotent &&
        r.result_xcdn != NULL) {
      char *masked = NULL;
      size_t nm = 0;
      if (asngn_redact(r.result_xcdn, strlen(r.result_xcdn), &masked, &nm) == ASNGN_OK &&
          masked != NULL) {
        asngn_toolcache_put(c, cache_key, masked);
        free(masked);
      } else {
        asngn_toolcache_put(c, cache_key, r.result_xcdn);
      }
    }

    if (astools_call_format(c->astools, ref, cmd, &r, &out_line) == ASTOOLS_OK &&
        out_line != NULL) {
      asngn_buf ob;
      asngn_buf_init(&ob);
      /* echo the originating call so the outcome stays attributable */
      if (asngn_buf_printf(&ob, "CALL %s.%s %s -> %s", ref, cmd, args != NULL ? args : "{}",
                           out_line) == ASNGN_OK) {
        char *masked = asngn_context_text(s, ob.data);
        if (masked != NULL) {
          char lbl[132];
          char *digested = NULL;
          snprintf(lbl, sizeof lbl, "%s.%s", ref, cmd);
          if (asngn_digest_item(c, s, t, lbl, masked, strlen(masked), &t->led.sv_digest,
                                &t->led.gt_aux, &digested) == ASNGN_OK &&
              digested != NULL) {
            e = asngn_work_data(c, t, digested);
            free(digested);
          } else {
            e = asngn_work_data(c, t, masked);
          }
          free(masked);
        }
      }
      asngn_buf_free(&ob);
      astools_free(out_line);
    }
    /* the model's own contingency plan steers the recovery pass */
    if (!(ae == ASTOOLS_OK && r.ok)) asngn_call_fallback(c, t, fallback);
  telem: {
    char data[192];
    snprintf(data, sizeof data, "{tool: \"%s\", command: \"%s\", ok: %s, ms: %lld}", ref, cmd,
             (ae == ASTOOLS_OK && r.ok) ? "true" : "false",
             (long long)(asngn_clock_mono_ms(&c->clock) - t0));
    asngn_tele_emit(c, "tool_call", t->span_root, NULL, s->slug, t->led.turn, data);
  }
    astools_result_free(&r);
  }

out:
  (void)cached_hit;

  free(canon_args);
  free(expanded_args);
  astools_free(ref);
  astools_free(cmd);
  astools_free(args);
  return e;
}
