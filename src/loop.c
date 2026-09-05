/*
 * loop.c — the control loop: one turn from INGEST to COMMIT.
 *
 *   INGEST -> MEMORY -> CACHE -> ROUTE -> [STEP LOOP] -> ANSWER -> COMMIT
 *
 * Admission writes a started intent; ACTION journals tool dispatch before
 * execution. COMMIT publishes conversation state through a single WAL frame.
 * Cancellation leaves auditable intents/observations, not a transcript turn.
 * Every stage emits telemetry
 * spans; every model call runs under the stall watchdog with the
 * cancellation flag cascading into the backend.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "execution.h"
#include "asngn_internal.h"

#include "astools.h"

/* ── small helpers ────────────────────────────────────────────────────── */

static const char *class_name(asngn_class k) {
  switch (k) {
  case ASNGN_CLASS_SIMPLE:   return "simple";
  case ASNGN_CLASS_MODERATE: return "moderate";
  case ASNGN_CLASS_COMPLEX:  return "complex";
  }
  return "?";
}

static const char *mode_name(asngn_mode m) {
  return m == ASNGN_MODE_DIRECT ? "direct" : "plan";
}

/* ── the turn ────────────────────────────────────────────────────────── */

asngn_err asngn_loop_run(asngn_ctx *c, asngn_turn_state *t) {
  asngn_session *s = t->s;
  int64_t start_ms = asngn_clock_mono_ms(&c->clock);
  asngn_err e = ASNGN_OK;
  size_t user_n = 0;
  asngn_cache_probe_result probe;
  bool cache_answered = false;

  memset(&probe, 0, sizeof probe);
  os_rwlock_wrlock(&s->lock);
  e=asngn_workspace_info_refresh(c,&s->workspace);
  if (e==ASNGN_OK) c->workspace=s->workspace;
  os_rwlock_wrunlock(&s->lock);
  if (e==ASNGN_OK) e=asngn_siblings_workspace_sync(c,c->workspace.canonical_root);
  if (e != ASNGN_OK) return e;
  t->phase = ASNGN_PHASE_ACTION;
  if (!t->span_root[0]) asngn_uuid_v4(t->span_root);
  if (t->cancel) return ASNGN_ERR_CANCELLED;
  if (t->deadline_mono > 0 && start_ms >= t->deadline_mono) return ASNGN_ERR_TIMEOUT;
  t->log_before=s->log_n; t->turns_before=s->turns;
  memcpy(t->led.turn_id,t->span_root,37);
  t->tx_started=true;
  snprintf(t->led.cache, sizeof t->led.cache, "off");
  t->gen_slot = asngn_models_slot_for_role(c, ASNGN_ROLE_GENERATOR);
  if (t->retry_up) {
    int up = asngn_route_tier_up(c, t->gen_slot);
    if (up >= 0) {
      t->gen_slot = up;
      t->escalations++;
    }
  }

  {
    char data[160];
    snprintf(data, sizeof data,
             "{bytes: %zu, mode: \"%s\", security_profile: \"%s\"}",
             strlen(t->user_msg), asngn_usage_mode_name(t->usage_mode),
             asngn_security_profile_name(t->security_profile));
    asngn_tele_emit(c, "turn_start", t->span_root, NULL, s->slug,
                    s->turns + 1, data);
  }

  /* ── INGEST ─────────────────────────────────────────────────────── */
  os_rwlock_wrlock(&s->lock);
  asngn_session_clear_blobs(s);
  if (!t->continuation) {
    asngn_turn ut;
    memset(&ut, 0, sizeof ut);
    user_n = s->turns + 1;
    ut.n = user_n;
    snprintf(ut.role, sizeof ut.role, "user");
    ut.text = t->user_msg;
    ut.at = asngn_clock_now(&c->clock);
    snprintf(ut.workspace,sizeof ut.workspace,"%s",s->workspace.canonical_root);
    snprintf(ut.commit,sizeof ut.commit,"%s",s->workspace.head);
    snprintf(ut.project,sizeof ut.project,"%s",s->project ? s->project : "");
    memcpy(ut.turn_id,t->span_root,37);
    e = asngn_session_stage_turn(s, &ut);
    if (e == ASNGN_OK) s->turns = user_n;
  } else {
    user_n = s->turns;
  }
  t->led.turn = s->turns + 1; /* the assistant turn we will commit */
  os_rwlock_wrunlock(&s->lock);
  if (e != ASNGN_OK) return e;

  /* continuation turns see the partial answer as working context */
  if (t->continuation && s->last_answer != NULL) {
    asngn_buf b;
    asngn_buf_init(&b);
    if (asngn_buf_printf(&b, "assistant (partial): %s",
                         s->last_answer) == ASNGN_OK)
      asngn_work_push(c, t, b.data);
    asngn_buf_free(&b);
  }

  /* ── MEMORY ─────────────────────────────────────────────────────── */
  /* several sessions can interleave on one engine (MCP): make sure the
   * memory zone is built against THIS session's project */
  {
    char *proj = NULL;
    bool have;
    os_rwlock_rdlock(&s->lock);
    have = s->project != NULL;
    proj = have ? asngn_strdup(s->project) : NULL;
    os_rwlock_rdunlock(&s->lock);
    if (!have || proj != NULL) /* OOM: keep the current selection */
      (void)asngn_siblings_project_sync(c, proj);
    free(proj);
  }
  if (c->astools_ok && !t->opts.no_tools) {
    e = asngn_tools_select(c,t,t->user_msg);
    if (e != ASNGN_OK) return e;
  }

  e=asngn_retrieval_query(s,t,&t->retrieval_query);
  if (e!=ASNGN_OK) return e;
  if (t->usage_mode==ASNGN_USAGE_CODING && !t->opts.no_tools) {
    e=asngn_code_retrieve(c,t);
    if (e!=ASNGN_OK) return e;
  }

  /* ── CACHE ──────────────────────────────────────────────────────── */
  {
    asngn_route_profile pre;
    asngn_route_evidence pre_ev;
    bool bypass_cache;
    asngn_route_evidence_collect(c, s, t, &pre_ev);
    asngn_route_heuristic(t->user_msg, &pre_ev, &pre);
    /* The semantic cache runs before the semantic router.  In a coding
     * profile that ordering could let an English-biased heuristic miss a
     * multilingual mutation request and replay prose instead of touching the
     * workspace.  Quality-first coding therefore bypasses answer reuse
     * entirely; general profiles still bypass every heuristic tool/coding
     * turn. */
    bypass_cache = t->usage_mode == ASNGN_USAGE_CHAT ||
                   c->cfg.profile == ASNGN_PROFILE_CODING ||
                   pre.mode == ASNGN_MODE_PLAN || asngn_coding_task(pre.task);
  if (!bypass_cache && !t->opts.no_cache && !t->continuation &&
      c->cfg.cache_enable) {
    double bias = asngn_pressure(c, s) >= 1.0 ? 0.02 : 0.0;
    if (asngn_cache_probe(c, s, t->user_msg, bias, &probe) == ASNGN_OK) {
      if (probe.outcome == ASNGN_CACHE_HIT && probe.answer != NULL &&
          !asngn_response_has_tool_protocol(c, probe.answer)) {
        t->answer = asngn_strdup(probe.answer);
        if (t->answer == NULL) {
          asngn_cache_probe_free(&probe);
          return ASNGN_ERR_NOMEM;
        }
        snprintf(t->led.cache, sizeof t->led.cache, "hit");
        snprintf(t->led.klass, sizeof t->led.klass, "simple");
        snprintf(t->led.detail, sizeof t->led.detail, "%s",
                 probe.detail);
        snprintf(t->led.mode, sizeof t->led.mode, "direct");
        snprintf(t->led.tier, sizeof t->led.tier, "%s",
                 probe.tier);
        t->led.sv_cache = probe.gen_tokens;
        t->phase = ASNGN_PHASE_RESPONSE;
        asngn_turn_stream_emit(t, ASNGN_STREAM_OUTPUT, t->answer);
        cache_answered = true;
        os_rwlock_wrlock(&c->lock);
        c->stats.cache_hits++;
        os_rwlock_wrunlock(&c->lock);
      } else if (probe.outcome == ASNGN_CACHE_ADAPT &&
                 probe.answer != NULL) {
        /* adapt pass on the adapter role */
        int slot = asngn_models_slot_for_role(c, ASNGN_ROLE_ADAPTER);
        asngn_buf up;
        char *adapted = NULL;
        int tin = 0, tout = 0;
        t->detail = asngn_detail_effective(
            c, t->opts.detail != ASNGN_DETAIL_AUTO
                   ? t->opts.detail
                   : asngn_detail_cue(t->user_msg),
            ASNGN_DETAIL_NORMAL, ASNGN_CLASS_SIMPLE,
            asngn_pressure(c, s));
        asngn_buf_init(&up);
        if (slot >= 0 &&
            asngn_buf_printf(
                &up,
                "Previous question:\n%s\n\nPrevious answer:\n%s\n\n"
                "New question:\n%s",
                probe.query, probe.answer,
                t->user_msg) == ASNGN_OK) {
          asngn_err ge = asngn_generate_watched(
              c, t, slot, ASNGN_TASK_ADAPT,
              "You adapt a previous answer to a new, similar question. "
              "Keep it correct; change only what the new question "
              "requires.",
              up.data, NULL, NULL, asngn_detail_cap(c, t->detail), NULL, NULL,
              &adapted, &tin, &tout);
          /* the adapter's spend is aux overhead whatever the outcome
           * (the cost of safety is measured, not hidden) */
          if (ge == ASNGN_OK && tout > 0) t->led.gt_aux += (size_t)tout;
          /* a judge rejection sends the turn down the full miss
           * path — one fallback, no loops */
          if (ge == ASNGN_OK && adapted != NULL && adapted[0] != '\0' &&
              c->cfg.judge == ASNGN_JUDGE_FULL) {
            int score = 0;
            size_t jaux = 0;
            if (asngn_judge_run(c, s, t, t->user_msg, NULL, adapted,
                                &score, NULL, &jaux) == ASNGN_OK) {
              t->led.gt_aux += jaux;
              if (score < c->cfg.judge_threshold) {
                asngn_log(c, ASNGN_LOG_WARN, "cache",
                          "adapt rejected by judge; miss path");
                free(adapted);
                adapted = NULL;
              }
            }
          }
          if (ge == ASNGN_OK && adapted != NULL && adapted[0] != '\0' &&
              !asngn_response_has_tool_protocol(c, adapted)) {
            t->answer = adapted;
            adapted = NULL;
            snprintf(t->led.cache, sizeof t->led.cache, "adapt");
            snprintf(t->led.klass, sizeof t->led.klass, "simple");
            snprintf(t->led.detail, sizeof t->led.detail, "%s",
                     asngn_detail_name(t->detail));
            snprintf(t->led.mode, sizeof t->led.mode, "direct");
            {
              int aslot = slot;
              snprintf(t->led.tier, sizeof t->led.tier, "%.15s",
                       c->models[aslot].cfg.id);
            }
            t->led.sv_cache = probe.gen_tokens;
            t->phase = ASNGN_PHASE_RESPONSE;
            asngn_turn_stream_emit(t, ASNGN_STREAM_OUTPUT, t->answer);
            cache_answered = true;
            os_rwlock_wrlock(&c->lock);
            c->stats.cache_adapts++;
            os_rwlock_wrunlock(&c->lock);
          }
          free(adapted);
        }
        asngn_buf_free(&up);
      }
      if (!cache_answered && probe.outcome != ASNGN_CACHE_MISS) {
        /* adapt fell through: full miss path (one fallback) */
      }
      if (!cache_answered) {
        os_rwlock_wrlock(&c->lock);
        c->stats.cache_misses++;
        os_rwlock_wrunlock(&c->lock);
        /* plan hint from tool-touched neighbors */
        if (probe.query_vec != NULL) {
          char *hint = NULL;
          if (asngn_cache_plan_hint(c, s, probe.query_vec, &hint) ==
                  ASNGN_OK && hint != NULL) {
            asngn_work_push(c, t, hint);
            free(hint);
          }
        }
        snprintf(t->led.cache, sizeof t->led.cache, "miss");
      }
    }
  }
  }

  /* ── ROUTE / STEP LOOP / ANSWER ─────────────────────────────────── */
  if (!cache_answered && !t->cancel) {
    size_t aux = 0;
    double pressure = asngn_pressure(c, s);
    e = asngn_route_classify(c, s, t->user_msg, t, &t->prof, &aux);
    if (e != ASNGN_OK) return e;
    t->led.gt_aux += aux;
    aux = 0;
    {
      asngn_detail user_ov = t->opts.detail != ASNGN_DETAIL_AUTO
                                 ? t->opts.detail
                                 : asngn_detail_cue(t->user_msg);
      t->detail = asngn_detail_effective(c, user_ov, t->prof.detail,
                                         t->prof.klass, pressure);
    }
    if (t->continuation) t->prof.mode = ASNGN_MODE_DIRECT;
    if (t->usage_mode == ASNGN_USAGE_CHAT) {
      t->prof.mode = ASNGN_MODE_DIRECT;
      t->prof.task = ASNGN_RTASK_CHAT;
    }
    /* evidence-gated starting tier (G1: capacity only on evidence).
     * A COMPLEX verdict starts one tier up instead of paying a demonstrated
     * failure first. A SIMPLE DIRECT turn with a clean recent window
     * starts one tier down: the classifier now has the evidence to say
     * so, and the judge/escalation ladder still recovers a miss. */
    if (t->prof.klass == ASNGN_CLASS_COMPLEX &&
        !t->retry_up && t->escalations < c->cfg.max_escalations) {
      int up = asngn_route_tier_up(c, t->gen_slot);
      if (up >= 0) {
        t->gen_slot = up;
        t->escalations++;
        asngn_tele_emit(c, "route", t->span_root, NULL, s->slug,
                        t->led.turn, "{start: \"up\", proactive: true}");
        os_rwlock_wrlock(&c->lock);
        c->stats.escalations++;
        os_rwlock_wrunlock(&c->lock);
      }
    } else if (c->cfg.profile != ASNGN_PROFILE_CODING &&
               (t->prof.task == ASNGN_RTASK_CHAT ||
                t->prof.task == ASNGN_RTASK_LOOKUP) &&
               t->prof.klass == ASNGN_CLASS_SIMPLE &&
               t->prof.mode == ASNGN_MODE_DIRECT && !t->retry_up &&
               t->evidence.escalated == 0 && t->evidence.unreliable == 0) {
      /* floor: the router tier classifies, it never answers */
      int down = asngn_route_tier_down(c, t->gen_slot);
      if (down >= 0 &&
          down != asngn_models_slot_for_role(c, ASNGN_ROLE_ROUTER)) {
        t->gen_slot = down;
        asngn_tele_emit(c, "route", t->span_root, NULL, s->slug,
                        t->led.turn, "{start: \"down\"}");
      }
    }
    /* Spend ceilings are telemetry/adaptation signals, not an instruction
     * to swap a proven-capable coding model for a weaker one. */
    if (pressure >= 1.0) {
      asngn_tele_emit(c, "guard", t->span_root, NULL, s->slug,
                      t->led.turn, "{guard: \"budget_pressure\"}");
    }
    snprintf(t->led.klass, sizeof t->led.klass, "%s",
             class_name(t->prof.klass));
    snprintf(t->led.detail, sizeof t->led.detail, "%s",
             asngn_detail_name(t->detail));
    snprintf(t->led.mode, sizeof t->led.mode, "%s",
             mode_name(t->prof.mode));
    {
      char data[160];
      snprintf(data, sizeof data,
               "{class: \"%s\", detail: \"%s\", mode: \"%s\", task: "
               "\"%s\", tier: \"%s\"}",
               t->led.klass, t->led.detail, t->led.mode,
               asngn_route_task_name(t->prof.task),
               t->gen_slot >= 0 ? c->models[t->gen_slot].cfg.id : "?");
      asngn_tele_emit(c, "route", t->span_root, NULL, s->slug,
                      t->led.turn, data);
    }

    if (t->prof.mode == ASNGN_MODE_PLAN) {
      t->phase = ASNGN_PHASE_ACTION;
      e = asngn_actions_run(c, t);
      if (e != ASNGN_OK) return e;
    }
    if (!t->clarify && !t->cancel) {
      if (asngn_generation_needs_artifact(c, t))
        return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                            "response phase blocked: generate task has no "
                            "successful artifact write");
      t->phase = ASNGN_PHASE_RESPONSE;
      e = asngn_answer_run(c, t, &aux);
      if (e != ASNGN_OK) return e;
      t->led.gt_aux += aux;
    }
    if (t->clarify) {
      t->phase = ASNGN_PHASE_RESPONSE;
      snprintf(t->led.klass, sizeof t->led.klass, "clarify");
    }
    snprintf(t->led.tier, sizeof t->led.tier, "%.15s",
             t->gen_slot >= 0 ? c->models[t->gen_slot].cfg.id : "none");
    t->led.escalations = t->escalations;
  }

  if (t->cancel) {
    asngn_cache_probe_free(&probe);
    return ASNGN_ERR_CANCELLED;
  }

  if (t->answer != NULL && asngn_response_has_tool_protocol(c, t->answer))
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "assistant output contains tool-call syntax");

  /* ── COMMIT ─────────────────────────────────────────────────────── */
  t->led.at = asngn_clock_now(&c->clock);
  t->led.duration_ms =
      (uint64_t)(asngn_clock_mono_ms(&c->clock) - start_ms);
  t->led.capped = t->capped;

  os_rwlock_wrlock(&s->lock);
  e=asngn_workspace_info_refresh(c,&s->workspace);
  if (e!=ASNGN_OK) { os_rwlock_wrunlock(&s->lock);asngn_cache_probe_free(&probe);return e; }
  {
    asngn_turn at;
    memset(&at, 0, sizeof at);
    at.n = s->turns + 1;
    snprintf(at.role, sizeof at.role, "assistant");
    at.text = t->answer != NULL ? t->answer : (char *)"";
    at.at = t->led.at;
    snprintf(at.klass, sizeof at.klass, "%s", t->led.klass);
    snprintf(at.detail, sizeof at.detail, "%s", t->led.detail);
    snprintf(at.mode, sizeof at.mode, "%s", t->led.mode);
    snprintf(at.tier, sizeof at.tier, "%s", t->led.tier);
    snprintf(at.cache, sizeof at.cache, "%s", t->led.cache);
    at.steps = t->steps;
    snprintf(at.workspace,sizeof at.workspace,"%s",s->workspace.canonical_root);
    snprintf(at.commit,sizeof at.commit,"%s",s->workspace.head);
    snprintf(at.project,sizeof at.project,"%s",s->project ? s->project : "");
    memcpy(at.turn_id,t->span_root,37);
    e = asngn_turn_commit(t, &at);
    if (e == ASNGN_OK) {
      s->turns = at.n;
      t->led.turn = at.n;
    }
  }

  /* /more bookkeeping */
  if (e == ASNGN_OK) {
    free(s->last_user_msg);
    free(s->last_answer);
    s->last_user_msg = asngn_strdup(t->user_msg);
    s->last_answer = asngn_strdup(t->answer != NULL ? t->answer : "");
    s->last_capped = t->capped;
    s->last_answer_turn = t->led.turn;
  }
  os_rwlock_wrunlock(&s->lock);
  if (e != ASNGN_OK) {
    asngn_cache_probe_free(&probe);
    return e;
  }
  /* cache insertion: generated (miss-path) answers only */
  if (strcmp(t->led.cache, "miss") == 0 && !t->clarify &&
      !t->forced_answer && t->answer != NULL && t->answer[0] != '\0' &&
      probe.query_vec != NULL) {
    char *masked = NULL, *mquery = NULL;
    size_t nm = 0;
    const char *store = t->answer;
    const char *query = t->user_msg;
    /* caches are always redacted: both the answer and the query */
    if (asngn_redact(t->answer, strlen(t->answer), &masked, &nm) ==
            ASNGN_OK && masked != NULL)
      store = masked;
    if (asngn_redact(t->user_msg, strlen(t->user_msg), &mquery, &nm) ==
            ASNGN_OK && mquery != NULL)
      query = mquery;
    asngn_cache_insert(c, s, query, probe.query_vec, store,
                       t->led.detail, t->led.tier, t->led.gt_answer,
                       t->tools_used, t->tools_list, t->tools_list_n);
    free(masked);
    free(mquery);
  }
  asngn_cache_probe_free(&probe);

  /* stats */
  { asngn_ctx *stats_ctx=c->owner ? c->owner : c;
  os_rwlock_wrlock(&stats_ctx->lock);
  stats_ctx->stats.turns++;
  stats_ctx->stats.tokens_prompt += t->led.pt_system + t->led.pt_memory +
                            t->led.pt_catalog + t->led.pt_summary +
                            t->led.pt_verbatim + t->led.pt_working;
  stats_ctx->stats.tokens_gen +=
      t->led.gt_decision + t->led.gt_answer + t->led.gt_aux;
  stats_ctx->stats.tokens_saved += t->led.sv_cache + t->led.sv_digest;
  stats_ctx->stats.qpt_rolling = asngn_session_qpt(s);
  stats_ctx->stats.last_turn_at = (long long)t->led.at;
  os_rwlock_wrunlock(&stats_ctx->lock);
  }

  {
    char data[96];
    snprintf(data, sizeof data, "{tokens: %zu, capped: %s}",
             t->led.gt_answer, t->capped ? "true" : "false");
    asngn_tele_emit(c, "answer", t->span_root, NULL, s->slug,
                    t->led.turn, data);
  }
  return ASNGN_OK;
}
