/*
 * worker.c — the agent worker and the background worker.
 *
 * The agent worker drains a FIFO of submitted turns and runs each one
 * through asngn_loop_run; llama.cpp calls block on this thread with the
 * stall watchdog and the cancellation flag cascading into the backend.
 * The background worker owns folding, cache sweeps, telemetry flushes,
 * and the stall watchdog tick; it borrows model instances only while the
 * agent worker is idle, so folding never delays a turn.
 *
 * ASNGN_NO_THREADS builds run turns synchronously on the caller and do
 * background work inside asngn_tick.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

/* ── task execution ───────────────────────────────────────────────────── */

static void task_finish(asngn_ctx *c, asngn_task *task, asngn_err verdict) {
  asngn_turn_state *t = task->turn;
  asngn_session *s = task->session;

  (void)c;
  os_mutex_lock(&task->mu);
  task->verdict = verdict;
  memset(&task->result, 0, sizeof task->result);
  if (t != NULL) {
    task->result.answer = t->answer;
    t->answer = NULL;
    task->result.turn = t->led.turn;
    snprintf(task->result.klass, sizeof task->result.klass, "%s",
             t->led.klass);
    snprintf(task->result.detail, sizeof task->result.detail, "%s",
             t->led.detail);
    snprintf(task->result.tier, sizeof task->result.tier, "%s",
             t->led.tier);
    snprintf(task->result.cache, sizeof task->result.cache, "%s",
             t->led.cache);
    task->result.capped = t->capped ? 1 : 0;
    task->result.clarify = t->clarify ? 1 : 0;
    task->result.tokens_prompt = t->led.pt_system + t->led.pt_memory +
                                 t->led.pt_catalog + t->led.pt_summary +
                                 t->led.pt_verbatim + t->led.pt_working;
    task->result.tokens_gen =
        t->led.gt_decision + t->led.gt_answer + t->led.gt_aux;
    task->result.tokens_saved = t->led.sv_cache + t->led.sv_digest;
    task->result.duration_ms = t->led.duration_ms;
  }
  /* Publish session idleness before waking waiters.  Once task_wait returns,
   * the documented one-turn-per-session invariant must permit an immediate
   * follow-up submission. */
  if (s != NULL) {
    os_rwlock_wrlock(&s->lock);
    s->busy = false;
    s->running = NULL;
    os_rwlock_wrunlock(&s->lock);
  }
  task->done = true;
  os_cond_broadcast(&task->cv);
  os_mutex_unlock(&task->mu);
}

static void task_run(asngn_ctx *c, asngn_task *task) {
  asngn_err e = asngn_loop_run(c, task->turn);
  task_finish(c, task, e);
}

/* ── background maintenance ───────────────────────────────────────────── */

static bool any_session_busy(asngn_ctx *c) {
  size_t i;
  bool busy = false;
  os_rwlock_rdlock(&c->lock);
  for (i = 0; i < c->sessions_n; i++)
    if (c->sessions[i]->busy) busy = true;
  os_rwlock_rdunlock(&c->lock);
  return busy;
}

asngn_err asngn_run_due_work(asngn_ctx *c) {
  int64_t now = asngn_clock_mono_ms(&c->clock);
  asngn_session *fs = NULL;
  bool warm;

  /* one-shot model preload queued by asngn_open */
  os_mutex_lock(&c->q_mu);
  warm = c->bg_due_warm;
  c->bg_due_warm = false;
  os_mutex_unlock(&c->q_mu);
  if (warm) asngn_models_warm(c);

  /* queued fold: only while the agent worker is idle. The pending
   * pair is read under q_mu; the fold itself runs under a c->lock read
   * hold so asngn_session_close (which deregisters under the write lock)
   * cannot free the session mid-fold. */
  os_mutex_lock(&c->q_mu);
  if (c->bg_due_fold) fs = c->bg_fold_session;
  os_mutex_unlock(&c->q_mu);
  if (fs != NULL && !any_session_busy(c)) {
    bool live = false, claimed = false;
    size_t i;
    os_rwlock_rdlock(&c->lock);
    for (i = 0; i < c->sessions_n; i++)
      if (c->sessions[i] == fs) live = true;
    if (live) {
      /* Claim the session the way a turn does: fold must never hold
       * s->lock across the compressor call (minutes on CPU — the TUI
       * thread would block inside asngn_submit for the duration), so
       * exclusion is by s->busy and fold takes only short holds. */
      os_rwlock_wrlock(&fs->lock);
      if (!fs->busy) {
        fs->busy = true;
        fs->bg_folding = true;
        claimed = true;
      }
      os_rwlock_wrunlock(&fs->lock);
    }
    if (claimed) {
      size_t aux = 0;
      bool yielded;
      /* a stale yield from the last fold must not abort this one; a
       * shutdown-raised flag stays raised */
      os_mutex_lock(&c->q_mu);
      if (!c->stop) c->bg_cancel = 0;
      os_mutex_unlock(&c->q_mu);
      if (asngn_fold_needed(c, fs)) {
        asngn_err e = asngn_fold_run(c, fs, false, &aux);
        if (e != ASNGN_OK)
          asngn_log(c, ASNGN_LOG_WARN, "context", "%s: fold failed: %s",
                    fs->slug, asngn_err_name(e));
        else
          c->stats.folds++;
      }
      yielded = c->bg_cancel != 0;
      os_rwlock_wrlock(&fs->lock);
      fs->busy = false;
      fs->bg_folding = false;
      os_rwlock_wrunlock(&fs->lock);
      /* the fold's compress phases set the live "now" marker; nothing
       * follows them, so clear it explicitly */
      asngn_tele_emit(c, "phase", NULL, NULL, NULL, 0,
                      "{what: \"idle\"}");
      /* wake a submit_common parked on the fold */
      os_mutex_lock(&c->q_mu);
      os_cond_broadcast(&c->q_cv);
      /* a yielded fold stays queued and resumes once the session is
       * idle again; a completed one is done */
      if (!yielded && c->bg_fold_session == fs) {
        c->bg_due_fold = false;
        c->bg_fold_session = NULL;
      }
      os_mutex_unlock(&c->q_mu);
    }
    os_rwlock_rdunlock(&c->lock);
    if (!live) {
      os_mutex_lock(&c->q_mu);
      if (c->bg_fold_session == fs) {
        c->bg_due_fold = false;
        c->bg_fold_session = NULL;
      }
      os_mutex_unlock(&c->q_mu);
    }
  }

  /* daily cache sweep */
  if (c->last_sweep_ms == 0 || now - c->last_sweep_ms > 86400000) {
    c->last_sweep_ms = now;
    asngn_cache_sweep(c);
    os_rwlock_wrlock(&c->lock);
    c->stats.last_sweep_at = (long long)asngn_clock_now(&c->clock);
    os_rwlock_wrunlock(&c->lock);
  }

  asngn_tele_flush(c);
  return ASNGN_OK;
}

/* Stall watchdog tick: abort the in-flight model call when no
 * token has been produced for stall_timeout (2x before the first token,
 * which also covers prompt evaluation). Threaded builds only: without
 * workers the model call and the watchdog would share one thread. */
#ifndef ASNGN_NO_THREADS
static void stall_tick(asngn_ctx *c) {
  int64_t now, last, started, limit, silence = 0;
  bool stalled = false;
  /* the whole check-and-cancel runs under q_mu, the same mutex
   * watched_generate arms/disarms the watch under — a call that ends
   * concurrently can never be cancelled retroactively */
  os_mutex_lock(&c->q_mu);
  if (c->call_active) {
    now = asngn_clock_mono_ms(&c->clock);
    last = c->call_last_ms;
    started = c->call_started_ms;
    limit = c->cfg.stall_timeout_s * 1000;
    if (last <= started) limit *= 2; /* still evaluating the prompt */
    silence = now - (last > started ? last : started);
    if (silence > limit) {
      c->call_cancel = 1;
      stalled = true;
    }
    /* a cancelled turn also aborts its in-flight call */
    if (c->call_turn != NULL && c->call_turn->cancel) c->call_cancel = 1;
  }
  os_mutex_unlock(&c->q_mu);
  if (stalled)
    asngn_log(c, ASNGN_LOG_WARN, "model",
              "stall guard: model call aborted after %lld ms of silence",
              (long long)silence);
}
#endif /* !ASNGN_NO_THREADS */

/* ── threads ──────────────────────────────────────────────────────────── */

#ifndef ASNGN_NO_THREADS

static void *agent_main(void *arg) {
  asngn_ctx *c = arg;
  for (;;) {
    asngn_task *task;
    os_mutex_lock(&c->q_mu);
    while (!c->stop && c->q_head == NULL)
      os_cond_wait(&c->q_cv, &c->q_mu);
    if (c->stop && c->q_head == NULL) {
      os_mutex_unlock(&c->q_mu);
      break;
    }
    task = c->q_head;
    c->q_head = task->next;
    if (c->q_head == NULL) c->q_tail = NULL;
    os_mutex_unlock(&c->q_mu);
    task_run(c, task);
  }
  return NULL;
}

static void *bg_main(void *arg) {
  asngn_ctx *c = arg;
  for (;;) {
    os_mutex_lock(&c->q_mu);
    if (!c->stop) os_cond_timedwait(&c->q_cv, &c->q_mu, 1000);
    if (c->stop) {
      os_mutex_unlock(&c->q_mu);
      break;
    }
    os_mutex_unlock(&c->q_mu);
    stall_tick(c);
    asngn_run_due_work(c);
  }
  return NULL;
}

#endif /* !ASNGN_NO_THREADS */

asngn_err asngn_workers_start(asngn_ctx *c) {
#ifdef ASNGN_NO_THREADS
  c->no_threads = true;
  return ASNGN_OK;
#else
  asngn_err e = os_thread_start(&c->agent_thread, agent_main, c);
  if (e != ASNGN_OK)
    return asngn_seterr(c, e, "cannot start the agent worker");
  e = os_thread_start(&c->bg_thread, bg_main, c);
  if (e != ASNGN_OK) {
    os_mutex_lock(&c->q_mu);
    c->stop = true;
    os_cond_broadcast(&c->q_cv);
    os_mutex_unlock(&c->q_mu);
    os_thread_join(&c->agent_thread);
    c->stop = false;
    return asngn_seterr(c, e, "cannot start the background worker");
  }
  return ASNGN_OK;
#endif
}

void asngn_workers_stop(asngn_ctx *c) {
  asngn_task *task;
#ifdef ASNGN_NO_THREADS
  (void)task;
  asngn_run_due_work(c);
  return;
#else
  os_mutex_lock(&c->q_mu);
  c->stop = true;
  c->bg_cancel = 1; /* abort an in-flight background fold compressor */
  /* cancel anything still queued */
  for (task = c->q_head; task != NULL; task = task->next)
    if (task->turn != NULL) task->turn->cancel = 1;
  os_cond_broadcast(&c->q_cv);
  os_mutex_unlock(&c->q_mu);
  os_thread_join(&c->agent_thread);
  os_thread_join(&c->bg_thread);
  /* finish queued tasks that never ran */
  while (c->q_head != NULL) {
    task = c->q_head;
    c->q_head = task->next;
    task_finish(c, task, ASNGN_ERR_CANCELLED);
  }
  c->q_tail = NULL;
  asngn_tele_flush(c);
#endif
}

asngn_err asngn_worker_submit(asngn_ctx *c, asngn_task *task) {
#ifdef ASNGN_NO_THREADS
  task_run(c, task);
  asngn_run_due_work(c);
  return ASNGN_OK;
#else
  os_mutex_lock(&c->q_mu);
  if (c->stop) {
    os_mutex_unlock(&c->q_mu);
    return asngn_seterr(c, ASNGN_ERR_BUSY, "engine is shutting down");
  }
  task->next = NULL;
  if (c->q_tail != NULL) c->q_tail->next = task;
  else c->q_head = task;
  c->q_tail = task;
  os_cond_broadcast(&c->q_cv);
  os_mutex_unlock(&c->q_mu);
  return ASNGN_OK;
#endif
}

void asngn_background_kick(asngn_ctx *c) {
  os_mutex_lock(&c->q_mu);
  os_cond_broadcast(&c->q_cv);
  os_mutex_unlock(&c->q_mu);
}
